"""OpenAI Chat Completions 互換エンドポイント用のマルチモーダル・マルチレイヤー DrawingPlan Adapter。

思考モデル（OpenAI o1 / o3-mini、DeepSeek R1、Gemini Flash Thinking、Claude Extended Thinking、QwQ 等）
におけるパラメータ非互換、思考タグ混入、トークン枯渇による途切れを堅牢に解決する。
"""

from __future__ import annotations

import base64
from collections.abc import Callable, Mapping, Sequence
import contextlib
from dataclasses import dataclass
import datetime
import ipaddress
import json
import math
import re
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .domain import DrawingPlan, Stroke, StrokePoint
from .image_converter import MAX_ENCODED_IMAGE_BYTES
from .planner import validate_iterations, validate_plan_request
from .ports import PlannerPort


class LLMPlannerError(RuntimeError):
    """互換 API の通信、応答形式、または描画契約に関するエラー。"""


class _CrossOriginRedirectError(RuntimeError):
    pass


def _url_origin(url: str) -> tuple[str, str, int] | None:
    parsed = urlsplit(url)
    try:
        port = parsed.port
    except ValueError:
        return None
    if port is None:
        port = 443 if parsed.scheme.lower() == "https" else 80
    return parsed.scheme.lower(), (parsed.hostname or "").lower(), port


class _SameOriginRedirectHandler(HTTPRedirectHandler):
    def redirect_request(
        self,
        req: Request,
        fp: Any,
        code: int,
        msg: str,
        headers: Any,
        newurl: str,
    ) -> Request | None:
        redirected: Request | None = super().redirect_request(req, fp, code, msg, headers, newurl)
        source_origin = _url_origin(req.full_url)
        target_origin = _url_origin(redirected.full_url) if redirected is not None else None
        if redirected is not None and (source_origin is None or source_origin != target_origin):
            raise _CrossOriginRedirectError
        return redirected


def _is_reasoning_model(model_name: str) -> bool:
    """モデル名から思考・推論（Reasoning / Thinking）モデルかどうかを判定する。"""
    name = model_name.lower().strip()
    return any(
        kw in name
        for kw in (
            "o1",
            "o3",
            "o4",
            "deepseek-r1",
            "deepseek-reasoner",
            "r1",
            "qwq",
            "thinking",
            "reasoning",
            "reasoner",
            "sonnet-3-7",
            "claude-3-7",
            "gemini-2.0-flash-thinking",
            "gemini-2.5",
        )
    )


def _detect_image_mime_type(data: bytes) -> str:
    """バイトヘッダー（マジックナンバー）から画像 MIME タイプを判定する。"""
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    if data.startswith(b"RIFF") and len(data) >= 12 and data[8:12] == b"WEBP":
        return "image/webp"
    if data.startswith((b"GIF87a", b"GIF89a")):
        return "image/gif"
    if data.startswith(b"BM"):
        return "image/bmp"
    return "image/png"


def _is_loopback_host(hostname: str | None) -> bool:
    if not hostname:
        return False
    host = hostname.strip().lower().rstrip(".")
    if host == "localhost" or host.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _endpoint_origin_label(url: str) -> str:
    """ログへ URL パス上のトークンを出さず、接続先オリジンだけを返す。"""
    parsed = urlsplit(url)
    host = parsed.hostname or "invalid-host"
    display_host = f"[{host}]" if ":" in host else host
    try:
        port = parsed.port
    except ValueError:
        port = None
    default_port = 443 if parsed.scheme.lower() == "https" else 80
    port_suffix = f":{port}" if port is not None and port != default_port else ""
    return f"{parsed.scheme.lower()}://{display_host}{port_suffix}"


