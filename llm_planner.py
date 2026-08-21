"""OpenAI Chat Completions 互換エンドポイント用のマルチモーダル・マルチレイヤー DrawingPlan Adapter。"""

from __future__ import annotations

import base64
from collections.abc import Callable, Mapping
from dataclasses import dataclass
import json
import re
import socket
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .domain import DrawingPlan, PlanValidationError
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
    timeout_seconds: float = 60.0

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
        ):
            raise ValueError("timeout_seconds は正の数値である必要があります")

    @property
    def endpoint_url(self) -> str:
        base = self.base_url.strip().rstrip("/")
        return base if base.endswith("/chat/completions") else f"{base}/chat/completions"


class OpenAICompatiblePlanner(PlannerPort):
    """OpenAI 互換の ``POST /chat/completions`` をマルチモーダル & 自律ビジョン Planner として利用する。"""

    MAX_RESPONSE_BYTES = 10_000_000

    def __init__(
        self,
        settings: OpenAICompatibleSettings,
        opener: Callable[..., Any] | None = None,
    ) -> None:
        self.settings = settings
        self._opener = opener or build_opener(_SameOriginRedirectHandler()).open

    def test_connection(self) -> str:
        """API 接続疎通確認を行う。"""
        payload = {
            "model": self.settings.model.strip(),
            "messages": [{"role": "user", "content": "Ping"}],
            "max_tokens": 5,
        }
        response = self._post(payload)
        choices = response.get("choices", [])
        if choices:
            return "接続成功: モデルが正常に応答しました"
        return "応答を受信しましたが choices が空でした"

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

        user_content_parts: list[dict[str, Any]] = []

        # 構造化テキスト指示
        req_json = json.dumps(
            {
                "prompt": valid_prompt,
                "seed": valid_seed,
                "stroke_count": valid_count,
                "canvas": {"width": valid_width, "height": valid_height},
                "iteration": iteration,
                "max_iterations": max_iterations,
                "goal": "Generate professional-grade multi-layer illustration strokes.",
            },
            ensure_ascii=False,
        )
        user_content_parts.append({"type": "text", "text": req_json})

        # 参照画像 (Base64) の添付
        if image_data:
            b64_ref = base64.b64encode(image_data).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{b64_ref}", "detail": "high"},
                }
            )

        # 現在のキャンバスキャプチャ (Base64) の添付 (自律改善ループ時)
        if canvas_image:
            b64_canvas = base64.b64encode(canvas_image).decode("ascii")
            user_content_parts.append(
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/png;base64,{b64_canvas}", "detail": "high"},
                }
            )

        payload = {
            "model": self.settings.model.strip(),
            "messages": [
                {"role": "system", "content": _system_instruction(iteration, max_iterations)},
                {"role": "user", "content": user_content_parts if len(user_content_parts) > 1 else req_json},
            ],
        }

        response = self._post(payload)
        plan = _plan_from_response(response)
        _validate_plan_contract(plan, valid_prompt, valid_seed, valid_count, valid_width, valid_height)
        return plan

    def _post(self, payload: Mapping[str, Any]) -> Mapping[str, Any]:
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if self.settings.api_key.strip():
            headers["Authorization"] = f"Bearer {self.settings.api_key.strip()}"
        request = Request(
            self.settings.endpoint_url,
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with self._opener(request, timeout=float(self.settings.timeout_seconds)) as response:
                raw = response.read(self.MAX_RESPONSE_BYTES + 1)
        except _CrossOriginRedirectError as exc:
            raise LLMPlannerError("LLM API の別オリジンへのリダイレクトを拒否しました") from exc
        except HTTPError as exc:
            detail = _read_http_error(exc)
            suffix = f": {detail}" if detail else ""
            raise LLMPlannerError(f"LLM API が HTTP {exc.code} を返しました{suffix}") from exc
        except (URLError, socket.timeout, TimeoutError, OSError) as exc:
            raise LLMPlannerError(f"LLM API に接続できませんでした: {_safe_error_message(exc)}") from exc

        if len(raw) > self.MAX_RESPONSE_BYTES:
            raise LLMPlannerError("LLM API の応答が大きすぎます")
        try:
            decoded = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
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
        "You are an expert master artist and drawing-plan director. Return exactly one valid JSON object. "
        f"{phase_guide} "
        "The schema must strictly be: "
        '{"schema_version":1,"prompt":string,"seed":integer,"title":string,"iteration":integer,"layers":[string,...],'
        '"strokes":[{"id":string,"brush_preset":string,"color":string,"size_px":number,"layer_name":string,'
        '"opacity":number,"points":[{"x":number,"y":number,"pressure":number,"time_ms":integer},...]}]}. '
        "Requirements:\n"
        "1. Layer names must be logical: 'Draft', 'Lineart', 'Flats', 'Shading', 'Highlights', or 'FX'.\n"
        "2. Colors must be hex #RRGGBB or #RGB.\n"
        "3. Size_px should vary with significance (e.g. 5-8px for main outlines, 2-3px for hatching/eyes, 8-15px for broad fills).\n"
        "4. Pressure must be smooth and dynamic (0.05 to 1.0) simulating pen pressure dynamics.\n"
        "5. Keep all x within [0, width) and all y within [0, height).\n"
        "6. Return strictly requested stroke_count or slightly below."
    )


