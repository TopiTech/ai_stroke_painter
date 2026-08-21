"""OpenAI Chat Completions 互換エンドポイント用のマルチモーダル・マルチレイヤー DrawingPlan Adapter。"""

from __future__ import annotations

import base64
from collections.abc import Callable, Mapping
from dataclasses import dataclass
import datetime
import json
import math
import re
import socket
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint
from .planner import validate_plan_request
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


@dataclass(frozen=True)
class OpenAICompatibleSettings:
    """キーを永続化しない、1 回の API 呼び出しに必要な接続設定。"""

    base_url: str
    model: str
    api_key: str = ""
    timeout_seconds: float = 120.0
    max_tokens: int = 8192

    def __post_init__(self) -> None:
        if not isinstance(self.base_url, str) or not self.base_url.strip():
            raise ValueError("Base URL を入力してください")
        parsed = urlsplit(self.base_url.strip())
        if parsed.scheme not in {"http", "https"} or not parsed.netloc or parsed.query or parsed.fragment:
            raise ValueError("Base URL はクエリを含まない http(s) URL にしてください")
        if not isinstance(self.model, str) or not self.model.strip():
            raise ValueError("Model を入力してください")
        if (
            isinstance(self.timeout_seconds, bool)
            or not isinstance(self.timeout_seconds, (int, float))
            or self.timeout_seconds <= 0
            or not math.isfinite(self.timeout_seconds)
        ):
            raise ValueError("timeout_seconds は正の有限数値である必要があります")
        if isinstance(self.max_tokens, bool) or not isinstance(self.max_tokens, int) or self.max_tokens <= 0:
            raise ValueError("max_tokens は正の整数である必要があります")

    @property
    def endpoint_url(self) -> str:
        """Base URL から OpenAI 互換の chat/completions エンドポイント URL をスマートに導出する。"""
        base = self.base_url.strip().rstrip("/")
        if base.endswith("/chat/completions"):
            return base
        if base.endswith("/v1"):
            return f"{base}/chat/completions"
        # Ollama, LM Studio, OpenAI などの主要ホストで /v1 が省略されている場合は自動補完
        lower = base.lower()
        if "/v1" not in lower and any(
            h in lower
            for h in ("api.openai.com", "localhost", "127.0.0.1", "groq.com", "openrouter.ai", "deepseek.com")
        ):
            return f"{base}/v1/chat/completions"
        return f"{base}/chat/completions"