@dataclass(frozen=True)
class OpenAICompatibleSettings:
    """キーを永続化しない、1 回の API 呼び出しに必要な接続設定。"""

    base_url: str
    model: str
    api_key: str = ""
    timeout_seconds: float = 120.0
    max_tokens: int = 8192
    reasoning_effort: str = "low"
    temperature: float = 0.7
    top_p: float = 1.0
    custom_system_prompt: str = ""
    vision_resolution: int = 512
    max_retries: int = 3
    fallback_to_procedural: bool = False

    def __post_init__(self) -> None:
        if not isinstance(self.base_url, str) or not self.base_url.strip():
            raise ValueError("Base URL を入力してください")
        if len(self.base_url) > 2_048:
            raise ValueError("Base URL が長すぎます")
        if any(ord(char) < 32 for char in self.base_url):
            raise ValueError("Base URL に制御文字を含めないでください")
        if not isinstance(self.api_key, str):
            raise ValueError("API Key は文字列である必要があります")
        if len(self.api_key) > 4_096:
            raise ValueError("API Key が長すぎます")
        if "\r" in self.api_key or "\n" in self.api_key:
            raise ValueError("API Key に改行を含めないでください")
        parsed = urlsplit(self.base_url.strip())
        try:
            parsed_port = parsed.port
        except ValueError as exc:
            raise ValueError("Base URL のポート番号が不正です") from exc
        del parsed_port
        if (
            parsed.scheme not in {"http", "https"}
            or not parsed.netloc
            or not parsed.hostname
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("Base URL はクエリを含まない http(s) URL にしてください")
        if parsed.username is not None or parsed.password is not None:
            raise ValueError("Base URL にユーザー名やパスワードを含めないでください")
        if parsed.scheme == "http" and not _is_loopback_host(parsed.hostname):
            raise ValueError("非ローカル接続には https:// の Base URL が必要です")
        if not isinstance(self.model, str) or not self.model.strip():
            raise ValueError("Model を入力してください")
        if len(self.model) > 256:
            raise ValueError("Model 名が長すぎます")
        if (
            isinstance(self.timeout_seconds, bool)
            or not isinstance(self.timeout_seconds, (int, float))
            or self.timeout_seconds <= 0
            or self.timeout_seconds > 3_600
            or not math.isfinite(self.timeout_seconds)
        ):
            raise ValueError("timeout_seconds は 3,600 秒以下の正の有限数値である必要があります")
        if (
            isinstance(self.max_tokens, bool)
            or not isinstance(self.max_tokens, int)
            or not 1 <= self.max_tokens <= 1_000_000
        ):
            raise ValueError("max_tokens は 1 から 1,000,000 の整数である必要があります")
        if not isinstance(self.reasoning_effort, str):
            raise ValueError("reasoning_effort は文字列である必要があります")
        if (
            isinstance(self.temperature, bool)
            or not isinstance(self.temperature, (int, float))
            or not (0.0 <= self.temperature <= 2.0)
            or not math.isfinite(self.temperature)
        ):
            raise ValueError("temperature は 0.0 から 2.0 の範囲である必要があります")
        if (
            isinstance(self.top_p, bool)
            or not isinstance(self.top_p, (int, float))
            or not (0.0 < self.top_p <= 1.0)
            or not math.isfinite(self.top_p)
        ):
            raise ValueError("top_p は 0.0 より大きく 1.0 以下の範囲である必要があります")
        if not isinstance(self.custom_system_prompt, str):
            raise ValueError("custom_system_prompt は文字列である必要があります")
        if len(self.custom_system_prompt) > 20_000:
            raise ValueError("custom_system_prompt は 20,000 文字以下である必要があります")
        if (
            isinstance(self.vision_resolution, bool)
            or not isinstance(self.vision_resolution, int)
            or not 64 <= self.vision_resolution <= 4_096
        ):
            raise ValueError("vision_resolution は 64 から 4,096 の整数である必要があります")
        if (
            isinstance(self.max_retries, bool)
            or not isinstance(self.max_retries, int)
            or not 1 <= self.max_retries <= 10
        ):
            raise ValueError("max_retries は 1 から 10 の整数である必要があります")
        if not isinstance(self.fallback_to_procedural, bool):
            raise ValueError("fallback_to_procedural は真偽値である必要があります")

    @property
    def endpoint_url(self) -> str:
        """Base URL から OpenAI 互換の chat/completions エンドポイント URL をスマートに導出する。"""
        base = self.base_url.strip().rstrip("/")
        if base.endswith("/chat/completions"):
            return base
        if base.endswith("/v1"):
            return f"{base}/chat/completions"
        # Ollama, LM Studio, OpenAI などの主要ホストで /v1 が省略されている場合は自動補完
        host = (urlsplit(base).hostname or "").lower().rstrip(".")
        known_hosts = ("api.openai.com", "groq.com", "openrouter.ai", "deepseek.com")
        if _is_loopback_host(host) or any(host == known or host.endswith(f".{known}") for known in known_hosts):
            return f"{base}/v1/chat/completions"
        return f"{base}/chat/completions"


class OpenAICompatiblePlanner(PlannerPort):
    """OpenAI 互換の ``POST /chat/completions`` をマルチモーダル & 自律ビジョン Planner として利用する。"""

    MAX_RESPONSE_BYTES = 25_000_000

    def __init__(
        self,
        settings: OpenAICompatibleSettings,
        opener: Callable[..., Any] | None = None,
        log_callback: Callable[[str], None] | None = None,
    ) -> None:
        self.settings = settings
        self._opener = opener or build_opener(_SameOriginRedirectHandler()).open
        self.log_callback = log_callback
        self._conversation_history: list[dict[str, Any]] = []

    def _log(self, message: str) -> None:
        if self.log_callback is not None:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            self.log_callback(f"[{ts}] {message}")

    def test_connection(self) -> str:
        """API 接続疎通確認を行う。思考モデルのパラメータ特性にも適応。"""
        is_reasoning = _is_reasoning_model(self.settings.model)
        self._log(
            f"API 接続テスト開始: {_endpoint_origin_label(self.settings.endpoint_url)} (Model: {self.settings.model}, 思考モデル判定: {is_reasoning}, ReasoningEffort: {self.settings.reasoning_effort})"
        )

        test_max_tokens = min(max(self.settings.max_tokens, 16), 2048)
        payload: dict[str, Any] = {
            "model": self.settings.model.strip(),
            "messages": [{"role": "user", "content": "Respond with 'OK'."}],
        }
        if is_reasoning:
            payload["max_completion_tokens"] = test_max_tokens
            if self.settings.reasoning_effort and self.settings.reasoning_effort.lower() not in ("none", ""):
                payload["reasoning_effort"] = self.settings.reasoning_effort.lower()
        else:
            payload["max_tokens"] = test_max_tokens

        start = time.perf_counter()
        response = self._post_with_parameter_fallback(payload)
        elapsed = time.perf_counter() - start

        try:
            plan_or_content = _extract_best_content_or_plan(response, log_func=self._log, is_drawing_plan=False)
            preview = str(plan_or_content)[:60].replace("\n", " ")
            msg = f"接続成功: モデルが正常に応答しました ({elapsed:.2f}s, 応答: {preview!r})"
            self._log(msg)
            return msg
        except Exception as exc:
            msg = f"接続確認完了 (警告): HTTP 200 を受信しましたが応答の解釈に失敗しました ({elapsed:.2f}s): {exc}"
            self._log(msg)
            return msg

    def plan(
        self,
        prompt: str,
        seed: int,
        count: int | None = None,
        width: float = 1000.0,
        height: float = 1000.0,
        image_data: bytes | None = None,
        canvas_image: bytes | None = None,
        iteration: int = 1,
        max_iterations: int = 1,
        palette_name: str = "anime",
        auto_count: bool = False,
        goal_mode: bool = False,
        **kwargs: Any,
    ) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height, auto_count=auto_count
        )
        iteration, max_iterations = validate_iterations(iteration, max_iterations)
        for image_name, image_value in (("image_data", image_data), ("canvas_image", canvas_image)):
            if image_value is not None and not isinstance(image_value, bytes):
                raise ValueError(f"{image_name} は bytes または None である必要があります")
            if image_value is not None and len(image_value) > MAX_ENCODED_IMAGE_BYTES:
                raise ValueError(f"{image_name} のサイズが上限を超えています")
        cancelled = kwargs.get("cancelled")

        def raise_if_cancelled() -> None:
            if callable(cancelled) and cancelled():
                raise LLMPlannerError("LLM 描画計画の生成をキャンセルしました")

        if iteration == 1:
            self._conversation_history = []

        is_reasoning = _is_reasoning_model(self.settings.model)
        count_display = f"{valid_count}" if valid_count is not None else "Auto (AI自律・無制限)"
        self._log(
            f"--- 描画計画生成開始 (Step {iteration}/{max_iterations}) ---\n"
            f"Prompt: {valid_prompt!r}, Seed: {valid_seed}, Count: {count_display}, GoalMode: {goal_mode}, Canvas: {valid_width}x{valid_height}\n"
            f"Endpoint: {_endpoint_origin_label(self.settings.endpoint_url)}, Model: {self.settings.model} (思考モデル最適化: {is_reasoning}, ReasoningEffort: {self.settings.reasoning_effort}), Timeout: {self.settings.timeout_seconds}s"
        )

        # 段階的ステップ描画時のフェーズ目標の導出
        phase_goal = ""
        if max_iterations > 1 or goal_mode:
            if max_iterations == 2:
                phase_goal = (
                    "Phase 1/2: Base Color Blocking, Environment & Initial Shadows (Flats/Shading layer)"
                    if iteration == 1
                    else "Phase 2/2 [FINAL]: Structural Lineart, Highlights, Petals & Final Polish (Lineart/Highlights/FX)"
                )
            elif max_iterations == 3:
                if iteration == 1:
                    phase_goal = "Phase 1/3: Base Color Masses & Silhouettes (Flats layer)"
                elif iteration == 2:
                    phase_goal = "Phase 2/3: 3D Form Sculpting, Ambient Occlusion & Shadows (Shading layer)"
                else:
                    phase_goal = (
                        "Phase 3/3 [FINAL]: Crisp Lineart, Highlights, Petal Scatter & Polish (Lineart/Highlights/FX)"
                    )
            else:
                if iteration == 1:
                    phase_goal = f"Phase {iteration}/{max_iterations}: Base Color Blocking & Foundations (Flats layer)"
                elif iteration == 2:
                    phase_goal = f"Phase {iteration}/{max_iterations}: 3D Volume Sculpting & Secondary Masses (Flats/Shading layer)"
                elif iteration < max_iterations:
                    phase_goal = f"Phase {iteration}/{max_iterations}: Shadow Crevices, Eraser Refinements & Structural Contours (Shading/Lineart layer)"
                else:
                    phase_goal = f"Phase {iteration}/{max_iterations} [FINAL]: Fine Lineart, Eraser Carving, Highlights, Petals & Polish (Lineart/Highlights/FX)"

        user_content_parts: list[dict[str, Any]] = []

        # 構造化テキスト指示
        req_stroke_count: Any = (
            valid_count
            if valid_count is not None
            else "auto (unconstrained - freely generate as many strokes as necessary to completely fulfill the prompt)"
        )
        req_dict: dict[str, Any] = {
            "prompt": valid_prompt,
            "seed": valid_seed,
            "stroke_count": req_stroke_count,
            "canvas": {"width": valid_width, "height": valid_height},
            "palette": palette_name,
            "iteration": iteration,
            "max_iterations": max_iterations,
            "goal_mode": goal_mode,
            "phase_goal": phase_goal or "Complete full professional illustration.",
            "instruction": (
                (
                    "Visually inspect the attached current canvas, identify what is missing or flawed, and fix/enrich it using strokes or eraser strokes (is_eraser: true). "
                    if canvas_image
                    else ""
                )
                + f"Generate drawing strokes for {phase_goal or 'the artwork'} strictly in valid DrawingPlan JSON format. "
                "Evaluate completion with 'goal_reached': boolean (true if finished, false if more work needed) and 'completion_score': float (0.0-1.0). "
                "Keep request_canvas_image false; multi-step runs receive automatic canvas feedback."
            ),
        }
        req_json = json.dumps(req_dict, ensure_ascii=False)
        user_content_parts.append({"type": "text", "text": req_json})

        # 参照画像 (Base64) の添付
        if image_data:
            mime_type = _detect_image_mime_type(image_data)
            self._log(f"参照画像を添付します ({len(image_data)} bytes, MIME: {mime_type})")
            b64_ref = base64.b64encode(image_data).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:{mime_type};base64,{b64_ref}", "detail": "high"},
                }
            )

        # Auto-Refine の前ステップで取得したキャンバスキャプチャ (Base64) を添付
        if canvas_image:
            canvas_mime = _detect_image_mime_type(canvas_image)
            self._log(f"現在のキャンバス状態を視覚評価用に添付します ({len(canvas_image)} bytes, MIME: {canvas_mime})")
            b64_canvas = base64.b64encode(canvas_image).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:{canvas_mime};base64,{b64_canvas}", "detail": "high"},
                }
            )
        elif max_iterations > 1 and iteration > 1:
            self._log("警告: Auto-Refine のキャンバス画像が取得できなかったため、テキスト情報のみで続行します")

        user_content: Any = user_content_parts if len(user_content_parts) > 1 else req_json

        # メッセージ履歴（マルチターン会話文脈）の構築
        system_content = _system_instruction(
            iteration=iteration,
            max_iterations=max_iterations,
            is_reasoning=is_reasoning,
            width=valid_width,
            height=valid_height,
            prompt=valid_prompt,
            palette_name=palette_name,
        )
        if self.settings.custom_system_prompt.strip():
            system_content += f"\n\n[USER CUSTOM INSTRUCTIONS]\n{self.settings.custom_system_prompt.strip()}"

        base_messages: list[dict[str, Any]] = [{"role": "system", "content": system_content}]
        if self._conversation_history and iteration > 1:
            base_messages.extend(self._conversation_history)
        base_messages.append({"role": "user", "content": user_content})

        payload: dict[str, Any] = {
            "model": self.settings.model.strip(),
            "messages": base_messages,
            "response_format": {"type": "json_object"},
        }

        if is_reasoning:
            # 思考モデル (o1/o3/r1 等) は max_completion_tokens を使用し、temperature は除外
            payload["max_completion_tokens"] = self.settings.max_tokens
            if self.settings.reasoning_effort and self.settings.reasoning_effort.lower() not in ("none", ""):
                payload["reasoning_effort"] = self.settings.reasoning_effort.lower()
        else:
            payload["max_tokens"] = self.settings.max_tokens
            payload["temperature"] = float(self.settings.temperature)
            if self.settings.top_p < 1.0:
                payload["top_p"] = float(self.settings.top_p)

        self._log(f"LLM API へリクエスト送信中 ({_endpoint_origin_label(self.settings.endpoint_url)})...")

        # 自動リカバリー付き計画生成ループ (設定回数)
        max_attempts = max(1, self.settings.max_retries)
        last_error: Exception | None = None

        for attempt in range(1, max_attempts + 1):
            raise_if_cancelled()
            if attempt == 1:
                current_payload = dict(payload)
            elif attempt == 2:
                self._log("[自動リトライ 1/2] response_format を除外し、直接確定 JSON 出力指定で再試行します...")
                current_payload = dict(payload)
                current_payload.pop("response_format", None)
                current_payload.pop("reasoning_effort", None)
                if not is_reasoning:
                    current_payload["temperature"] = 0.2

                retry_sys = (
                    _system_instruction(
                        iteration=iteration,
                        max_iterations=max_iterations,
                        is_reasoning=True,
                        width=valid_width,
                        height=valid_height,
                        prompt=valid_prompt,
                        palette_name=palette_name,
                    )
                    + "\nIMPORTANT: Output ONLY the raw JSON starting immediately with ```json. Do NOT write any reasoning text or preamble."
                )
                r_messages: list[dict[str, Any]] = [{"role": "system", "content": retry_sys}]
                if self._conversation_history and iteration > 1:
                    r_messages.extend(self._conversation_history)
                r_messages.append({"role": "user", "content": user_content})
                current_payload["messages"] = r_messages
            else:
                self._log("[自動リトライ 2/2] 思考抑制・最小構造モードで再試行します...")
                current_payload = dict(payload)
                current_payload.pop("response_format", None)
                current_payload.pop("reasoning_effort", None)
                if not is_reasoning:
                    current_payload["temperature"] = 0.0

                min_sys = (
                    'You must output ONLY valid JSON matching DrawingPlan schema. Start output directly with {"schema_version": 1. '
                    "Include request_canvas_image: false. No thoughts, no analysis."
                )
                r_messages_min: list[dict[str, Any]] = [{"role": "system", "content": min_sys}]
                if self._conversation_history and iteration > 1:
                    r_messages_min.extend(self._conversation_history)
                r_messages_min.append({"role": "user", "content": user_content})
                current_payload["messages"] = r_messages_min

            try:
                response = self._post_with_parameter_fallback(current_payload, cancelled=cancelled)
                self._log("LLM API 応答受信。思考タグ解析・JSON パース・ストローク救済を実行中...")
                plan = _plan_from_response(
                    response,
                    prompt=valid_prompt,
                    seed=valid_seed,
                    width=valid_width,
                    height=valid_height,
                    log_func=self._log,
                )
                sanitized_plan = _validate_and_sanitize_plan(
                    plan=plan,
                    prompt=valid_prompt,
                    seed=valid_seed,
                    count=valid_count,
                    width=valid_width,
                    height=valid_height,
                    iteration=iteration,
                    log_func=self._log,
                )
                total_pts = sum(len(s.points) for s in sanitized_plan.strokes)
                layers_str = ", ".join(sanitized_plan.layers)
                self._log(
                    f"描画計画生成成功: ストローク数={len(sanitized_plan.strokes)}, 総点数={total_pts}, レイヤー=[{layers_str}], "
                    f"互換フィールド(request_canvas_image)={sanitized_plan.request_canvas_image}"
                )

                # 会話履歴に記録（次ステップへの文脈継承）
                if max_iterations > 1:
                    summary_dict = {
                        "schema_version": 1,
                        "iteration": iteration,
                        "completed_layers": list(sanitized_plan.layers),
                        "stroke_count": len(sanitized_plan.strokes),
                        "request_canvas_image": sanitized_plan.request_canvas_image,
                        "strokes_summary": [
                            {
                                "id": s.id,
                                "layer": s.layer_name,
                                "color": s.color,
                                "size_px": s.size_px,
                                "start_xy": [round(s.points[0].x, 1), round(s.points[0].y, 1)],
                                "end_xy": [round(s.points[-1].x, 1), round(s.points[-1].y, 1)],
                            }
                            for s in sanitized_plan.strokes[:15]
                        ],
                    }
                    # 画像を履歴へ複製すると、反復ごとに Base64 ペイロードが累積する。
                    # 次のキャンバス画像は常に最新リクエストへ個別添付し、履歴はテキスト要約だけ保持する。
                    self._conversation_history.append({"role": "user", "content": req_json})
                    self._conversation_history.append(
                        {"role": "assistant", "content": json.dumps(summary_dict, ensure_ascii=False)}
                    )
                    if len(self._conversation_history) > 8:
                        self._conversation_history = self._conversation_history[-8:]

                return sanitized_plan

            except LLMPlannerError as exc:
                last_error = exc
                err_str = str(exc)
                # 致命的な認証エラー（401/403/404 等）はリトライせず即座に例外を上げる
                if any(
                    code in err_str
                    for code in ("HTTP 401", "HTTP 403", "HTTP 404", "認証", "API key", "invalid_api_key")
                ):
                    raise
                if attempt < max_attempts:
                    self._log(f"警告: 試行 {attempt}/{max_attempts} でエラーが発生しました: {exc}")
                    for _ in range(5):
                        raise_if_cancelled()
                        time.sleep(0.1)
                else:
                    self._log(f"エラー: 全 {max_attempts} 回の試行が失敗しました: {exc}")

        if not self.settings.fallback_to_procedural:
            if last_error is not None:
                raise last_error
            raise LLMPlannerError("LLM 描画計画の生成に失敗しました")

        # 利用者が明示的に許可した場合だけ、プロシージャル生成へフォールバックする。
        self._log(
            "通知: LLM 思考トークン枯渇または抽出不能のため、プロシージャルエンジンによる緊急フォールバック描画計画を自動生成します。"
        )
        try:
            from .procedural import generate_procedural_plan

            fallback_plan = generate_procedural_plan(
                prompt=valid_prompt,
                seed=valid_seed,
                count=valid_count,
                width=valid_width,
                height=valid_height,
                palette_name=palette_name,
            )
            self._log(
                f"緊急救済成功: プロシージャル描画計画を生成しました (ストローク数: {len(fallback_plan.strokes)})"
            )
            return DrawingPlan(
                prompt=fallback_plan.prompt,
                seed=fallback_plan.seed,
                strokes=fallback_plan.strokes,
                title=fallback_plan.title,
                iteration=iteration,
                layers=fallback_plan.layers,
                metadata={
                    **dict(fallback_plan.metadata),
                    "planner_fallback": "procedural",
                    "fallback_reason": str(last_error or "LLM response could not be parsed")[:500],
                },
                canvas_width=valid_width,
                canvas_height=valid_height,
            )
        except Exception as fb_exc:
            if last_error is not None:
                raise last_error from fb_exc
            raise LLMPlannerError(f"LLM 描画計画の生成に失敗しました: {last_error or fb_exc}") from fb_exc

    def _post_with_parameter_fallback(
        self,
        payload: dict[str, Any],
        cancelled: Callable[[], bool] | None = None,
    ) -> Mapping[str, Any]:
        """400/422 のパラメータ非互換エラー（temperature, max_tokens, response_format, reasoning_effort 等）を自動検知・パージして再試行する。"""
        current_payload = dict(payload)
        max_param_retries = 4

        for p_attempt in range(max_param_retries):
            if cancelled is not None and cancelled():
                raise LLMPlannerError("LLM API リクエストをキャンセルしました")
            try:
                return self._post(current_payload)
            except LLMPlannerError as exc:
                err_text = str(exc).lower()
                modified = False

                # 1. reasoning_effort 非対応エラーの自動パージ
                if (
                    "reasoning_effort" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "extra_forbidden"))
                    and "reasoning_effort" in current_payload
                ):
                    self._log("[パラメータ自動適応] モデルが reasoning_effort をサポートしていないため除外します")
                    current_payload.pop("reasoning_effort", None)
                    modified = True

                # 2. temperature 非対応エラーの自動パージ
                if (
                    "temperature" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "extra_forbidden"))
                    and "temperature" in current_payload
                ):
                    self._log("[パラメータ自動適応] モデルが temperature をサポートしていないため除外します")
                    current_payload.pop("temperature", None)
                    modified = True

                # 3. max_tokens -> max_completion_tokens への自動変換
                if (
                    "max_tokens" in err_text
                    and any(kw in err_text for kw in ("max_completion_tokens", "unsupported", "not support"))
                    and "max_tokens" in current_payload
                ):
                    self._log("[パラメータ自動適応] max_tokens を max_completion_tokens に変換します")
                    val = current_payload.pop("max_tokens")
                    current_payload["max_completion_tokens"] = val
                    modified = True

                # 4. max_completion_tokens -> max_tokens への逆変換 (旧型互換サーバー対応)
                if (
                    "max_completion_tokens" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "extra_forbidden"))
                    and "max_completion_tokens" in current_payload
                ):
                    self._log("[パラメータ自動適応] max_completion_tokens を max_tokens に変換します")
                    val = current_payload.pop("max_completion_tokens")
                    current_payload["max_tokens"] = val
                    modified = True

                # 5. response_format 非対応エラーの自動パージ
                if (
                    "response_format" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "schema", "json_object"))
                    and "response_format" in current_payload
                ):
                    self._log("[パラメータ自動適応] モデルが response_format をサポートしていないため除外します")
                    current_payload.pop("response_format", None)
                    modified = True

                # 6. system ロール非対応エラーの developer / user ロール統合
                if "system" in err_text and (
                    "role" in err_text or "developer" in err_text or "not support" in err_text
                ):
                    messages = current_payload.get("messages", [])
                    if messages and messages[0].get("role") == "system":
                        self._log("[パラメータ自動適応] system ロールを user プロンプトに統合します")
                        sys_content = messages[0].get("content", "")
                        new_messages = []
                        for m in messages[1:]:
                            if m.get("role") == "user":
                                u_content = m.get("content")
                                if isinstance(u_content, list):
                                    new_parts: list[dict[str, Any]] = []
                                    text_merged = False
                                    for part in u_content:
                                        if isinstance(part, Mapping) and part.get("type") == "text":
                                            orig_text = part.get("text", "")
                                            new_parts.append(
                                                {
                                                    "type": "text",
                                                    "text": f"{sys_content}\n\n[USER REQUEST]\n{orig_text}",
                                                }
                                            )
                                            text_merged = True
                                        elif isinstance(part, Mapping):
                                            new_parts.append(dict(part))
                                        else:
                                            new_parts.append(part)
                                    if not text_merged:
                                        new_parts.insert(0, {"type": "text", "text": str(sys_content)})
                                    new_messages.append({"role": "user", "content": new_parts})
                                else:
                                    combined_content = f"{sys_content}\n\n[USER REQUEST]\n{u_content}"
                                    new_messages.append({"role": "user", "content": combined_content})
                            else:
                                new_messages.append(m)
                        if not new_messages:
                            new_messages.append({"role": "user", "content": str(sys_content)})
                        current_payload["messages"] = new_messages
                        modified = True

                if modified and p_attempt < max_param_retries - 1:
                    self._log(f"[パラメータ自動適応] 修正済みリクエストで再送します (試行 {p_attempt + 1})...")
                    continue

                raise

        return self._post(current_payload)

    def _post(self, payload: Mapping[str, Any]) -> Mapping[str, Any]:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "AIStrokePainter/1.1.0",
        }
        if self.settings.api_key.strip():
            headers["Authorization"] = f"Bearer {self.settings.api_key.strip()}"

        post_data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        request = Request(
            self.settings.endpoint_url,
            data=post_data,
            headers=headers,
            method="POST",
        )

        start_time = time.perf_counter()
        try:
            with self._opener(request, timeout=self.settings.timeout_seconds) as response:
                status_code = getattr(response, "status", getattr(response, "code", 200))
                raw = response.read(self.MAX_RESPONSE_BYTES + 1)
        except _CrossOriginRedirectError as exc:
            self._log("エラー: 別オリジンへのリダイレクト拒否")
            raise LLMPlannerError("LLM API の別オリジンへのリダイレクトを拒否しました") from exc
        except HTTPError as exc:
            detail = _read_http_error(exc)
            elapsed = time.perf_counter() - start_time
            err_msg = f"LLM API が HTTP {exc.code} を返しました ({elapsed:.2f}s): {detail}"
            self._log(f"エラー: {err_msg}")
            raise LLMPlannerError(err_msg) from exc
        except (URLError, TimeoutError, OSError) as exc:
            elapsed = time.perf_counter() - start_time
            err_msg = f"LLM API に接続できませんでした ({elapsed:.2f}s): {_safe_error_message(exc)}"
            self._log(f"エラー: {err_msg}")
            raise LLMPlannerError(err_msg) from exc

        elapsed = time.perf_counter() - start_time
        self._log(f"HTTP {status_code} 受信完了 ({elapsed:.2f}s, {len(raw)} bytes)")

        if len(raw) > self.MAX_RESPONSE_BYTES:
            raise LLMPlannerError(f"LLM API の応答が上限 ({self.MAX_RESPONSE_BYTES} bytes) を超えています")

        try:
            decoded_text = raw.decode("utf-8", errors="replace")
            decoded = json.loads(decoded_text)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            preview = _redact_sensitive_text(raw[:500].decode("utf-8", errors="replace"))
            self._log(f"JSON デコードエラー: {exc}\n応答先頭 500 文字: {preview!r}")
            raise LLMPlannerError("LLM API が JSON 応答を返しませんでした") from exc

        if not isinstance(decoded, Mapping):
            raise LLMPlannerError("LLM API の応答は JSON オブジェクトである必要があります")

        # デバッグモード用: レスポンス詳細メタデータおよびコンテンツのログ出力
        resp_model = decoded.get("model", self.settings.model)
        usage = decoded.get("usage")
        if isinstance(usage, Mapping):
            p_tok = usage.get("prompt_tokens", "?")
            c_tok = usage.get("completion_tokens", "?")
            t_tok = usage.get("total_tokens", "?")
            details = usage.get("completion_tokens_details") or {}
            reasoning_tok = details.get("reasoning_tokens") if isinstance(details, Mapping) else None
            reasoning_info = f", 思考推論: {reasoning_tok}" if reasoning_tok is not None else ""
            self._log(
                f"[トークン消費] Prompt: {p_tok}, Completion: {c_tok}{reasoning_info}, Total: {t_tok} (Model: {resp_model})"
            )

        # choices 内部の詳細ログ
        choices = decoded.get("choices")
        if isinstance(choices, list) and choices and isinstance(choices[0], Mapping):
            c0 = choices[0]
            f_reason = c0.get("finish_reason", "unknown")
            self._log(f"[LLM 応答状態] finish_reason: {f_reason}")
            msg = c0.get("message", {})
            if isinstance(msg, Mapping):
                # 思考プロセスのログ
                reasoning = msg.get("reasoning_content") or msg.get("reasoning") or msg.get("thought")
                if isinstance(reasoning, str) and reasoning.strip():
                    r_preview = _redact_sensitive_text(reasoning.strip()[:300].replace("\n", " ")) + (
                        "..." if len(reasoning) > 300 else ""
                    )
                    self._log(f"[思考プロセス (reasoning)] {len(reasoning)} 文字: {r_preview}")

                # 本文 content のプレビュー
                content_val = msg.get("content")
                if isinstance(content_val, str) and content_val.strip():
                    c_lines = content_val.strip().splitlines()
                    head_lines = _redact_sensitive_text("\n".join(c_lines[:6]))
                    tail_preview = (
                        (
                            "\n... [中略 "
                            + str(len(c_lines) - 10)
                            + " 行] ...\n"
                            + _redact_sensitive_text("\n".join(c_lines[-4:]))
                        )
                        if len(c_lines) > 10
                        else ""
                    )
                    self._log(
                        f"[LLM 応答本文プレビュー ({len(content_val)} 文字, {len(c_lines)} 行)]:\n{head_lines}{tail_preview}"
                    )

        return decoded