def _plan_from_response(response: Mapping[str, Any]) -> DrawingPlan:
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
    try:
        value = _extract_json_object(content)
        return DrawingPlan.from_dict(value)
    except (PlanValidationError, json.JSONDecodeError, TypeError, ValueError) as exc:
        raise LLMPlannerError(f"LLM が有効な DrawingPlan JSON を返しませんでした: {exc}") from exc


def _extract_json_object(content: str) -> Mapping[str, Any]:
    text = content.strip()
    fence_match = re.search(r"```(?:json)?\s*([\s\S]*?)\s*```", text)
    candidate = fence_match.group(1).strip() if fence_match is not None else text

    try:
        value = json.loads(candidate)
    except json.JSONDecodeError:
        start = candidate.find("{")
        if start >= 0:
            try:
                value, _ = json.JSONDecoder().raw_decode(candidate[start:])
            except json.JSONDecodeError:
                value = None
        else:
            value = None

    if value is None and candidate is not text:
        start = text.find("{")
        if start >= 0:
            value, _ = json.JSONDecoder().raw_decode(text[start:])
        else:
            raise json.JSONDecodeError("JSON オブジェクトが見つかりません", text, 0)
    elif value is None:
        raise json.JSONDecodeError("JSON オブジェクトが見つかりません", text, 0)

    if not isinstance(value, Mapping):
        raise ValueError("DrawingPlan は JSON オブジェクトである必要があります")
    return value


def _validate_plan_contract(
    plan: DrawingPlan,
    prompt: str,
    seed: int,
    count: int,
    width: float,
    height: float,
) -> None:
    if plan.prompt != prompt or plan.seed != seed:
        raise LLMPlannerError("LLM は要求した prompt と seed をそのまま返す必要があります")
    if len(plan.strokes) > max(count, 500):
        raise LLMPlannerError(f"LLM のストローク数が上限を超えています: {len(plan.strokes)}")
    point_count = 0
    for stroke in plan.strokes:
        if len(stroke.points) > 150:
            raise LLMPlannerError("1 ストロークの点数は 150 以下にしてください")
        point_count += len(stroke.points)
        for point in stroke.points:
            if not 0.0 <= point.x < width or not 0.0 <= point.y < height:
                raise LLMPlannerError("LLM の点座標がキャンバス範囲外です")
    if point_count > 30_000:
        raise LLMPlannerError("LLM の総点数が多すぎます")


def _read_http_error(error: HTTPError) -> str:
    try:
        body = error.read(1000).decode("utf-8", errors="replace").strip()
    except OSError:
        return ""
    return " ".join(body.split())[:1000]


def _safe_error_message(error: BaseException) -> str:
    return " ".join(str(error).split())[:500] or error.__class__.__name__