class OpenAICompatiblePlanner(PlannerPort):
    """OpenAI 互換の ``POST /chat/completions`` をマルチモーダル & 自律ビジョン Planner として利用する。"""

    MAX_RESPONSE_BYTES = 15_000_000

    def __init__(
        self,
        settings: OpenAICompatibleSettings,
        opener: Callable[..., Any] | None = None,
        log_callback: Callable[[str], None] | None = None,
    ) -> None:
        self.settings = settings
        self._opener = opener or build_opener(_SameOriginRedirectHandler()).open
        self.log_callback = log_callback

    def _log(self, message: str) -> None:
        if self.log_callback is not None:
            ts = datetime.datetime.now().strftime("%H:%M:%S")
            self.log_callback(f"[{ts}] {message}")

    def test_connection(self) -> str:
        """API 接続疎通確認を行う。"""
        self._log(f"API 接続テスト開始: {self.settings.endpoint_url} (Model: {self.settings.model})")
        payload = {
            "model": self.settings.model.strip(),
            "messages": [{"role": "user", "content": "Ping"}],
            "max_tokens": 10,
        }
        start = time.perf_counter()
        response = self._post(payload)
        elapsed = time.perf_counter() - start
        choices = response.get("choices", [])
        if choices:
            msg = f"接続成功: モデルが正常に応答しました ({elapsed:.2f}s)"
            self._log(msg)
            return msg
        msg = f"応答を受信しましたが choices が空でした ({elapsed:.2f}s)"
        self._log(msg)
        return msg

    def plan(
        self,
        prompt: str,
        seed: int,
        count: int,
        width: float,
        height: float,
        image_data: bytes | None = None,
        canvas_image: bytes | None = None,
        iteration: int = 1,
        max_iterations: int = 1,
        palette_name: str = "anime",
    ) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height
        )

        self._log(
            f"--- 描画計画生成開始 (Iteration {iteration}/{max_iterations}) ---\n"
            f"Prompt: {valid_prompt!r}, Seed: {valid_seed}, Count: {valid_count}, Canvas: {valid_width}x{valid_height}\n"
            f"Endpoint: {self.settings.endpoint_url}, Model: {self.settings.model}, Timeout: {self.settings.timeout_seconds}s"
        )

        user_content_parts: list[dict[str, Any]] = []

        # 構造化テキスト指示
        req_dict = {
            "prompt": valid_prompt,
            "seed": valid_seed,
            "stroke_count": valid_count,
            "canvas": {"width": valid_width, "height": valid_height},
            "palette": palette_name,
            "iteration": iteration,
            "max_iterations": max_iterations,
            "goal": "Generate professional-grade multi-layer illustration strokes in valid JSON format.",
        }
        req_json = json.dumps(req_dict, ensure_ascii=False)
        user_content_parts.append({"type": "text", "text": req_json})

        # 参照画像 (Base64) の添付
        if image_data:
            self._log(f"参照画像を添付します ({len(image_data)} bytes)")
            b64_ref = base64.b64encode(image_data).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{b64_ref}", "detail": "high"},
                }
            )

        # 現在のキャンバスキャプチャ (Base64) の添付 (自律改善ループ時)
        if canvas_image:
            self._log(f"現在のキャンバス状態を添付します ({len(canvas_image)} bytes)")
            b64_canvas = base64.b64encode(canvas_image).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{b64_canvas}", "detail": "high"},
                }
            )

        user_content: Any = user_content_parts if len(user_content_parts) > 1 else req_json

        payload: dict[str, Any] = {
            "model": self.settings.model.strip(),
            "messages": [
                {"role": "system", "content": _system_instruction(iteration, max_iterations)},
                {"role": "user", "content": user_content},
            ],
            "response_format": {"type": "json_object"},
            "max_tokens": self.settings.max_tokens,
            "temperature": 0.7,
        }

        self._log(f"LLM API へリクエスト送信中 ({self.settings.endpoint_url})...")
        try:
            response = self._post(payload)
        except LLMPlannerError as exc:
            # もし response_format が原因で 400 が返った場合はフォールバック送信
            if "response_format" in payload and "400" in str(exc):
                self._log("response_format を除外してリトライ送信します...")
                fallback_payload = dict(payload)
                del fallback_payload["response_format"]
                response = self._post(fallback_payload)
            else:
                raise

        self._log("LLM API 応答受信。JSON パースとストローク構築を実行中...")

        plan = _plan_from_response(response, log_func=self._log)
        sanitized_plan = _validate_and_sanitize_plan(
            plan=plan,
            prompt=valid_prompt,
            seed=valid_seed,
            count=valid_count,
            width=valid_width,
            height=valid_height,
            log_func=self._log,
        )

        total_pts = sum(len(s.points) for s in sanitized_plan.strokes)
        layers_str = ", ".join(sanitized_plan.layers)
        self._log(
            f"描画計画生成成功: ストローク数={len(sanitized_plan.strokes)}, 総点数={total_pts}, レイヤー=[{layers_str}]"
        )
        return sanitized_plan

    def _post(self, payload: Mapping[str, Any]) -> Mapping[str, Any]:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "AIStrokePainter/1.0",
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
            with self._opener(request, timeout=float(self.settings.timeout_seconds)) as response:
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
        except (URLError, socket.timeout, TimeoutError, OSError) as exc:
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
            self._log(f"JSON デコードエラー: {exc}\n応答先頭 500 文字: {raw[:500]!r}")
            raise LLMPlannerError("LLM API が JSON 応答を返しませんでした") from exc

        if not isinstance(decoded, Mapping):
            raise LLMPlannerError("LLM API の応答は JSON オブジェクトである必要があります")

        return decoded