def _system_instruction(
    iteration: int = 1,
    max_iterations: int = 1,
    is_reasoning: bool = False,
    width: float = 1000.0,
    height: float = 1000.0,
    prompt: str = "",
    palette_name: str = "anime",
) -> str:
    """プロフェッショナルなデジタルイラスト作画戦略・レイヤー階層・キャンバス連動ブラシサイズ・筆圧ダイナミクスを含む高品質プロンプト。"""
    min_dim = min(width, height)

    # 推奨ブラシサイズ（大面積塗り用から極細ディテール用まで多層定義）
    flats_sz = f"{max(40, round(min_dim * 0.08))} to {max(150, round(min_dim * 0.20))} px"
    form_shad_sz = f"{max(25, round(min_dim * 0.03))} to {max(60, round(min_dim * 0.06))} px"
    detail_shad_sz = "8 to 22 px"
    main_line_sz = "3.5 to 6.0 px"
    detail_line_sz = "1.5 to 3.0 px (CRITICAL for eyes, lashes, double eyelids, nose tip, lips, hair tips)"
    hl_glint_sz = "2.0 to 3.5 px (eye specular glints) / 6 to 16 px (hair halo & rim light)"

    # ドメイン別作画ガイダンス
    prompt_lower = prompt.lower()
    domain_guidance = ""

    if any(
        k in prompt_lower
        for k in [
            "sakura",
            "桜",
            "landscape",
            "mountain",
            "山",
            "cloud",
            "雲",
            "nature",
            "風景",
            "forest",
            "tree",
            "river",
            "lake",
            "ocean",
            "sea",
            "wave",
        ]
    ):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Landscape, Mountains, Clouds & Sakura Trees]\n"
            "1. Layer 'Flats' (Complete Seamless Coverage & Base Volumes):\n"
            f"   - Sky Gradient: Paint multiple dense overlapping horizontal sweep strokes (size_px: {flats_sz}, brush: 'Airbrush Soft' or 'Basic-5 Size') "
            "from deep blue zenith (#2b5c8f) down to clear sky (#5c93cf), horizon haze (#b8d8f8), and soft white (#eef6ff). Leave NO white canvas gaps.\n"
            f"   - Distant & Midground Mountains: Paint sweeping mountain silhouettes (size_px: {flats_sz}, brush: 'Basic-5 Size'). "
            "Use atmospheric perspective (distant peaks in soft blue-gray #6f829d, nearer peaks in deep pine/slate #283e50).\n"
            f"   - Rolling Hills & Ground: Dense green terrain wash (#4e7d58, #72a37c, #9ec4a5) with organic curving strokes.\n"
            f"   - Sakura Blossom Canopy Clumps: Paint rich, billowing clouds of pink foliage masses (size_px: {flats_sz}, brush: 'Wet Textured Soft' or 'Basic-5 Size') "
            "arranged above and around the branches (colors: #ff9ebb, #ffb8cd, #ffd6e5). Overlap multiple puffy clusters to create huge 3D volume.\n"
            "2. Layer 'Shading' (3D Depth, Occlusion & Mountain Crags):\n"
            f"   - Mountain Ridges & Shadow Facets: Carve dramatic shadow slopes along mountain ridge lines (size_px: {form_shad_sz}, opacity: 0.6-0.8, brush: 'Dry Bristles' or 'Basic-5 Size', colors: #1a2733, #223445).\n"
            f"   - Cloud Undersides: Paint soft purplish shadow bulges under cloud masses (size_px: {form_shad_sz}, opacity: 0.5-0.7, colors: #92a4bc, #7b8ea7).\n"
            f"   - Blossom Canopy Deep Shadows: Paint deep magenta/purple-pink core shadows underneath blossom clusters (size_px: {detail_shad_sz}, colors: #a3436a, #842f53).\n"
            "3. Layer 'Lineart' (Organic Tree Anatomy & Crisp Ridge Contours):\n"
            f"   - Majestic Sakura Tree Trunk & Branches: Draw powerful, organic twisting tree trunks with S-curves and wide root flares (size_px: {main_line_sz}, brush: 'Ink-3 Gpen' or 'Basic-5 Size', colors: #342017, #24140d). "
            f"Crucial: Branch hierarchically! Main thick trunk -> major bending limbs -> tapering fine secondary branches (size_px: {detail_line_sz}, pressure: 0.2->0.8->0.1) threading through the pink blossom canopy.\n"
            f"   - Mountain Crests & Sharp Contours: Outline sharp jagged crags and crisp cloud rim curves (size_px: {main_line_sz}, brush: 'Ink-3 Gpen').\n"
            "4. Layer 'Highlights' & 'FX' (Light Accents & Falling Petal Blizzard):\n"
            f"   - Falling Petal Blizzard: Scatter 30 to 60+ individual falling petal strokes and petals drifting on wind (size_px: {detail_line_sz}, 2-3 curve points each, colors: #ffffff, #ffe6f0, #ffd0e2) across foreground and midground.\n"
            f"   - Luminous Rim Lighting & Cloud Edges: Pure glowing white/pale-gold rim highlights on sunny mountain peaks and top cloud rims (size_px: {hl_glint_sz}, colors: #ffffff, #fffde6).\n"
        )
    elif any(
        k in prompt_lower
        for k in [
            "girl",
            "boy",
            "portrait",
            "face",
            "anime",
            "character",
            "hero",
            "eyes",
            "hair",
            "美少女",
            "人物",
            "顔",
        ]
    ):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Anime / Manga Character Portrait (Exquisite Fine Anatomy)]\n"
            "1. Layer 'Flats' (Flawless Base Volumes):\n"
            f"   - Skin Base: Smooth coverage of face, neck and shoulders (size_px: {flats_sz}, brush: 'Basic-5 Size', color: #fff0e6 / #fef2ea).\n"
            f"   - Hair Base Mass: Large solid silhouette of hair volume (size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            "   - Sclera (Whites of eyes): Clean bright base (#f8f9fa, size_px: 12-20px) under eye area.\n"
            "   - Iris Base: Expressive oval color discs for irises (#e84a75 / #3b82f6 / #8b5cf6, size_px: 15-28px).\n"
            "2. Layer 'Shading' (Delicate Facial Depth, Blush & Ambient Occlusion):\n"
            "   - Soft Facial Blush (Cheeks): Airbrush Soft, color: #ff9fb2, size_px: 40-75px, opacity: 0.25-0.35, pressure: 0.2-0.3 on cheek apples.\n"
            "   - Neck Cast Shadow: Soft warm shadow right beneath chin/jaw (color: #e09f90, size_px: 20-40px, opacity: 0.35-0.5, pressure: 0.35).\n"
            f"   - Iris Top Gradient: Deep shadow covering top half of irises (#201222, size_px: {detail_shad_sz}, opacity: 0.7-0.9).\n"
            f"   - Hair Strand Depth & Clothing Folds: Carve shadow crevices along hair locks (size_px: {detail_shad_sz}, opacity: 0.5-0.7).\n"
            "3. Layer 'Lineart' (EXQUISITE MICRO-DETAILS - Must use 1.5 to 3.0 px for facial features!):\n"
            "   - Upper Eyelashes (CRUCIAL): Bold sweeping arch with sharp tapered outer flick (preset: 'Ink-3 Gpen', size_px: 3.0 to 4.5 px, color: #1c1018, pressure: [[x,y,0.3], [x,y,0.95], [x,y,0.15]]).\n"
            "   - Double Eyelid Crease: Delicate thin curve just above upper lash (preset: 'Ink-3 Gpen', size_px: 1.5 to 2.0 px, color: #7a5850, pressure: 0.35).\n"
            "   - Iris Outer Ring & Pupil: Crisp circular iris outline and dark centered pupil dot (size_px: 2.0 to 2.8 px, color: #221220).\n"
            "   - Lower Lash / Eye Corner: 1-2 tiny delicate dots/short dashes under iris (size_px: 1.5 to 2.0 px, color: #221220, pressure: 0.2-0.4).\n"
            "   - Eyebrows: Graceful, slender arched curves above eyes (size_px: 2.0 to 3.0 px, color: #36222c, pressure: 0.2 -> 0.7 -> 0.15).\n"
            "   - Nose Tip: CRITICAL: DO NOT draw a vertical bridge line! Draw ONLY a tiny dot or tiny subtle hook at the nose tip (size_px: 1.5 to 2.0 px, color: #b87c70, pressure: 0.3-0.5).\n"
            "   - Lips / Mouth: Delicate upper lip line (size_px: 1.8 to 2.5 px, color: #a3384c, pressure: 0.2 -> 0.8 -> 0.2) and small subtle lower lip shadow accent.\n"
            f"   - Jawline & Chin: Elegant, smooth V/U-curve chin and slender neck lines (size_px: {main_line_sz}, color: #321c22).\n"
            "   - Flowing Hair Strands: Fine tapered hair strands framing face and flicking at tips (size_px: 1.8 to 3.5 px, pressure: 0.15 -> 0.85 -> 0.08).\n"
            "4. Layer 'Highlights' (Crisp Specular Sparkles & Angel Halo):\n"
            "   - Eye Specular Glints: 1 crisp white dot on top-left of each iris + 1 smaller dot on bottom-right (size_px: 2.0 to 3.5 px, color: #ffffff, pressure: 1.0, preset: 'Ink-3 Gpen').\n"
            "   - Hair Halo (Angel Ring): Luminous curved streak across hair crown (#ffffff / #fff2b2, size_px: 6 to 14 px, opacity: 0.7-0.9).\n"
            "   - Lip Gloss Glint: Tiny specular sparkle on lower lip (size_px: 2.0 px, color: #ffffff).\n"
        )
    elif any(
        k in prompt_lower
        for k in ["cat", "dog", "animal", "creature", "dragon", "bird", "wolf", "猫", "犬", "動物", "獣", "竜"]
    ):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Animal / Creature Art]\n"
            f"1. Layer 'Flats': Base body volume & fur base masses (size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            f"2. Layer 'Shading': Musculature shadows & fur tone gradations (size_px: {form_shad_sz}, opacity: 0.6, brush: 'Dry Bristles').\n"
            f"3. Layer 'Lineart': Facial contours, ears, paws, expressive eyes & whiskers (size_px: {detail_line_sz}, brush: 'Ink-3 Gpen', pressure: 0.2 -> 0.8 -> 0.1).\n"
            f"4. Layer 'Highlights': Glowing eyes, rim light on fur & whiskers (size_px: {hl_glint_sz}, brush: 'Basic-5 Size', color: #ffffff).\n"
        )
    elif any(
        k in prompt_lower
        for k in ["cyber", "cyberpunk", "city", "neon", "skyline", "sci-fi", "mech", "都市", "ビル", "ネオン"]
    ):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Cyberpunk City & Sci-Fi]\n"
            f"1. Layer 'Flats': Dark atmospheric background & skyscraper building silhouettes (size_px: {flats_sz}, colors: #0a0e17, #131b2e).\n"
            f"2. Layer 'Shading': Deep occlusion between buildings and foggy street glow (size_px: {form_shad_sz}, brush: 'Airbrush Soft').\n"
            f"3. Layer 'Lineart': Sharp structural edges, perspective grid, antenna spires (size_px: {main_line_sz}, brush: 'Ink-3 Gpen').\n"
            f"4. Layer 'Highlights' & 'FX': Vibrant neon signs, window grids, laser light beams (size_px: {hl_glint_sz}, colors: #00f0ff, #ff007f, #ffe600).\n"
        )
    elif any(k in prompt_lower for k in ["flower", "rose", "bouquet", "petal", "花", "バラ", "薔薇"]):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Blooming Flowers & Botanical]\n"
            f"1. Layer 'Flats': Petal base color blocks & leaf masses (size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            f"2. Layer 'Shading': Petal inner spiral crevice shadows (size_px: {detail_shad_sz}, opacity: 0.6, brush: 'Wet Textured Soft').\n"
            f"3. Layer 'Lineart': Organic petal edges, curving stem, leaf vein contours (size_px: {detail_line_sz}, brush: 'Ink-3 Gpen', pressure: 0.2 -> 0.8 -> 0.1).\n"
            f"4. Layer 'Highlights': Dewdrops, petal edge rim highlights (size_px: {hl_glint_sz}, brush: 'Basic-5 Size', color: #ffffff).\n"
        )

    progressive_section = ""
    if max_iterations > 1:
        if max_iterations == 2:
            if iteration == 1:
                phase_title = "Step 1/2: Foundation, Backdrop & Base Color Blocking (Flats/Shading layer)"
                phase_task = (
                    "Focus strictly on painting complete, gapless broad base colors (Flats): sky gradient, terrain/ground, mountain silhouettes, "
                    "skin/hair masses, or foliage clumps with large brush sizes. Do not draw lineart or fine details yet."
                )
            else:
                phase_title = (
                    "Step 2/2 [FINAL]: Shading, Structural Lineart, Highlights & Details (Lineart/Highlights/FX)"
                )
                phase_task = (
                    "Complete the artwork by layering shadow depths (Shading), drawing crisp expressive contours (Lineart), "
                    "and scattering sparkling highlights / petals (Highlights/FX) over the existing base."
                )
        elif max_iterations == 3:
            if iteration == 1:
                phase_title = "Step 1/3: Foundation, Backdrop & Base Color Masses (Flats layer)"
                phase_task = "Paint ONLY the broad foundation and seamless base color silhouettes (Flats layer) with large brush sizes."
            elif iteration == 2:
                phase_title = "Step 2/3: 3D Form Sculpting, Ambient Occlusion & Shadows (Shading layer)"
                phase_task = (
                    "Paint shadow crevices, cloud depth, muscle/cloth shading, and midtones over the base colors."
                )
            else:
                phase_title = (
                    "Step 3/3 [FINAL]: Expressive Lineart, Highlights, Petal Scatter & Polish (Lineart/Highlights/FX)"
                )
                phase_task = (
                    "Draw crisp structural lines, facial/branch details, glowing highlights, and finishing touches."
                )
        else:
            if iteration == 1:
                phase_title = (
                    f"Step {iteration}/{max_iterations}: Base Color Blocking & Silhouette Masses (Flats layer)"
                )
                phase_task = "Paint foundational backdrop, environment wash, and base color masses (Flats layer)."
            elif iteration == 2:
                phase_title = (
                    f"Step {iteration}/{max_iterations}: 3D Volume Sculpting & Secondary Masses (Flats/Shading layer)"
                )
                phase_task = "Sculpt intermediate forms, secondary color variations, and primary shadow volumes."
            elif iteration < max_iterations:
                phase_title = f"Step {iteration}/{max_iterations}: Deep Shadow Crevices & Structural Contours (Shading/Lineart layer)"
                phase_task = "Add deep occlusion shadows and organic structural contours over the existing shapes."
            else:
                phase_title = f"Step {iteration}/{max_iterations} [FINAL]: Fine Lineart, Highlights, Petals & Polish (Lineart/Highlights/FX)"
                phase_task = "Finish the painting with sharp line details, sparkling highlights, falling petals, and lighting FX."

        progressive_section = (
            f"\n=== MULTI-STEP PROGRESSIVE DRAWING MODE ===\n"
            f"Current Execution: {phase_title}\n"
            f"Goal for this step: {phase_task}\n"
            "Draw ONLY the strokes appropriate for THIS step. Do not try to rush and draw everything at once; build coherently upon previous steps.\n"
        )

    visual_feedback_section = (
        "\n=== AUTOMATIC VISUAL FEEDBACK ===\n"
        "In multi-step mode, the current rendered canvas is captured automatically after each intermediate step and attached to the next request. "
        "Inspect that image before adding corrections. Keep `request_canvas_image` false; the field remains only for schema compatibility.\n"
    )

    reasoning_guide = (
        "Output ONLY the JSON object. Do not output any conversational thoughts, explanations, or commentary."
        if is_reasoning
        else "Do not output conversational commentary."
    )

    return (
        "You are an elite master digital painter directing layer-by-layer drawing plans for Krita. "
        "Create a rich, complete, painterly illustration by generating multi-layered strokes from back to front.\n"
        f"{reasoning_guide}\n\n"
        "=== DIGITAL PAINTING METHODOLOGY & BRUSH PRESETS ===\n"
        "Available Brush Presets:\n"
        "  - 'Airbrush Soft': Smooth sky gradients, soft blush, and ambient shading.\n"
        "  - 'Wet Textured Soft': Puffy clouds, water reflections, painterly blossom clusters.\n"
        "  - 'Dry Bristles': Rough tree bark, rocky mountain crags, textured foliage.\n"
        "  - 'Basic-5 Size' / 'Basic-1': Solid silhouette blocking, undercoats, general painting.\n"
        "  - 'Ink-3 Gpen': Razor-sharp lineart, expressive eyes, twisting branches, delicate facial features.\n"
        "  - 'Eraser Soft' / 'Eraser Small': Carving clean silhouette edges and light accents.\n\n"
        "=== PRESSURE SENSITIVITY & DYNAMICS (0.05 to 1.0) ===\n"
        "Points support per-point pressure: `[x, y, pressure]` (e.g. `[[100, 150, 0.2], [120, 140, 0.95], [150, 130, 0.08]]`):\n"
        "  - Tapering & Flick Strokes (Eyelashes, Hair Tips, Branches): Use low entry (0.2), peak core (0.9-1.0), and razor-sharp exit (0.05-0.15).\n"
        "  - Feathering & Soft Blending (Blush, Skin Shadows, Glows): Use gentle low pressure (0.15 to 0.35) with 'Airbrush Soft'.\n"
        "  - Structural Occlusion: Use firm pressure (0.85 to 1.0) for depth accents.\n"
        "  - Eye Highlights: Use pressure 1.0 on tiny strokes (size_px: 2.0 to 3.0) for crisp sparkling dots.\n\n"
        "Layer Hierarchy (Back to Front):\n"
        f"1. Layer 'Flats' (Backdrop, Gradients & Seamless Color Blocking):\n"
        f"   - Must use LARGE brush sizes (size_px: {flats_sz}) with dense overlapping strokes to fully cover backgrounds (sky, mountains, foliage masses, skin, ground). Leave no unpainted gaps!\n"
        f"2. Layer 'Shading' (3D Volume, Cast Shadows & Occlusion):\n"
        f"   - Use medium/fine brush sizes ({form_shad_sz} for general volume, {detail_shad_sz} for delicate face/crevices) with darker/cooler tones.\n"
        f"3. Layer 'Lineart' (Contours, Tree Anatomy & Fine Features):\n"
        f"   - Use dynamic crisp brush sizes ({main_line_sz} for outer silhouettes, {detail_line_sz} for fine eyes/lashes/nose/mouth/hair tips, preset: 'Ink-3 Gpen').\n"
        f"4. Layer 'Highlights' & 'FX' (Specular Glints, Petal Swarms, Atmosphere):\n"
        f"   - Use accent brush sizes ({hl_glint_sz}) with luminous colors for falling petals, cloud rim light, sun flecks, and particle FX.\n"
        "5. Eraser Strokes ('is_eraser': true):\n"
        '   - You can add eraser strokes (`"is_eraser": true`) on any layer to cleanly sculpt contours, sharpen silhouette edges, fix color bleeds, or carve sharp light highlights.\n\n'
        f"=== PALETTE DIRECTION: {palette_name.upper()} ===\n"
        f"Harmonize colors to match the '{palette_name}' aesthetic: prioritize cohesive color theory, distinct value contrast between shadow and light, and vibrant accent highlights.\n"
        f"{domain_guidance}"
        f"{progressive_section}"
        f"{visual_feedback_section}\n"
        "=== OUTPUT SCHEMA & TOKEN-EFFICIENT POINT FORMAT ===\n"
        "Return exactly one valid JSON object. Points use high-efficiency compact arrays [[x, y], [x, y, pressure]] or object format:\n"
        "```json\n"
        "{\n"
        '  "schema_version": 1,\n'
        f'  "prompt": {json.dumps(prompt or "illustration")},\n'
        '  "seed": 42,\n'
        '  "title": "Artwork Title",\n'
        f'  "iteration": {iteration},\n'
        '  "request_canvas_image": false,\n'
        '  "goal_reached": false,\n'
        '  "completion_score": 0.85,\n'
        '  "layers": ["Flats", "Shading", "Lineart", "Highlights", "FX"],\n'
        '  "strokes": [\n'
        "    {\n"
        '      "id": "s1",\n'
        '      "brush_preset": "Airbrush Soft",\n'
        '      "color": "#3a7bd5",\n'
        f'      "size_px": {max(70, round(min_dim * 0.12))},\n'
        '      "layer_name": "Flats",\n'
        '      "opacity": 1.0,\n'
        '      "is_eraser": false,\n'
        f'      "points": [[0, {round(height * 0.15)}], [{round(width * 0.5)}, {round(height * 0.12)}], [{round(width)}, {round(height * 0.18)}]]\n'
        "    },\n"
        "    {\n"
        '      "id": "s2",\n'
        '      "brush_preset": "Ink-3 Gpen",\n'
        '      "color": "#2c1810",\n'
        f'      "size_px": {max(10, round(min_dim * 0.008))},\n'
        '      "layer_name": "Lineart",\n'
        '      "opacity": 1.0,\n'
        '      "is_eraser": false,\n'
        f'      "points": [[{round(width * 0.5)}, {round(height * 0.95)}, 0.95], [{round(width * 0.51)}, {round(height * 0.65)}, 0.85], [{round(width * 0.48)}, {round(height * 0.42)}, 0.7]]\n'
        "    }\n"
        "  ]\n"
        "}\n"
        "```\n"
        "=== CRITICAL RULES ===\n"
        f"1. Coordinates (x, y): Must be within canvas dimensions (0 to {width:.0f} width, 0 to {height:.0f} height).\n"
        "2. Stroke Points: Provide 2 to 6 key curve control points per stroke in compact format [[x, y], [x, y, pressure]]. The engine automatically interpolates smooth Catmull-Rom splines and natural pressure tapering.\n"
        f"3. Brush Sizes & Coverage: Do NOT make all strokes tiny. Use dense wide strokes ({flats_sz}) to completely paint undercoats and backgrounds without empty gaps.\n"
        "4. Stroke Count: When stroke_count is auto or unconstrained, freely generate as many strokes (e.g. 50-150+ strokes for complete detail) as needed to create a stunning finished artwork.\n"
        "5. Goal Evaluation: Set 'goal_reached' to true only when the artwork is fully finished and meets all prompt requirements.\n"
        "6. First character of output must be '{' or '```json'."
    )


