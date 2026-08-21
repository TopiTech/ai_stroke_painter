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

        try:
            extracted = _extract_content_from_response(response, log_func=self._log)
            preview = str(extracted)[:60].replace("\n", " ")
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

        # 自動リカバリー付き計画生成ループ (最大 3 試行)
        max_attempts = 3
        last_error: Exception | None = None

        for attempt in range(1, max_attempts + 1):
            if attempt == 1:
                current_payload = dict(payload)
            elif attempt == 2:
                self._log("[自動リトライ 1/2] response_format を除外し、直接 JSON 出力を指定して再試行します...")
                current_payload = dict(payload)
                current_payload.pop("response_format", None)
                current_payload["messages"] = [
                    {
                        "role": "system",
                        "content": _system_instruction(iteration, max_iterations)
                        + "\nIMPORTANT: Do NOT output thinking/reasoning tags. Output ONLY the JSON inside ```json ``` codeblock.",
                    },
                    {"role": "user", "content": user_content},
                ]
                current_payload["temperature"] = 0.4
            else:
                self._log("[自動リトライ 2/2] temperature を最小化 (0.2) し、最速確定 JSON 出力モードで再試行します...")
                current_payload = dict(payload)
                current_payload.pop("response_format", None)
                current_payload["temperature"] = 0.2
                current_payload["messages"] = [
                    {
                        "role": "system",
                        "content": "Return ONLY valid JSON matching DrawingPlan schema. No thinking, no markdown wrapper, no extra text.",
                    },
                    {"role": "user", "content": user_content},
                ]

            try:
                response = self._post(current_payload)
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

            except LLMPlannerError as exc:
                last_error = exc
                err_str = str(exc)
                # 致命的な認証エラー（401/403/404 等）はリトライせず即座に例外を上げる
                if any(code in err_str for code in ("HTTP 401", "HTTP 403", "HTTP 404", "認証", "API key")):
                    raise
                if attempt < max_attempts:
                    self._log(f"警告: 試行 {attempt}/{max_attempts} でエラーが発生しました: {exc}")
                    time.sleep(0.5)
                else:
                    self._log(f"エラー: 全 {max_attempts} 回の試行が失敗しました。")
                    raise last_error from exc

        if last_error is not None:
            raise last_error
        raise LLMPlannerError("LLM 描画計画の生成に失敗しました")

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


def _extract_content_from_response(
    response: Mapping[str, Any],
    log_func: Callable[[str], None] | None = None,
) -> str | Mapping[str, Any]:
    """OpenAI, Ollama, Gemini, Anthropic, Direct JSON 等の多様な API レスポンスから安全・堅牢にコンテンツを抽出する。"""
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

    # 3. OpenAI Chat Completions 形式 (response["choices"])
    choices = response.get("choices")
    if choices is not None:
        if not isinstance(choices, list) or len(choices) == 0:
            refusal = response.get("refusal")
            if refusal:
                raise LLMPlannerError(f"モデルが出力を拒否しました: {refusal}")
            raise LLMPlannerError(
                "LLM API 応答の choices 配列が空です (安全フィルターまたはトークン上限の可能性があります)"
            )

        first_choice = choices[0]
        if isinstance(first_choice, Mapping):
            finish_reason = first_choice.get("finish_reason")
            message = first_choice.get("message")

            if isinstance(message, Mapping):
                # 拒否理由の確認
                refusal = message.get("refusal")
                if refusal:
                    raise LLMPlannerError(f"モデルが出力を拒否しました: {refusal}")

                # 本文コンテンツ
                content = message.get("content")
                if isinstance(content, list):
                    content = "".join(part.get("text", "") for part in content if isinstance(part, Mapping))

                if isinstance(content, str) and content.strip():
                    return content

                # Thinking / Reasoning モデル救済 (DeepSeek R1, QwQ, Gemini Flash Thinking 等)
                reasoning = message.get("reasoning_content") or message.get("reasoning") or message.get("thought")
                if isinstance(reasoning, str) and reasoning.strip():
                    if log_func is not None:
                        log_func("通知: content が空のため、reasoning_content (思考出力) から JSON 抽出を試みます")
                    return reasoning

                # Tool Calls 形式の救済
                tool_calls = message.get("tool_calls")
                if isinstance(tool_calls, list) and tool_calls:
                    first_tool = tool_calls[0]
                    if isinstance(first_tool, Mapping):
                        args = first_tool.get("function", {}).get("arguments")
                        if isinstance(args, str) and args.strip():
                            if log_func is not None:
                                log_func("通知: tool_calls 引数から JSON データを抽出しました")
                            return args

            # Legacy completions 形式 (choices[0].text)
            if "text" in first_choice and isinstance(first_choice["text"], str) and first_choice["text"].strip():
                return first_choice["text"]

            if finish_reason == "length":
                raise LLMPlannerError(
                    "LLM の最大トークン数上限に達したため本文が空になりました。max_tokens を増やすかストローク数を減らしてください"
                )
            if finish_reason == "content_filter":
                raise LLMPlannerError("安全フィルター (content_filter) によりモデル出力が遮断されました")

    # 4. Google Gemini Direct 形式 (candidates[0].content.parts[0].text)
    candidates = response.get("candidates")
    if isinstance(candidates, list) and candidates:
        first_candidate = candidates[0]
        if isinstance(first_candidate, Mapping):
            parts = first_candidate.get("content", {}).get("parts", [])
            if isinstance(parts, list) and parts:
                text = "".join(p.get("text", "") for p in parts if isinstance(p, Mapping))
                if text.strip():
                    return text

    # 5. Anthropic Direct 形式 (content[0].text または content 文字列)
    content_field = response.get("content")
    if isinstance(content_field, list) and content_field:
        text = "".join(p.get("text", "") for p in content_field if isinstance(p, Mapping))
        if text.strip():
            return text
    elif isinstance(content_field, str) and content_field.strip():
        return content_field

    # 6. Ollama Direct Chat 形式 (message.content)
    msg_field = response.get("message")
    if isinstance(msg_field, Mapping):
        c = msg_field.get("content")
        if isinstance(c, str) and c.strip():
            return c

    # 7. Ollama Direct Generate 形式 (response)
    resp_field = response.get("response")
    if isinstance(resp_field, str) and resp_field.strip():
        return resp_field

    # 8. Bedrock / Cohere / AWS 形式 (output.text)
    out_field = response.get("output")
    if isinstance(out_field, Mapping) and "text" in out_field:
        t = out_field["text"]
        if isinstance(t, str) and t.strip():
            return t

    # 抽出失敗時の診断情報
    available_keys = ", ".join(list(response.keys())[:8])
    if log_func is not None:
        log_func(f"エラー: 認識可能なコンテンツが見つかりませんでした (レスポンスキー: [{available_keys}])")
    raise LLMPlannerError(f"LLM API 応答から本文を抽出できませんでした (キー: [{available_keys}])")