def _system_instruction(iteration: int = 1, max_iterations: int = 1) -> str:
    phase_guide = (
        "Phase 1: Rough draft and anatomical gesture/composition lines."
        if iteration == 1 and max_iterations > 1
        else "Phase: Refined clean lineart, flat colors, shading hatchings, and specular highlights."
    )
    return (
        "You are an expert master artist and drawing-plan director. Return exactly one valid JSON object (no extra commentary). "
        f"{phase_guide} "
        "The schema must strictly be:\n"
        "{\n"
        '  "schema_version": 1,\n'
        '  "prompt": string,\n'
        '  "seed": integer,\n'
        '  "title": string,\n'
        '  "iteration": integer,\n'
        '  "layers": ["Draft", "Lineart", "Flats", "Shading", "Highlights", "FX"],\n'
        '  "strokes": [\n'
        "    {\n"
        '      "id": string,\n'
        '      "brush_preset": "Basic-5 Size",\n'
        '      "color": "#RRGGBB",\n'
        '      "size_px": number (e.g. 2.0 to 15.0),\n'
        '      "layer_name": string,\n'
        '      "opacity": number (0.1 to 1.0),\n'
        '      "points": [\n'
        '        {"x": number, "y": number, "pressure": number (0.05 to 1.0), "time_ms": integer},\n'
        "        ...\n"
        "      ]\n"
        "    }\n"
        "  ]\n"
        "}\n"
        "Requirements:\n"
        "1. Layer names must be logical: 'Draft', 'Lineart', 'Flats', 'Shading', 'Highlights', or 'FX'.\n"
        "2. Colors must be hex #RRGGBB or #RGB.\n"
        "3. Coordinates x, y must be within the canvas dimensions [0, width] and [0, height].\n"
        "4. Pressure must be dynamic (0.05 to 1.0) simulating natural pen pressure.\n"
        "5. Each stroke must contain at least 2 points forming a smooth continuous curve."
    )


def _plan_from_response(response: Mapping[str, Any], log_func: Callable[[str], None] | None = None) -> DrawingPlan:
    try:
        choices = response["choices"]
        first_choice = choices[0]
        content = first_choice["message"]["content"]
    except (KeyError, IndexError, TypeError) as exc:
        raise LLMPlannerError("LLM API 応答に choices[0].message.content がありません") from exc

    if isinstance(content, list):
        content = "".join(part.get("text", "") for part in content if isinstance(part, Mapping))

    if not isinstance(content, str) or not content.strip():
        raise LLMPlannerError("LLM の応答本文が空です")

    if log_func is not None:
        log_preview = content[:200] + ("..." if len(content) > 200 else "")
        log_func(f"LLM 応答テキスト抽出 (長さ: {len(content)} 文字):\n{log_preview}")

    try:
        value = _extract_json_object(content, log_func=log_func)
        return DrawingPlan.from_dict(value)
    except (PlanValidationError, json.JSONDecodeError, TypeError, ValueError) as exc:
        raise LLMPlannerError(f"LLM が有効な DrawingPlan JSON を返しませんでした: {exc}") from exc


def _clean_thinking_tokens(text: str) -> str:
    """<think>...</think> や <thought>...</thought> などの思考プロセスを除去する。"""
    cleaned = re.sub(r"<(?:think|thought)>[\s\S]*?</(?:think|thought)>", "", text, flags=re.IGNORECASE)
    return cleaned.strip()


def _extract_json_object(content: str, log_func: Callable[[str], None] | None = None) -> Mapping[str, Any]:
    text = _clean_thinking_tokens(content.strip())

    # 1. コードブロック ```json ... ``` の抽出
    fence_match = re.search(r"```(?:json)?\s*([\s\S]*?)\s*```", text)
    candidate = fence_match.group(1).strip() if fence_match is not None else text

    # 2. 通常の json.loads 試行
    try:
        value = json.loads(candidate)
        if isinstance(value, Mapping):
            return value
    except json.JSONDecodeError:
        pass

    # 3. 最外郭 { ... } の探索と raw_decode
    start = candidate.find("{")
    if start >= 0:
        try:
            value, _ = json.JSONDecoder().raw_decode(candidate[start:])
            if isinstance(value, Mapping):
                return value
        except json.JSONDecodeError:
            pass

    # 4. 全体テキストからのフォールバック探索
    if candidate is not text:
        start = text.find("{")
        if start >= 0:
            try:
                value, _ = json.JSONDecoder().raw_decode(text[start:])
                if isinstance(value, Mapping):
                    return value
            except json.JSONDecodeError:
                pass

    # 5. 途中で途切れた JSON の末尾修復の試行
    repaired = _attempt_json_repair(candidate)
    if repaired is not None and isinstance(repaired, Mapping):
        if log_func is not None:
            log_func("警告: 途切れた JSON を自動修復して読み込みました")
        return repaired

    raise json.JSONDecodeError("DrawingPlan JSON オブジェクトを抽出できませんでした", text, 0)