# 思考タグ除去用正規表現（大文字小文字不問、未閉鎖タグ対応）
_THINKING_TAG_PATTERNS = [
    # 閉じたタグ: <think>...</think>, <thought>...</thought>, <reasoning>...</reasoning>, <thought_process>...</thought_process>, <reflection>...</reflection>
    re.compile(
        r"<(?:think|thought|reasoning|thought_process|reflection)>[\s\S]*?</(?:think|thought|reasoning|thought_process|reflection)>",
        re.IGNORECASE,
    ),
    # 角括弧形式: [THOUGHT]...[/THOUGHT], [REASONING]...[/REASONING]
    re.compile(
        r"\[(?:thought|reasoning|thought_process)\][\s\S]*?\[/(?:thought|reasoning|thought_process)\]", re.IGNORECASE
    ),
    # 特殊トークン形式: |begin_of_thought|...|end_of_thought|
    re.compile(r"\|begin_of_thought\|[\s\S]*?\|end_of_thought\|", re.IGNORECASE),
    # 日本語タグ: 【思考】...【/思考】
    re.compile(r"【(?:思考|推論)】[\s\S]*?【/(?:思考|推論)】", re.IGNORECASE),
    # 閉じられていない未閉鎖タグ（途中で切れた場合や本文手前で終了した場合）
    re.compile(r"<(?:think|thought|reasoning|thought_process|reflection)>[\s\S]*$", re.IGNORECASE),
    re.compile(r"\[(?:thought|reasoning|thought_process)\][\s\S]*$", re.IGNORECASE),
    re.compile(r"\|begin_of_thought\|[\s\S]*$", re.IGNORECASE),
    re.compile(r"【(?:思考|推論)】[\s\S]*$", re.IGNORECASE),
]