def _plan_from_response(response: Mapping[str, Any], log_func: Callable[[str], None] | None = None) -> DrawingPlan:
    extracted = _extract_content_from_response(response, log_func=log_func)

    # すでに辞書オブジェクトとして得られている場合 (Direct JSON)
    if isinstance(extracted, Mapping):
        try:
            return DrawingPlan.from_dict(extracted)
        except (PlanValidationError, TypeError, ValueError) as exc:
            raise LLMPlannerError(f"LLM が有効な DrawingPlan JSON を返しませんでした: {exc}") from exc

    content = extracted
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
    # 閉じられていない <think> タグ（途中で切れた場合）も考慮
    cleaned = re.sub(r"<(?:think|thought)>[\s\S]*$", "", cleaned, flags=re.IGNORECASE)
    return cleaned.strip()


def _extract_json_object(content: str, log_func: Callable[[str], None] | None = None) -> Mapping[str, Any]:
    cleaned = _clean_thinking_tokens(content.strip())

    # 1. 思考タグ除去後のテキストからコードブロック ```json ... ``` の抽出
    for text_source in (cleaned, content):
        if not text_source.strip():
            continue
        fence_match = re.search(r"```(?:json)?\s*([\s\S]*?)\s*```", text_source)
        if fence_match is not None:
            candidate = fence_match.group(1).strip()
            try:
                value = json.loads(candidate)
                if isinstance(value, Mapping):
                    return value
            except json.JSONDecodeError:
                repaired = _attempt_json_repair(candidate)
                if repaired is not None and isinstance(repaired, Mapping):
                    if log_func is not None:
                        log_func("通知: コードブロック内の途切れた JSON を自動修復しました")
                    return repaired

    # 2. 思考タグ除去後のテキストから raw_decode
    if cleaned:
        try:
            value = json.loads(cleaned)
            if isinstance(value, Mapping):
                return value
        except json.JSONDecodeError:
            pass

        start = cleaned.find("{")
        if start >= 0:
            try:
                value, _ = json.JSONDecoder().raw_decode(cleaned[start:])
                if isinstance(value, Mapping):
                    return value
            except json.JSONDecodeError:
                pass

    # 3. 思考タグ内を含めた全体テキストからのフォールバック探索 (思考内に JSON を出力するモデル対策)
    start = content.find("{")
    if start >= 0:
        try:
            value, _ = json.JSONDecoder().raw_decode(content[start:])
            if isinstance(value, Mapping):
                if log_func is not None:
                    log_func("通知: 思考タグ内部から DrawingPlan JSON オブジェクトを直接救出しました")
                return value
        except json.JSONDecodeError:
            pass

    # 4. 途中で途切れた JSON の高度な末尾修復
    for text_target in (cleaned, content):
        if not text_target:
            continue
        repaired = _attempt_json_repair(text_target)
        if repaired is not None and isinstance(repaired, Mapping):
            if log_func is not None:
                log_func("警告: トークン上限等で途切れた JSON を自動修復して読み込みました")
            return repaired

    raise json.JSONDecodeError("DrawingPlan JSON オブジェクトを抽出できませんでした", content, 0)


def _attempt_json_repair(text: str) -> Mapping[str, Any] | None:
    """トークン上限等で末尾が切れた JSON のスタック解析および構文修復を行う。"""
    start = text.find("{")
    if start < 0:
        return None
    s = text[start:].strip()

    # 末尾の不完全なトークン（カンマ、中途半端なキー名や文字列）のトリミング
    # 例: ... "points": [{"x": 10, "y": 20, "pressure": 0.5, "time_
    s_cleaned = re.sub(r',\s*"[^"]*"?\s*:\s*[^,}\]]*$', "", s)
    s_cleaned = re.sub(r",\s*$", "", s_cleaned)

    # 1. 開き括弧スタック解析による自動バランシング
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
                return val
        except json.JSONDecodeError:
            pass

    # 2. 定型サフィックスによるフォールバック修復
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
                return val
        except json.JSONDecodeError:
            continue
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
