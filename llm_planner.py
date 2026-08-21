"""OpenAI Chat Completions 互換エンドポイント用の DrawingPlan Adapter。"""

from __future__ import annotations

from dataclasses import dataclass
import json
import socket
from typing import Any, Callable, Mapping, Optional
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


def _url_origin(url: str):
    parsed = urlsplit(url)
    try:
        port = parsed.port
    except ValueError:
        return None
    if port is None:
        port = 443 if parsed.scheme.lower() == "https" else 80
    return parsed.scheme.lower(), (parsed.hostname or "").lower(), port


class _SameOriginRedirectHandler(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        redirected = super().redirect_request(req, fp, code, msg, headers, newurl)
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
    timeout_seconds: float = 45.0

    def __post_init__(self) -> None:
        if not isinstance(self.base_url, str) or not self.base_url.strip():
            raise ValueError("Base URL を入力してください")
        parsed = urlsplit(self.base_url.strip())
        if parsed.scheme not in {"http", "https"} or not parsed.netloc or parsed.query or parsed.fragment:
            raise ValueError("Base URL はクエリを含まない http(s) URL にしてください")
        if not isinstance(self.model, str) or not self.model.strip():
            raise ValueError("Model を入力してください")
        if isinstance(self.timeout_seconds, bool) or not isinstance(self.timeout_seconds, (int, float)) or self.timeout_seconds <= 0:
            raise ValueError("timeout_seconds は正の数値である必要があります")

    @property
    def endpoint_url(self) -> str:
        base = self.base_url.strip().rstrip("/")
        return base if base.endswith("/chat/completions") else base + "/chat/completions"


class OpenAICompatiblePlanner(PlannerPort):
    """OpenAI 互換の ``POST /chat/completions`` を PlannerPort として利用する。"""

    MAX_RESPONSE_BYTES = 5_000_000

    def __init__(
        self,
        settings: OpenAICompatibleSettings,
        opener: Optional[Callable[..., Any]] = None,
    ) -> None:
        self.settings = settings
        self._opener = opener or build_opener(_SameOriginRedirectHandler()).open

    def plan(self, prompt, seed, count, width, height):
        prompt, seed, count, width, height = validate_plan_request(prompt, seed, count, width, height)
        payload = {
            "model": self.settings.model.strip(),
            "messages": [
                {"role": "system", "content": _system_instruction()},
                {
                    "role": "user",
                    "content": json.dumps(
                        {
                            "prompt": prompt,
                            "seed": seed,
                            "stroke_count": count,
                            "canvas": {"width": width, "height": height},
                        },
                        ensure_ascii=False,
                    ),
                },
            ],
        }
        response = self._post(payload)
        plan = _plan_from_response(response)
        _validate_plan_contract(plan, prompt, seed, count, width, height)
        return plan

    def _post(self, payload: Mapping[str, Any]) -> Mapping[str, Any]:
        headers = {"Content-Type": "application/json", "Accept": "application/json"}
        if self.settings.api_key.strip():
            headers["Authorization"] = "Bearer " + self.settings.api_key.strip()
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
            raise LLMPlannerError("LLM API が HTTP %d を返しました%s" % (exc.code, (": " + detail) if detail else "")) from exc
        except (URLError, socket.timeout, TimeoutError, OSError) as exc:
            raise LLMPlannerError("LLM API に接続できませんでした: %s" % _safe_error_message(exc)) from exc

        if len(raw) > self.MAX_RESPONSE_BYTES:
            raise LLMPlannerError("LLM API の応答が大きすぎます")
        try:
            decoded = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise LLMPlannerError("LLM API が JSON 応答を返しませんでした") from exc
        if not isinstance(decoded, Mapping):
            raise LLMPlannerError("LLM API の応答は JSON オブジェクトである必要があります")
        return decoded


def _system_instruction() -> str:
    return (
        "You are a drawing-plan generator. Return exactly one JSON object and no Markdown. "
        "The object must follow this schema: "
        '{"schema_version":1,"prompt":string,"seed":integer,"strokes":[stroke,...]}. '
        "Each stroke has a unique string id, an array of 2 to 120 points, brush_preset, "
        "color (#RRGGBB), and positive size_px. Each point has finite x, y, pressure "
        "(0.0 to 1.0), and nondecreasing integer time_ms. Use exactly the requested "
        "stroke_count. Copy prompt and seed exactly. Keep every x within [0,width) and "
        "every y within [0,height)."
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
        raise LLMPlannerError("LLM が有効な DrawingPlan JSON を返しませんでした: %s" % exc) from exc


def _extract_json_object(content: str) -> Mapping[str, Any]:
    candidate = content.strip()
    if candidate.startswith("```"):
        first_newline = candidate.find("\n")
        if first_newline < 0 or not candidate.endswith("```"):
            raise json.JSONDecodeError("不完全な Markdown code fence", candidate, 0)
        candidate = candidate[first_newline + 1 : -3].strip()
    try:
        value = json.loads(candidate)
    except json.JSONDecodeError:
        start = candidate.find("{")
        if start < 0:
            raise
        value, _ = json.JSONDecoder().raw_decode(candidate[start:])
    if not isinstance(value, Mapping):
        raise ValueError("DrawingPlan は JSON オブジェクトである必要があります")
    return value


def _validate_plan_contract(plan: DrawingPlan, prompt: str, seed: int, count: int, width: float, height: float) -> None:
    if plan.prompt != prompt or plan.seed != seed:
        raise LLMPlannerError("LLM は要求した prompt と seed をそのまま返す必要があります")
    if len(plan.strokes) != count:
        raise LLMPlannerError("LLM は要求した本数 (%d) のストロークを返す必要があります" % count)
    point_count = 0
    for stroke in plan.strokes:
        if len(stroke.points) > 120:
            raise LLMPlannerError("1 ストロークの点数は 120 以下にしてください")
        point_count += len(stroke.points)
        for point in stroke.points:
            if not 0.0 <= point.x < width or not 0.0 <= point.y < height:
                raise LLMPlannerError("LLM の点座標がキャンバス範囲外です")
    if point_count > 10_000:
        raise LLMPlannerError("LLM の総点数が多すぎます")


def _read_http_error(error: HTTPError) -> str:
    try:
        body = error.read(1000).decode("utf-8", errors="replace").strip()
    except OSError:
        return ""
    return " ".join(body.split())[:1000]


def _safe_error_message(error: BaseException) -> str:
    return " ".join(str(error).split())[:500] or error.__class__.__name__