def _clean_thinking_tokens(text: str) -> str:
    """思考プロセスタグ（<think>, <thought>, <reasoning>, [THOUGHT] 等）を安全に除去する。"""
    cleaned = text
    for pattern in _THINKING_TAG_PATTERNS:
        cleaned = pattern.sub("", cleaned)
    return cleaned.strip()


# タグなしプレーンテキスト思考の冒頭パターン（CoT: "The user wants...", "Let me plan...", "Thinking process:" 等）
_PLAIN_THINKING_PATTERNS = [
    re.compile(
        r"^(?:The user wants|I need to|Let me plan|Let's create|Thinking Process|Plan:|Step 1:|To draw|In this drawing)[\s\S]*?(?=(?:```|\{\s*\"(?:schema_version|prompt|seed|title|iteration|layers|strokes)\"))",
        re.IGNORECASE,
    ),
    re.compile(
        r"^[\s\S]*?(?=(?:```json\s*\{|```\s*\{|\{\s*\"(?:schema_version|prompt|strokes)\"))",
        re.IGNORECASE,
    ),
]


def _strip_plain_text_thinking(text: str) -> str:
    """タグのない自然言語の思考プロセス（Chain of Thought）を検知し、JSON 本文の手前をパージする。"""
    s = text.strip()
    if not s:
        return ""

    # すでに JSON またはコードブロックで始まっている場合はそのまま
    if s.startswith("{") or s.startswith("```"):
        return s

    # 1. 明示的なコードブロックまたは主要 JSON キーの開始位置を探す
    for pat in _PLAIN_THINKING_PATTERNS:
        m = pat.search(s)
        if m and m.end() < len(s):
            trimmed = s[m.end() :].strip()
            if trimmed.startswith("```") or trimmed.startswith("{"):
                return trimmed

    # 2. 最も妥当な JSON 開始アンカーの探索
    best_idx = _find_best_json_start(s)
    if best_idx >= 0:
        return s[best_idx:].strip()

    return s


def _find_best_json_start(text: str) -> int:
    """思考文のゴミに含まれる `{` を避け、DrawingPlan または Stroke JSON の最も確からしい開始位置を特定する。"""
    if not text:
        return -1

    # 優先順位 1: コードブロック直後の `{`
    fence_m = re.search(r"```(?:json)?\s*(\{)", text, flags=re.IGNORECASE)
    if fence_m:
        return fence_m.start(1)

    # 優先順位 2: DrawingPlan 主要キーを含む `{`
    plan_key_m = re.search(
        r"\{\s*\"(?:schema_version|prompt|seed|title|iteration|layers|strokes)\"", text, flags=re.IGNORECASE
    )
    if plan_key_m:
        return plan_key_m.start()

    # 優先順位 3: ストローク要素のキーを含む `{`
    stroke_key_m = re.search(r"\{\s*\"(?:id|points|brush_preset|layer_name)\"", text, flags=re.IGNORECASE)
    if stroke_key_m:
        return stroke_key_m.start()

    # 優先順位 4: 単純な最初の `{`
    return text.find("{")


def _sanitize_json_text(text: str) -> str:
    """LLM 特有の構文乱れ（コメント、末尾カンマ、シングルクォート、Python 定数）をサニタイズする。"""
    if not text:
        return ""

    s = text.strip()

    # 1. ブロックコメント /* ... */ の除去
    s = re.sub(r"/\*[\s\S]*?\*/", "", s)

    # 2. 行コメント // ... の除去 (URL 中の "http://" や "https://" は除外)
    lines = []
    for line in s.splitlines():
        line_clean = re.sub(r'(?<![:"\'/])//.*$', "", line)
        lines.append(line_clean)
    s = "\n".join(lines)

    # 3. Python 定数を JSON 定数に正規化
    s = re.sub(r"\bTrue\b", "true", s)
    s = re.sub(r"\bFalse\b", "false", s)
    s = re.sub(r"\bNone\b", "null", s)

    # 4. シングルクォートで囲まれた文字列 '...' をダブルクォート "..." に変換
    def _replace_sq(m: re.Match[str]) -> str:
        inner = m.group(1).replace('"', '\\"')
        return f'"{inner}"'

    s = re.sub(r"(?<!\\)'([^'\\]*(?:\\.[^'\\]*)*)'", _replace_sq, s)

    # 5. オブジェクト・配列末尾のカンマ（Trailing commas）の除去
    s = re.sub(r",\s*([}\]])", r"\1", s)

    return s.strip()