def _attempt_json_repair(text: str) -> Mapping[str, Any] | None:
    """トークン上限等で末尾が切れた JSON の簡易自動修復を行う。"""
    start = text.find("{")
    if start < 0:
        return None
    s = text[start:].strip()

    # 開いている括弧のバランスを補完
    for suffix in (
        '"]}]}',
        '"}]}',
        "]}",
        "}",
        "}]}",
        '0,"time_ms":0}]}]}',
    ):
        try:
            val = json.loads(s + suffix)
            if isinstance(val, Mapping) and "strokes" in val:
                return val
        except json.JSONDecodeError:
            continue
    return None


def _validate_and_sanitize_plan(
    plan: DrawingPlan,
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
    log_func: Callable[[str], None] | None = None,
) -> DrawingPlan:
    """LLM の応答を堅牢にサニタイズし、契約違反（範囲外、正規化座標、属性欠落等）を自動修正する。"""
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

    for idx, stroke in enumerate(plan.strokes[: max(count, 500)], start=1):
        # ストローク ID の一意性確保
        st_id = stroke.id if stroke.id and stroke.id not in stroke_id_set else f"stroke_{idx}"
        stroke_id_set.add(st_id)

        # 点のサニタイズ
        sanitized_pts: list[StrokePoint] = []
        last_time = 0
        for p_idx, pt in enumerate(stroke.points[:150]):
            px = min(max(0.0, pt.x * scale_x), max(1.0, width - 0.5))
            py = min(max(0.0, pt.y * scale_y), max(1.0, height - 0.5))
            pressure = min(max(0.05, pt.pressure), 1.0)
            time_ms = max(last_time, pt.time_ms) if pt.time_ms > 0 else p_idx * 15
            last_time = time_ms
            sanitized_pts.append(StrokePoint(x=px, y=py, pressure=pressure, time_ms=time_ms))

        # 点数が 2 点未満の場合は補完
        if len(sanitized_pts) == 1:
            p0 = sanitized_pts[0]
            sanitized_pts.append(
                StrokePoint(
                    x=min(width - 0.1, p0.x + 1.0),
                    y=min(height - 0.1, p0.y + 1.0),
                    pressure=p0.pressure,
                    time_ms=p0.time_ms + 10,
                )
            )
        elif len(sanitized_pts) == 0:
            continue

        # カラーのサニタイズ
        color = stroke.color if re.match(r"^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$", stroke.color) else "#232323"
        layer = stroke.layer_name.strip() if stroke.layer_name and stroke.layer_name.strip() else "Lineart"
        size_px = min(max(0.5, stroke.size_px), 200.0)
        opacity = min(max(0.05, stroke.opacity), 1.0)

        sanitized_strokes.append(
            Stroke(
                id=st_id,
                points=sanitized_pts,
                brush_preset=stroke.brush_preset or "Basic-5 Size",
                color=color,
                size_px=size_px,
                layer_name=layer,
                opacity=opacity,
            )
        )

    if not sanitized_strokes:
        raise LLMPlannerError("有効なストロークを構築できませんでした")

    return DrawingPlan(
        prompt=safe_prompt,
        seed=safe_seed,
        strokes=sanitized_strokes,
        title=plan.title or f"AI Artwork - {safe_prompt[:20]}",
        iteration=plan.iteration,
        layers=plan.layers,
        metadata=plan.metadata,
    )


def _read_http_error(error: HTTPError) -> str:
    try:
        body = error.read(2000).decode("utf-8", errors="replace").strip()
    except OSError:
        return ""
    return " ".join(body.split())[:2000]


def _safe_error_message(error: BaseException) -> str:
    return " ".join(str(error).split())[:500] or error.__class__.__name__