def _collect_candidate_texts_from_response(
    response: Mapping[str, Any],
    log_func: Callable[[str], None] | None = None,
    is_drawing_plan: bool = False,
) -> list[tuple[str, str]]:
    """API レスポンスから抽出可能な全テキスト候補を (ソース名, テキスト) の優先順序付きリストとして収集する。"""
    candidates: list[tuple[str, str]] = []

    # 1. OpenAI Chat Completions 形式 (response["choices"])
    choices = response.get("choices")
    if choices and isinstance(choices, list) and isinstance(choices[0], Mapping):
        first_choice = choices[0]
        finish_reason = first_choice.get("finish_reason")
        if finish_reason in ("length", "max_tokens", "MAX_TOKENS") and log_func is not None:
            if is_drawing_plan:
                log_func(
                    f"警告: LLM の最大トークン数上限に達しました (finish_reason: {finish_reason})。"
                    "設定の Max Tokens を増やすかストローク数を調整してください。途切れ JSON の救済を試みます。"
                )
            else:
                log_func(f"[LLM 応答状態] finish_reason: {finish_reason}")

        message = first_choice.get("message")
        if isinstance(message, Mapping):
            # 本文 content
            content = message.get("content")
            if isinstance(content, list):
                content_str = "".join(part.get("text", "") for part in content if isinstance(part, Mapping))
            elif isinstance(content, str):
                content_str = content
            else:
                content_str = ""
            if content_str.strip():
                candidates.append(("message.content", content_str.strip()))

            # 思考フィールド reasoning_content / reasoning / thought
            reasoning = message.get("reasoning_content") or message.get("reasoning") or message.get("thought")
            if isinstance(reasoning, str) and reasoning.strip():
                candidates.append(("message.reasoning_content", reasoning.strip()))

            # Tool Calls 形式
            tool_calls = message.get("tool_calls")
            if isinstance(tool_calls, list) and tool_calls:
                first_tool = tool_calls[0]
                if isinstance(first_tool, Mapping):
                    args = first_tool.get("function", {}).get("arguments")
                    if isinstance(args, str) and args.strip():
                        candidates.append(("tool_calls.arguments", args.strip()))

        # Legacy completions 形式
        if "text" in first_choice and isinstance(first_choice["text"], str) and first_choice["text"].strip():
            candidates.append(("choices[0].text", first_choice["text"].strip()))

    # 2. Google Gemini Native 形式 (candidates[0].content.parts)
    gemini_candidates = response.get("candidates")
    if isinstance(gemini_candidates, list) and gemini_candidates:
        first_cand = gemini_candidates[0]
        if isinstance(first_cand, Mapping):
            g_finish = first_cand.get("finishReason") or first_cand.get("finish_reason")
            if g_finish in ("MAX_TOKENS", "max_tokens", "LENGTH", "length") and log_func is not None:
                if is_drawing_plan:
                    log_func(
                        f"警告: Gemini API の最大トークン数上限に達しました (finishReason: {g_finish})。"
                        "設定の Max Tokens を増やしてください。途切れ JSON の救済を試みます。"
                    )
                else:
                    log_func(f"[Gemini 応答状態] finishReason: {g_finish}")

            content = first_cand.get("content")
            parts = content.get("parts") if isinstance(content, Mapping) else []
            if isinstance(parts, list):
                # 通常テキストパート
                normal_parts = [p.get("text", "") for p in parts if isinstance(p, Mapping) and not p.get("thought")]
                normal_text = "".join(normal_parts).strip()
                if normal_text:
                    candidates.append(("gemini.parts.text", normal_text))

                # 思考パート (thought: True)
                thought_parts = [p.get("text", "") for p in parts if isinstance(p, Mapping) and p.get("thought")]
                thought_text = "".join(thought_parts).strip()
                if thought_text:
                    candidates.append(("gemini.parts.thought", thought_text))

                # 全パート結合
                all_text = "".join(p.get("text", "") for p in parts if isinstance(p, Mapping)).strip()
                if all_text and all_text != normal_text and all_text != thought_text:
                    candidates.append(("gemini.parts.all", all_text))

    # 3. Anthropic Native 形式 (content)
    anth_stop = response.get("stop_reason")
    if anth_stop in ("max_tokens", "length") and log_func is not None:
        if is_drawing_plan:
            log_func(
                f"警告: Anthropic API の最大トークン数上限に達しました (stop_reason: {anth_stop})。"
                "設定の Max Tokens を増やしてください。途切れ JSON の救済を試みます。"
            )
        else:
            log_func(f"[Anthropic 応答状態] stop_reason: {anth_stop}")

    anth_content = response.get("content")
    if isinstance(anth_content, list) and anth_content:
        # text パート
        text_parts = [p.get("text", "") for p in anth_content if isinstance(p, Mapping) and p.get("type") == "text"]
        text_str = "".join(text_parts).strip()
        if text_str:
            candidates.append(("anthropic.content.text", text_str))

        # thinking パート
        thinking_parts = [
            p.get("thinking", "") or p.get("text", "")
            for p in anth_content
            if isinstance(p, Mapping) and p.get("type") in ("thinking", "thought")
        ]
        thinking_str = "".join(thinking_parts).strip()
        if thinking_str:
            candidates.append(("anthropic.content.thinking", thinking_str))
    elif isinstance(anth_content, str) and anth_content.strip():
        candidates.append(("anthropic.content", anth_content.strip()))

    # 4. Ollama / Bedrock / Direct 形式
    msg_field = response.get("message")
    if isinstance(msg_field, Mapping):
        c = msg_field.get("content")
        if isinstance(c, str) and c.strip():
            candidates.append(("ollama.message.content", c.strip()))

    resp_field = response.get("response")
    if isinstance(resp_field, str) and resp_field.strip():
        candidates.append(("ollama.response", resp_field.strip()))

    out_field = response.get("output")
    if isinstance(out_field, Mapping) and "text" in out_field:
        t = out_field["text"]
        if isinstance(t, str) and t.strip():
            candidates.append(("output.text", t.strip()))

    return candidates


def _extract_content_from_response(
    response: Mapping[str, Any],
    log_func: Callable[[str], None] | None = None,
    is_drawing_plan: bool = False,
) -> str | Mapping[str, Any]:
    """互換性維持のためのコンテンツ抽出関数。トップレベルエラー検知および直接 JSON を検出する。"""
    # 1. API エラーオブジェクトの明示的検出
    if "error" in response:
        err = response["error"]
        if isinstance(err, Mapping):
            err_msg = err.get("message") or err.get("code") or str(err)
            err_type = err.get("type", "")
            type_info = f" [{err_type}]" if err_type else ""
            raise LLMPlannerError(f"LLM API エラー{type_info}: {err_msg}")
        raise LLMPlannerError(f"LLM API エラー: {err}")

    if "detail" in response and not any(
        k in response for k in ("choices", "candidates", "content", "message", "strokes")
    ):
        raise LLMPlannerError(f"LLM API エラー: {response['detail']}")

    # 2. トップレベルが直接 DrawingPlan 辞書である場合
    if "strokes" in response and isinstance(response["strokes"], list):
        if log_func is not None:
            log_func("通知: レスポンス直下の DrawingPlan JSON 構造を直接検出しました")
        return response

    # 3. 候補テキストの収集
    candidates = _collect_candidate_texts_from_response(response, log_func=log_func, is_drawing_plan=is_drawing_plan)
    if candidates:
        return candidates[0][1]

    # 空の choices チェック
    choices = response.get("choices")
    if choices is not None and isinstance(choices, list) and len(choices) == 0:
        raise LLMPlannerError(
            "LLM API 応答の choices 配列が空です (安全フィルターまたはトークン上限の可能性があります)"
        )

    # 抽出失敗時の診断情報
    available_keys = ", ".join(list(response.keys())[:8])
    if log_func is not None:
        log_func(f"エラー: 認識可能なコンテンツが見つかりませんでした (レスポンスキー: [{available_keys}])")
    raise LLMPlannerError(f"LLM API 応答から本文を抽出できませんでした (キー: [{available_keys}])")


def _extract_best_content_or_plan(
    response: Mapping[str, Any],
    log_func: Callable[[str], None] | None = None,
    is_drawing_plan: bool = False,
) -> str | Mapping[str, Any]:
    """接続テストおよび汎用抽出用ヘルパー。"""
    return _extract_content_from_response(response, log_func=log_func, is_drawing_plan=is_drawing_plan)


def _plan_from_response(
    response: Mapping[str, Any],
    prompt: str = "",
    seed: int = 0,
    width: float = 1000.0,
    height: float = 1000.0,
    log_func: Callable[[str], None] | None = None,
) -> DrawingPlan:
    """思考モデルを含む多様なレスポンスから、最も妥当な DrawingPlan を多段探索・救出して生成する。"""
    # 1. API エラー確認
    if "error" in response:
        err = response["error"]
        if isinstance(err, Mapping):
            err_msg = err.get("message") or err.get("code") or str(err)
            raise LLMPlannerError(f"LLM API エラー: {err_msg}")
        raise LLMPlannerError(f"LLM API エラー: {err}")

    # 2. 直接 DrawingPlan 辞書の場合
    if "strokes" in response and isinstance(response["strokes"], list) and len(response["strokes"]) > 0:
        try:
            return DrawingPlan.from_dict(response)
        except Exception as exc:
            if log_func is not None:
                log_func(f"通知: トップレベル辞書からの復元を試行中に警告: {exc}")

    # 3. レスポンス内の全テキスト候補（content, reasoning_content, parts, thinking等）を順次精査
    candidates = _collect_candidate_texts_from_response(response, log_func=log_func, is_drawing_plan=True)
    if not candidates:
        choices = response.get("choices")
        if choices is not None and isinstance(choices, list) and len(choices) == 0:
            raise LLMPlannerError("LLM API 応答の choices 配列が空です")
        raise LLMPlannerError("LLM API 応答から本文または思考プロセスを抽出できませんでした")

    last_error: Exception | None = None

    for source_name, text in candidates:
        if not text.strip():
            continue
        try:
            value = _extract_json_object(text, log_func=log_func)
            if "strokes" in value and isinstance(value["strokes"], list) and len(value["strokes"]) > 0:
                if log_func is not None and source_name != "message.content":
                    log_func(f"通知: ソース [{source_name}] から DrawingPlan JSON オブジェクトを救出しました")
                return DrawingPlan.from_dict(value)
        except Exception as exc:
            last_error = exc
            continue

    # 4. 断片ストローク・ハーベスターによる救出（全候補テキストからストロークを探索）
    for source_name, text in candidates:
        if not text.strip():
            continue
        harvested = _harvest_stroke_fragments(text, log_func=log_func)
        if harvested is not None and "strokes" in harvested and len(harvested["strokes"]) > 0:
            if log_func is not None:
                log_func(
                    f"通知: ソース [{source_name}] から {len(harvested['strokes'])} 本のストローク断片を直接救出・合成しました"
                )
            try:
                return DrawingPlan.from_dict(harvested)
            except Exception as exc:
                last_error = exc

    # どの候補からも完全な DrawingPlan が得られなかった場合
    first_text = candidates[0][1]
    if log_func is not None:
        log_preview = first_text[:200] + ("..." if len(first_text) > 200 else "")
        log_func(f"LLM 応答パース試行 (テキスト先頭):\n{log_preview}")

    try:
        value = _extract_json_object(first_text, log_func=log_func)
        return DrawingPlan.from_dict(value)
    except Exception as exc:
        raise LLMPlannerError(f"LLM が有効な DrawingPlan JSON を返しませんでした: {exc}") from (last_error or exc)


def _extract_json_object(content: str, log_func: Callable[[str], None] | None = None) -> Mapping[str, Any]:
    """プレーンテキスト思考除去・タグ除去・未完コードブロック救出・構文修復・断片救出を駆使して JSON を抽出する。"""
    # 1. 思考タグおよびプレーンテキスト思考の除去
    cleaned_tags = _clean_thinking_tokens(content.strip())
    stripped_thinking = _strip_plain_text_thinking(cleaned_tags)
    sanitized_stripped = _sanitize_json_text(stripped_thinking)
    sanitized_cleaned = _sanitize_json_text(cleaned_tags)
    sanitized_raw = _sanitize_json_text(content)

    text_sources = [
        sanitized_stripped,
        stripped_thinking,
        sanitized_cleaned,
        cleaned_tags,
        sanitized_raw,
        content,
    ]

    # 2. 完全なコードブロック ```json ... ``` の抽出
    for text_source in text_sources:
        if not text_source.strip():
            continue
        fence_matches = list(re.finditer(r"```(?:json)?\s*([\s\S]*?)\s*```", text_source, flags=re.IGNORECASE))
        for match in reversed(fence_matches):
            candidate = _sanitize_json_text(match.group(1).strip())
            try:
                value = json.loads(candidate)
                if isinstance(value, Mapping) and ("strokes" in value or "prompt" in value):
                    return value
            except json.JSONDecodeError:
                repaired = _attempt_json_repair(candidate)
                if repaired is not None and isinstance(repaired, Mapping) and "strokes" in repaired:
                    if log_func is not None:
                        log_func("通知: コードブロック内の途切れた JSON を自動修復しました")
                    return repaired

    # 3. 閉じられていない未完コードブロック (Unclosed Fences) の抽出
    for text_source in text_sources:
        if not text_source.strip():
            continue
        unclosed_m = re.search(r"```(?:json)?\s*(\{[\s\S]*)$", text_source, flags=re.IGNORECASE)
        if unclosed_m:
            candidate = _sanitize_json_text(unclosed_m.group(1).strip())
            repaired = _attempt_json_repair(candidate)
            if repaired is not None and isinstance(repaired, Mapping) and "strokes" in repaired:
                if log_func is not None:
                    log_func("通知: 閉じられていないコードブロックから途切れ JSON を自動修復しました")
                return repaired

    # 4. raw_decode (最適開始アンカーから)
    for target in (sanitized_stripped, stripped_thinking, sanitized_cleaned, cleaned_tags):
        if not target.strip():
            continue
        try:
            value = json.loads(target)
            if isinstance(value, Mapping) and ("strokes" in value or "prompt" in value):
                return value
        except json.JSONDecodeError:
            pass

        start_idx = _find_best_json_start(target)
        if start_idx >= 0:
            try:
                value, _ = json.JSONDecoder().raw_decode(target[start_idx:])
                if isinstance(value, Mapping) and ("strokes" in value or "prompt" in value):
                    return value
            except json.JSONDecodeError:
                pass

    # 5. 途中で途切れた JSON の高度な末尾修復 & スタック解析
    for target in text_sources:
        if not target.strip():
            continue
        repaired = _attempt_json_repair(target)
        if repaired is not None and isinstance(repaired, Mapping) and "strokes" in repaired:
            if log_func is not None:
                log_func("警告: トークン上限等で途切れた JSON を自動修復して読み込みました")
            return repaired

    # 6. 断片ストローク・ハーベスター（Stroke Fragment Harvester）による救出
    for target in text_sources:
        if not target.strip():
            continue
        harvested = _harvest_stroke_fragments(target, log_func=log_func)
        if harvested is not None and "strokes" in harvested and len(harvested["strokes"]) > 0:
            if log_func is not None:
                log_func(f"通知: テキストから {len(harvested['strokes'])} 本のストローク断片を直接救出しました")
            return harvested

    raise json.JSONDecodeError("DrawingPlan JSON オブジェクトを抽出できませんでした", content, 0)


def _harvest_stroke_fragments(text: str, log_func: Callable[[str], None] | None = None) -> dict[str, Any] | None:
    """崩壊した JSON や長文テキストから個々のストロークオブジェクトを正規表現・個別パースで救出して DrawingPlan 辞書を合成する。"""
    if not text:
        return None

    # prompt の抽出試行
    prompt_m = re.search(r'"prompt"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)"', text)
    found_prompt = prompt_m.group(1) if prompt_m else "ai illustration"

    # seed の抽出試行
    seed_m = re.search(r'"seed"\s*:\s*(\d+)', text)
    found_seed = int(seed_m.group(1)) if seed_m else 42

    strokes: list[dict[str, Any]] = []
    decoder = json.JSONDecoder()

    # points 配列を持つ JSON オブジェクトの開始位置を検索
    pattern = re.compile(
        r'\{\s*(?:"id"|"brush_preset"|"color"|"size_px"|"layer_name"|"opacity"|"points")', re.IGNORECASE
    )
    for m in pattern.finditer(text):
        idx = m.start()
        try:
            obj, _ = decoder.raw_decode(text[idx:])
            if (
                isinstance(obj, Mapping)
                and "points" in obj
                and isinstance(obj["points"], list)
                and len(obj["points"]) >= 2
            ):
                # 重複防止
                st_id = obj.get("id") or f"stroke_{len(strokes) + 1}"
                if not any(s.get("id") == st_id for s in strokes):
                    strokes.append(dict(obj))
        except (json.JSONDecodeError, ValueError):
            continue

    if strokes:
        return {
            "schema_version": 1,
            "prompt": found_prompt,
            "seed": found_seed,
            "title": f"Rescued AI Plan - {found_prompt[:20]}",
            "iteration": 1,
            "request_canvas_image": bool(re.search(r'"request_canvas_image"\s*:\s*true', text, re.I)),
            "layers": ["Draft", "Lineart", "Flats", "Shading", "Highlights", "FX"],
            "strokes": strokes,
        }

    return None


def _sanitize_repaired_dict(val: Mapping[str, Any]) -> dict[str, Any] | None:
    """修復された JSON 辞書内の strokes 配列を検査し、不完全な点を補完または未完ストロークを除去する。"""
    d = dict(val)
    strokes_raw = d.get("strokes")
    if not isinstance(strokes_raw, list):
        return d

    clean_strokes: list[dict[str, Any]] = []
    for idx, st in enumerate(strokes_raw, start=1):
        if not isinstance(st, Mapping):
            continue
        pts = st.get("points")
        if not isinstance(pts, list) or len(pts) == 0:
            continue
        clean_pts: list[Any] = []
        for p in pts:
            if isinstance(p, Mapping):
                if "x" in p and "y" in p and not isinstance(p.get("x"), bool) and not isinstance(p.get("y"), bool):
                    clean_pts.append(dict(p))
            elif (
                isinstance(p, (list, tuple))
                and len(p) >= 2
                and not isinstance(p[0], bool)
                and not isinstance(p[1], bool)
            ):
                clean_pts.append(list(p))
        if len(clean_pts) == 1:
            p0 = clean_pts[0]
            if isinstance(p0, Mapping):
                clean_pts.append(
                    {
                        "x": float(p0.get("x", 0.0)) + 1.0,
                        "y": float(p0.get("y", 0.0)) + 1.0,
                        "pressure": float(p0.get("pressure", 0.5)),
                        "time_ms": int(p0.get("time_ms", 0)) + 10,
                    }
                )
            else:
                clean_pts.append([float(p0[0]) + 1.0, float(p0[1]) + 1.0, 0.8, 10])
        if len(clean_pts) >= 2:
            st_clean = dict(st)
            st_clean["points"] = clean_pts
            if not st_clean.get("id"):
                st_clean["id"] = f"stroke_{idx}"
            clean_strokes.append(st_clean)

    if clean_strokes:
        d["strokes"] = clean_strokes
        return d
    return None


def _attempt_json_repair(text: str) -> Mapping[str, Any] | None:
    """トークン上限等で末尾が切れた JSON の最適開始アンカー特定、未完ストローク切落し、構文修復を行う。"""
    start = _find_best_json_start(text)
    if start < 0:
        return None
    s = _sanitize_json_text(text[start:].strip())

    # 1. 未完のストロークを直前の完全なストローク `}` までロールバックして閉じる救済
    last_brace = s.rfind("}")
    if last_brace > 0:
        candidate_truncated = s[: last_brace + 1].strip()
        for suffix in ("]}", "]}]}", "}]}", "]}", "}"):
            try:
                val = json.loads(candidate_truncated + suffix)
                if isinstance(val, Mapping) and "strokes" in val:
                    sanitized = _sanitize_repaired_dict(val)
                    if sanitized is not None:
                        return sanitized
            except json.JSONDecodeError:
                continue

    # 2. 末尾の不完全なキーや未閉鎖文字列のトリミング
    s_cleaned = re.sub(r',\s*"[^"]*"?\s*:\s*[^,}\]]*$', "", s)
    s_cleaned = re.sub(r",\s*$", "", s_cleaned)

    # 3. 開き括弧スタック解析による自動バランシング
    stack: list[str] = []
    in_string = False
    escape = False

    for char in s_cleaned:
        if escape:
            escape = False
            continue
        if char == "\\":
            escape = True
            continue
        if char == '"':
            in_string = not in_string
            continue
        if in_string:
            continue
        if char in ("{", "["):
            stack.append(char)
        elif (char == "}" and stack and stack[-1] == "{") or (char == "]" and stack and stack[-1] == "["):
            stack.pop()

    # スタックに残った開き括弧に対応する閉じ括弧を生成
    closing_suffix = ""
    if in_string:
        closing_suffix += '"'
    for opener in reversed(stack):
        closing_suffix += "}" if opener == "{" else "]"

    if closing_suffix:
        try:
            val = json.loads(s_cleaned + closing_suffix)
            if isinstance(val, Mapping) and "strokes" in val:
                sanitized = _sanitize_repaired_dict(val)
                if sanitized is not None:
                    return sanitized
        except json.JSONDecodeError:
            pass

    # 4. 定型サフィックスによるフォールバック修復
    for suffix in (
        '"]}]}',
        '"}]}',
        "]}",
        "}",
        "}]}",
        '0,"time_ms":0}]}]}',
        "]}]}",
    ):
        try:
            val = json.loads(s_cleaned + suffix)
            if isinstance(val, Mapping) and "strokes" in val:
                sanitized = _sanitize_repaired_dict(val)
                if sanitized is not None:
                    return sanitized
        except json.JSONDecodeError:
            pass
        try:
            val = json.loads(s + suffix)
            if isinstance(val, Mapping) and "strokes" in val:
                sanitized = _sanitize_repaired_dict(val)
                if sanitized is not None:
                    return sanitized
        except json.JSONDecodeError:
            pass

    return None


def _catmull_rom_points(
    control_points: list[tuple[float, float, float]],
    samples_per_segment: int = 6,
) -> list[tuple[float, float, float]]:
    """Catmull-Rom スプライン補間により、制御点 (x, y, pressure) を通る滑らかな曲線を算出する。"""
    if len(control_points) < 2:
        return list(control_points)
    if len(control_points) == 2:
        p0, p1 = control_points
        return [
            (
                p0[0] + (p1[0] - p0[0]) * (i / samples_per_segment),
                p0[1] + (p1[1] - p0[1]) * (i / samples_per_segment),
                p0[2] + (p1[2] - p0[2]) * (i / samples_per_segment),
            )
            for i in range(samples_per_segment + 1)
        ]

    pts = [control_points[0]] + list(control_points) + [control_points[-1]]
    result: list[tuple[float, float, float]] = []

    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        num_steps = samples_per_segment if i < len(pts) - 3 else samples_per_segment + 1
        for step in range(num_steps):
            t = step / samples_per_segment
            t2 = t * t
            t3 = t2 * t

            x = 0.5 * (
                (2 * p1[0])
                + (-p0[0] + p2[0]) * t
                + (2 * p0[0] - 5 * p1[0] + 4 * p2[0] - p3[0]) * t2
                + (-p0[0] + 3 * p1[0] - 3 * p2[0] + p3[0]) * t3
            )
            y = 0.5 * (
                (2 * p1[1])
                + (-p0[1] + p2[1]) * t
                + (2 * p0[1] - 5 * p1[1] + 4 * p2[1] - p3[1]) * t2
                + (-p0[1] + 3 * p1[1] - 3 * p2[1] + p3[1]) * t3
            )
            p = 0.5 * (
                (2 * p1[2])
                + (-p0[2] + p2[2]) * t
                + (2 * p0[2] - 5 * p1[2] + 4 * p2[2] - p3[2]) * t2
                + (-p0[2] + 3 * p1[2] - 3 * p2[2] + p3[2]) * t3
            )
            result.append((x, y, p))
    return result


def _smooth_and_densify_points(
    raw_points: Sequence[StrokePoint],
    width: float,
    height: float,
    layer_name: str = "Lineart",
    size_px: float = 10.0,
    max_dense_points: int = 40,
) -> list[StrokePoint]:
    """大まかな制御点列を Catmull-Rom スプライン補間および自然な筆圧テーパリングで滑らかな手描きストロークへ変換する。

    ブラシサイズが大きい場合（下塗り・背景）は Krita の paintLine での Dab 重複（ビーズ状アーティファクト）
    を防ぐため、ステップ間隔を適応的に間引き、均一な面塗りを維持する。
    """
    if not raw_points:
        return []
    if len(raw_points) == 1:
        p0 = raw_points[0]
        return [
            p0,
            StrokePoint(
                x=min(width - 0.1, p0.x + 1.0),
                y=min(height - 0.1, p0.y + 1.0),
                pressure=p0.pressure,
                time_ms=p0.time_ms + 10,
            ),
        ]

    layer_lower = layer_name.lower().strip()
    is_flat_or_shading = "flat" in layer_lower or "back" in layer_lower or "draft" in layer_lower

    # 点数がすでに十分多い場合 (>= 35点) はそのままバウンディングのみ
    if len(raw_points) >= 35:
        return [
            StrokePoint(
                x=min(max(0.0, pt.x), width - 0.5),
                y=min(max(0.0, pt.y), height - 0.5),
                pressure=min(max(0.05, pt.pressure), 1.0),
                time_ms=pt.time_ms,
            )
            for pt in raw_points
        ]

    ctrl = [(pt.x, pt.y, pt.pressure) for pt in raw_points]
    num_segs = max(1, len(ctrl) - 1)

    # ブラシサイズに応じた適応的サンプリング上限（太いブラシほど過剰サンプリングを抑えて Dab 重複を防ぐ）
    if is_flat_or_shading or size_px >= 40.0:
        effective_max_points = max(6, min(16, max_dense_points // 2))
        samples_per_seg = max(2, min(5, effective_max_points // num_segs))
    else:
        effective_max_points = max_dense_points
        samples_per_seg = max(3, min(8, effective_max_points // num_segs))

    spline_pts = _catmull_rom_points(ctrl, samples_per_segment=samples_per_seg)

    # 距離ベースの間引き: 太いブラシで近すぎる連続点を排除
    if len(spline_pts) > 2 and size_px >= 20.0:
        min_dist_sq = max(16.0, (size_px * 0.25) ** 2)
        filtered_spline: list[tuple[float, float, float]] = [spline_pts[0]]
        for pt in spline_pts[1:-1]:
            last_pt = filtered_spline[-1]
            d_sq = (pt[0] - last_pt[0]) ** 2 + (pt[1] - last_pt[1]) ** 2
            if d_sq >= min_dist_sq:
                filtered_spline.append(pt)
        filtered_spline.append(spline_pts[-1])
        spline_pts = filtered_spline

    if len(spline_pts) > effective_max_points:
        step = len(spline_pts) / effective_max_points
        spline_pts = [spline_pts[int(i * step)] for i in range(effective_max_points)]

    smoothed: list[StrokePoint] = []
    total_n = len(spline_pts) - 1
    start_time = raw_points[0].time_ms

    for idx, (sx, sy, sp) in enumerate(spline_pts):
        t = idx / max(1, total_n)
        if is_flat_or_shading:
            # 下塗り・背景: 両端が細くならずに均一な面塗りを維持するフラットテーパー
            taper_in = min(1.0, 0.8 + 0.2 * (t / 0.15)) if total_n > 2 else 1.0
            taper_out = min(1.0, 0.8 + 0.2 * ((1.0 - t) / 0.15)) if total_n > 2 else 1.0
            taper_factor = taper_in * taper_out
            final_pressure = max(0.2, min(1.0, sp * taper_factor))
        else:
            # 線画・ハイライト: AIが指定した筆圧 sp (0.05〜1.0) の抑揚を忠実に生かしつつ、自然な入り抜きテーパーを融合
            taper_in = min(1.0, t / 0.10) if total_n > 2 else 1.0
            taper_out = min(1.0, (1.0 - t) / 0.12) if total_n > 2 else 1.0
            taper_factor = 0.2 + 0.8 * (taper_in * taper_out)
            final_pressure = max(0.02, min(1.0, sp * taper_factor))

        bx = min(max(0.0, sx), width - 0.5)
        by = min(max(0.0, sy), height - 0.5)
        t_ms = start_time + idx * 10
        smoothed.append(StrokePoint(x=bx, y=by, pressure=final_pressure, time_ms=t_ms))

    return smoothed


def _adaptive_stroke_size(
    raw_size: float,
    layer_name: str,
    width: float,
    height: float,
) -> float:
    """キャンバス解像度とレイヤー特性に基づいて、ストローク太さを適切に自動補正する。

    高解像度キャンバス（2000〜4000px等）でも、目・瞳・まつ毛・毛先などの微細線画（1.0〜4.0px）を
    強制的に太くせず、繊細な描写を完全に保持する。
    """
    min_dim = min(width, height)
    layer_lower = layer_name.lower().strip()
    r_sz = float(raw_size)

    if "flat" in layer_lower or "back" in layer_lower or "draft" in layer_lower:
        # 下塗り・背景: 最低でもキャンバス短辺の 2.0% 以上、上限 450px
        min_sz = max(15.0, min_dim * 0.02)
        return float(min(max(r_sz, min_sz), 450.0))
    elif "shad" in layer_lower:
        # 陰影: 細部シェーディング（チーク、鼻下、二重まぶた等 r_sz <= 25px）はそのまま尊重
        if r_sz <= 25.0:
            return float(min(max(r_sz, 1.5), 60.0))
        min_sz = max(15.0, min_dim * 0.01)
        return float(min(max(r_sz, min_sz), 250.0))
    elif "line" in layer_lower:
        # 主線・ディテール線: 極細ペン（1.0〜4.0px）は高解像度でも強制底上げせず完全に尊重！
        if r_sz <= 5.0:
            return float(max(1.0, r_sz))
        max_line_sz = max(12.0, min_dim * 0.012)
        return float(min(max(r_sz, 1.0), max_line_sz))
    else:  # Highlights, FX, etc.
        # ハイライト・粒子: 1.0px 以上の極細ハイライト点を尊重
        if r_sz <= 5.0:
            return float(max(1.0, r_sz))
        max_hl_sz = max(15.0, min_dim * 0.02)
        return float(min(max(r_sz, 1.0), max_hl_sz))


def _validate_and_sanitize_plan(
    plan: DrawingPlan,
    prompt: str,
    seed: int,
    count: int | None,
    width: float,
    height: float,
    iteration: int | None = None,
    log_func: Callable[[str], None] | None = None,
) -> DrawingPlan:
    """LLM の応答を堅牢にサニタイズし、Catmull-Rom スプライン平滑化・筆圧テーパリング・解像度適応を実行する。"""
    # 1. prompt, seed の補正
    safe_prompt = prompt if not plan.prompt.strip() or plan.prompt != prompt else plan.prompt
    safe_seed = seed if plan.seed == 0 and seed != 0 else plan.seed

    if not plan.strokes:
        raise LLMPlannerError("LLM からストロークが 1 本も返されませんでした")

    # 2. 座標系の自動検出 (正規化座標 0.0〜1.0 のキャンバス解像度スケーリング)
    all_x = [p.x for s in plan.strokes for p in s.points]
    all_y = [p.y for s in plan.strokes for p in s.points]
    max_x = max(all_x) if all_x else 0.0
    max_y = max(all_y) if all_y else 0.0

    is_normalized = (max_x <= 1.05 and max_y <= 1.05) and (width > 10.0 and height > 10.0)
    scale_x = width if is_normalized else 1.0
    scale_y = height if is_normalized else 1.0

    if is_normalized and log_func is not None:
        log_func(
            f"通知: 正規化座標 (0.0〜1.0) を検出したためキャンバスサイズ ({width:.0f}x{height:.0f}) に自動変換します"
        )

    sanitized_strokes: list[Stroke] = []
    stroke_id_set: set[str] = set()
    strokes_source = plan.strokes[:count] if count is not None and count > 0 else plan.strokes

    for idx, stroke in enumerate(strokes_source, start=1):
        st_id = stroke.id if stroke.id and stroke.id not in stroke_id_set else f"stroke_{idx}"
        stroke_id_set.add(st_id)

        # 制御点のバウンディング & 初期サニタイズ
        raw_pts: list[StrokePoint] = []
        last_time = 0
        for p_idx, pt in enumerate(stroke.points[:150]):
            px = min(max(0.0, pt.x * scale_x), max(1.0, width - 0.5))
            py = min(max(0.0, pt.y * scale_y), max(1.0, height - 0.5))
            pressure = min(max(0.05, pt.pressure), 1.0)
            time_ms = max(last_time, pt.time_ms) if pt.time_ms > 0 else (0 if p_idx == 0 else last_time + 15)
            last_time = time_ms
            raw_pts.append(StrokePoint(x=px, y=py, pressure=pressure, time_ms=time_ms))

        if not raw_pts:
            continue

        layer = stroke.layer_name.strip() if stroke.layer_name and stroke.layer_name.strip() else "Lineart"
        size_px = _adaptive_stroke_size(stroke.size_px, layer, width, height)

        # Catmull-Rom スプライン平滑化 & 筆圧テーパリング & 適応的サンプリング
        smoothed_pts = _smooth_and_densify_points(
            raw_points=raw_pts,
            width=width,
            height=height,
            layer_name=layer,
            size_px=size_px,
        )

        if len(smoothed_pts) < 2:
            continue

        # カラー & サイズ & 不透明度のサニタイズ
        color = (
            stroke.color
            if re.match(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$", stroke.color)
            else "#232323"
        )
        opacity = min(max(0.05, stroke.opacity), 1.0)
        preset_name = stroke.brush_preset or "Basic-5 Size"
        is_eraser = bool(
            getattr(stroke, "is_eraser", False) or "eraser" in preset_name.lower() or layer.lower() == "eraser"
        )

        sanitized_strokes.append(
            Stroke(
                id=st_id,
                points=smoothed_pts,
                brush_preset=preset_name,
                color=color,
                size_px=size_px,
                layer_name=layer,
                opacity=opacity,
                is_eraser=is_eraser,
            )
        )

    if not sanitized_strokes:
        raise LLMPlannerError("有効なストロークを構築できませんでした")

    metadata_val = dict(plan.metadata) if isinstance(plan.metadata, Mapping) else {}
    goal_reached = bool(getattr(plan, "goal_reached", False) or metadata_val.get("goal_reached", False) is True)
    completion_score = float(
        getattr(plan, "completion_score", 1.0)
        if getattr(plan, "completion_score", None) is not None
        else metadata_val.get("completion_score", 1.0)
    )
    metadata_val["goal_reached"] = goal_reached
    metadata_val["completion_score"] = completion_score

    return DrawingPlan(
        prompt=safe_prompt,
        seed=safe_seed,
        strokes=sanitized_strokes,
        title=plan.title or f"AI Artwork - {safe_prompt[:20]}",
        iteration=plan.iteration if iteration is None else iteration,
        layers=plan.layers,
        request_canvas_image=getattr(plan, "request_canvas_image", False),
        metadata=metadata_val,
        canvas_width=width,
        canvas_height=height,
        goal_reached=goal_reached,
        completion_score=completion_score,
    )


def _redact_sensitive_text(text: str) -> str:
    """通信エラーやデバッグプレビューに混入した資格情報を伏せる。"""
    redacted = re.sub(r"(?i)\b(https?://)[^/@\s]+@", r"\1[REDACTED]@", text)
    redacted = re.sub(r"(?i)\bBearer\s+[A-Za-z0-9._~+/=-]+", "Bearer [REDACTED]", redacted)
    redacted = re.sub(r"\bsk-[A-Za-z0-9_-]{8,}\b", "sk-[REDACTED]", redacted)
    redacted = re.sub(
        r"(?i)(\b(?:api[_ -]?key|authorization)\b\s*[:=]\s*)([\"'])(.*?)(\2)",
        r"\1\2[REDACTED]\2",
        redacted,
    )
    redacted = re.sub(
        r"(?i)(\b(?:api[_ -]?key|authorization)\b\s*[:=]\s*)(?![\"'])[^\s,;}\]]+",
        r"\1[REDACTED]",
        redacted,
    )
    return redacted


def _read_http_error(error: HTTPError) -> str:
    try:
        body = error.read(2000).decode("utf-8", errors="replace").strip()
    except OSError:
        return ""
    finally:
        with contextlib.suppress(Exception):
            error.close()
    return _redact_sensitive_text(" ".join(body.split()))[:2000]


def _safe_error_message(error: BaseException) -> str:
    return _redact_sensitive_text(" ".join(str(error).split()))[:500] or error.__class__.__name__
