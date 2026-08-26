"""OpenAI Chat Completions 互換エンドポイント用のマルチモーダル StrokeProgram Adapter。

思考モデル（OpenAI o1 / o3-mini、DeepSeek R1、Gemini Flash Thinking、Claude Extended Thinking、QwQ 等）
におけるパラメータ非互換、思考タグ混入、トークン枯渇による途切れを堅牢に解決する。
"""

from __future__ import annotations

import ast
import base64
from collections.abc import Callable, Mapping, Sequence
import contextlib
from dataclasses import dataclass, replace
import datetime
import hashlib
import ipaddress
import json
import math
import re
import time
from typing import Any, TypeGuard
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener
import uuid

from .brushes import brush_preset_for_profile, canonical_brush_profile, infer_brush_profile
from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint
from .image_converter import MAX_ENCODED_IMAGE_BYTES, sanitize_reference_image
from .planner import validate_iterations, validate_plan_request
from .ports import PlannerPort
from .procedural.base import pressure_profile, recolor_strokes_to_palette, sample_strokes_by_priority
from .stroke_program import StrokeProgram, compile_stroke_program, normalize_hex_color

AUTO_LLM_STROKE_BUDGET = 500


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
            "deepseek-v3",
            "r1",
            "qwq",
            "qwen-2.5-coder",
            "thinking",
            "reasoning",
            "reasoner",
            "sonnet-3-7",
            "claude-3-7",
            "gemini-2.0-flash-thinking",
            "gemini-2.5",
            "gemini-thinking",
        )
    )


def _get_stroke_program_json_schema() -> dict[str, Any]:
    """OpenAI Structured Outputs (json_schema) 用の厳格な StrokeProgram スキーマ。"""
    point_schema = {
        "type": "array",
        "items": {"type": "number"},
        "minItems": 2,
        "maxItems": 3,
    }
    brush_schema = {
        "type": "object",
        "properties": {
            "profile": {"type": "string"},
            "color": {"type": "string"},
            "size": {"type": "number"},
            "size_mode": {"type": "string", "enum": ["ratio", "px"]},
            "opacity": {"type": "number"},
            "is_eraser": {"type": "boolean"},
            "preset_hint": {"type": ["string", "null"]},
        },
        "required": ["profile", "color", "size", "size_mode", "opacity", "is_eraser", "preset_hint"],
        "additionalProperties": False,
    }
    common_properties: dict[str, Any] = {
        "id": {"type": "string", "minLength": 1},
        "layer": {"type": "string", "minLength": 1},
        "brush": brush_schema,
    }
    operation_schemas = [
        {
            "type": "object",
            "properties": {
                **common_properties,
                "kind": {"type": "string", "enum": ["path"]},
                "points": {"type": "array", "items": point_schema, "minItems": 2},
                "closed": {"type": "boolean"},
                "smooth": {"type": "boolean"},
            },
            "required": ["kind", "id", "layer", "points", "brush", "closed", "smooth"],
            "additionalProperties": False,
        },
        {
            "type": "object",
            "properties": {
                **common_properties,
                "kind": {"type": "string", "enum": ["fill"]},
                "polygon": {"type": "array", "items": point_schema, "minItems": 3},
                "style": {
                    "type": "string",
                    "enum": ["wash", "scanline", "feathered", "contour", "radial", "directional"],
                },
                "spacing": {"type": "number"},
                "angle_deg": {"type": "number"},
            },
            "required": ["kind", "id", "layer", "polygon", "brush", "style", "spacing", "angle_deg"],
            "additionalProperties": False,
        },
        {
            "type": "object",
            "properties": {
                **common_properties,
                "kind": {"type": "string", "enum": ["hatch"]},
                "polygon": {"type": "array", "items": point_schema, "minItems": 3},
                "spacing": {"type": "number"},
                "angle_deg": {"type": "number"},
                "cross": {"type": "boolean"},
            },
            "required": ["kind", "id", "layer", "polygon", "brush", "spacing", "angle_deg", "cross"],
            "additionalProperties": False,
        },
        {
            "type": "object",
            "properties": {
                **common_properties,
                "kind": {"type": "string", "enum": ["particles"]},
                "bounds": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 4,
                    "maxItems": 4,
                },
                "count": {"type": "integer", "minimum": 1},
                "length": {"type": "number"},
                "angle_deg": {"type": "number"},
                "angle_jitter": {"type": "number"},
                "shape": {"type": "string", "enum": ["petal", "line", "sparkle", "drift", "bokeh"]},
            },
            "required": [
                "kind",
                "id",
                "layer",
                "bounds",
                "count",
                "brush",
                "length",
                "angle_deg",
                "angle_jitter",
                "shape",
            ],
            "additionalProperties": False,
        },
        {
            "type": "object",
            "properties": {
                **common_properties,
                "kind": {"type": "string", "enum": ["macro"]},
                "name": {
                    "type": "string",
                    "enum": [
                        "flower_cluster",
                        "sakura_canopy",
                        "branch_tree",
                        "mountain_range",
                        "watercolor_wash",
                        "rose_bloom",
                        "wildflower",
                    ],
                },
                "center": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 2,
                    "maxItems": 2,
                },
                "radius": {"type": "number"},
                "bounds": {
                    "type": "array",
                    "items": {"type": "number"},
                    "minItems": 4,
                    "maxItems": 4,
                },
                "colors": {
                    "type": "array",
                    "items": {"type": "string"},
                },
                "params": {
                    "type": "object",
                    "properties": {
                        "petal_type": {"type": "string"},
                        "has_stem": {"type": "boolean"},
                        "foliage": {"type": "boolean"},
                        "layers": {"type": "integer"},
                        "style": {"type": "string"},
                    },
                    "required": [],
                    "additionalProperties": False,
                },
            },
            "required": [
                "kind",
                "id",
                "layer",
                "name",
                "brush",
                "center",
                "radius",
                "bounds",
                "colors",
                "params",
            ],
            "additionalProperties": False,
        },
    ]
    return {
        "name": "stroke_program",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {
                "schema_version": {"type": "integer", "enum": [2]},
                "prompt": {"type": "string"},
                "seed": {"type": "integer"},
                "title": {"type": "string"},
                "iteration": {"type": "integer"},
                "goal_reached": {"type": "boolean"},
                "completion_score": {"type": "number"},
                "canvas": {
                    "type": "object",
                    "properties": {
                        "width": {"type": "number"},
                        "height": {"type": "number"},
                    },
                    "required": ["width", "height"],
                    "additionalProperties": False,
                },
                "operations": {
                    "type": "array",
                    "items": {"oneOf": operation_schemas},
                    "minItems": 1,
                },
                "request_canvas_image": {"type": "boolean"},
            },
            "required": [
                "schema_version",
                "prompt",
                "seed",
                "title",
                "iteration",
                "goal_reached",
                "completion_score",
                "canvas",
                "operations",
                "request_canvas_image",
            ],
            "additionalProperties": False,
        },
    }


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
    max_retries: int = 2
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
        self._reference_cache_key: bytes | None = None
        self._reference_cache_bytes: bytes | None = None

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
            if isinstance(plan_or_content, str):
                response_kind = "text"
                has_content = bool(plan_or_content.strip())
            else:
                response_kind = "JSON object"
                has_content = True
            if not has_content:
                raise LLMPlannerError("モデル応答が空でした")
            msg = f"接続成功: モデルが正常に応答しました ({elapsed:.2f}s, 応答形式: {response_kind})"
            self._log(msg)
            return msg
        except Exception as exc:
            msg = f"接続失敗: HTTP 200 でしたがモデル応答を解釈できませんでした ({elapsed:.2f}s): {exc}"
            self._log(msg)
            raise LLMPlannerError(msg) from exc

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
        brush_profile = canonical_brush_profile(str(kwargs.get("brush_profile", "auto") or "auto"))

        if image_data:
            cache_key = hashlib.blake2b(image_data, digest_size=16).digest()
            if cache_key == self._reference_cache_key and self._reference_cache_bytes is not None:
                image_data = self._reference_cache_bytes
            else:
                try:
                    image_data = sanitize_reference_image(
                        image_data,
                        max_dimension=min(1024, self.settings.vision_resolution),
                    )
                except ValueError as exc:
                    raise LLMPlannerError(f"参照画像を外部送信用に安全化できませんでした: {exc}") from exc
                self._reference_cache_key = cache_key
                self._reference_cache_bytes = image_data
            if iteration > 1:
                self._log("参照画像は初回ステップで送信済みのため再送せず、最新キャンバスだけを評価します")
                image_data = None
        if canvas_image:
            try:
                canvas_image = sanitize_reference_image(
                    canvas_image,
                    max_dimension=min(1024, self.settings.vision_resolution),
                )
            except ValueError as exc:
                raise LLMPlannerError(f"現在のキャンバス画像を外部送信用に安全化できませんでした: {exc}") from exc

        def raise_if_cancelled() -> None:
            if callable(cancelled) and cancelled():
                raise LLMPlannerError("LLM 描画計画の生成をキャンセルしました")

        if iteration == 1:
            self._conversation_history = []

        is_reasoning = _is_reasoning_model(self.settings.model)
        count_display = (
            f"{valid_count}" if valid_count is not None else f"Auto (品質予算: 最大{AUTO_LLM_STROKE_BUDGET}本)"
        )
        prompt_digest = hashlib.blake2b(valid_prompt.encode("utf-8"), digest_size=6).hexdigest()
        self._log(
            f"--- 描画計画生成開始 (Step {iteration}/{max_iterations}) ---\n"
            f"Prompt: {len(valid_prompt)} chars, digest={prompt_digest}, Seed: {valid_seed}, Count: {count_display}, GoalMode: {goal_mode}, Canvas: {valid_width}x{valid_height}\n"
            f"Endpoint: {_endpoint_origin_label(self.settings.endpoint_url)}, Model: {self.settings.model} (思考モデル最適化: {is_reasoning}, ReasoningEffort: {self.settings.reasoning_effort}), Timeout: {self.settings.timeout_seconds}s"
        )

        # 段階的ステップ描画時のフェーズ目標の導出
        phase_goal = ""
        if max_iterations > 1 or goal_mode:
            if max_iterations == 2:
                phase_goal = (
                    "Phase 1/2: Base Color Blocking, Environment & Initial Shadows (Flats/Shading layer)"
                    if iteration == 1
                    else "Phase 2/2 [FINAL]: Structural Lineart, Highlights & Subject-appropriate Polish (Lineart/Highlights/FX)"
                )
            elif max_iterations == 3:
                if iteration == 1:
                    phase_goal = "Phase 1/3: Base Color Masses & Silhouettes (Flats layer)"
                elif iteration == 2:
                    phase_goal = "Phase 2/3: 3D Form Sculpting, Ambient Occlusion & Shadows (Shading layer)"
                else:
                    phase_goal = "Phase 3/3 [FINAL]: Crisp Lineart, Highlights & Subject-appropriate Polish (Lineart/Highlights/FX)"
            else:
                if iteration == 1:
                    phase_goal = f"Phase {iteration}/{max_iterations}: Base Color Blocking & Foundations (Flats layer)"
                elif iteration == 2:
                    phase_goal = f"Phase {iteration}/{max_iterations}: 3D Volume Sculpting & Secondary Masses (Flats/Shading layer)"
                elif iteration < max_iterations:
                    phase_goal = f"Phase {iteration}/{max_iterations}: Shadow Crevices, Eraser Refinements & Structural Contours (Shading/Lineart layer)"
                else:
                    phase_goal = f"Phase {iteration}/{max_iterations} [FINAL]: Fine Lineart, Eraser Carving, Highlights & Subject-appropriate Polish (Lineart/Highlights/FX)"

        user_content_parts: list[dict[str, Any]] = []

        # 構造化テキスト指示
        req_stroke_count: Any = (
            valid_count
            if valid_count is not None
            else f"auto (quality budget up to {AUTO_LLM_STROKE_BUDGET} compiled strokes)"
        )
        req_dict: dict[str, Any] = {
            "prompt": valid_prompt,
            "seed": valid_seed,
            "stroke_count": req_stroke_count,
            "canvas": {"width": valid_width, "height": valid_height},
            "palette": palette_name,
            "brush_profile": brush_profile,
            "iteration": iteration,
            "max_iterations": max_iterations,
            "goal_mode": goal_mode,
            "phase_goal": phase_goal or "Complete full professional illustration.",
            "instruction": (
                (
                    'Visually inspect the attached current canvas, identify what is missing or flawed, and fix/enrich it using operations or an eraser brush ({"brush":{"is_eraser":true}}). '
                    if canvas_image
                    else ""
                )
                + f"Generate a compact StrokeProgram for {phase_goal or 'the artwork'} strictly in schema_version 2 JSON format. "
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
                    "type": "text",
                    "text": "REFERENCE IMAGE: Use this only as the requested subject/style reference.",
                }
            )
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
                    "type": "text",
                    "text": "CURRENT CANVAS: Evaluate this rendered state and return only missing corrective operations.",
                }
            )
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
            brush_profile=brush_profile,
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
            "response_format": {"type": "json_schema", "json_schema": _get_stroke_program_json_schema()},
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
                err_summary = (
                    str(last_error)[:200].replace("\n", " ") if last_error else "Invalid format or incomplete JSON"
                )
                is_len_err = any(kw in err_summary.lower() for kw in ("token", "length", "途切れ", "上限"))
                self._log(
                    f"[自動リトライ 1/{max_attempts - 1}] エラーフィードバック付き (原因: {err_summary}) で再試行します..."
                )

                current_payload = dict(payload)
                current_payload.pop("reasoning_effort", None)
                if not is_reasoning:
                    current_payload["temperature"] = 0.2

                len_advice = (
                    " Your previous output was cut off due to token limit. Use compact operations (e.g. fill with 'wash' style and hatch) to keep total operations under 35."
                    if is_len_err
                    else ""
                )
                retry_sys = (
                    _system_instruction(
                        iteration=iteration,
                        max_iterations=max_iterations,
                        is_reasoning=True,
                        width=valid_width,
                        height=valid_height,
                        prompt=valid_prompt,
                        palette_name=palette_name,
                        brush_profile=brush_profile,
                    )
                    + f"\n\n[FEEDBACK FROM PREVIOUS ATTEMPT]\nPrevious attempt failed: {err_summary}.{len_advice}\n"
                    "CRITICAL: Output ONLY a single valid raw JSON object for the StrokeProgram schema, beginning with '{'. Do not use Markdown fences, preamble, or commentary."
                )
                r_messages: list[dict[str, Any]] = [{"role": "system", "content": retry_sys}]
                if self._conversation_history and iteration > 1:
                    r_messages.extend(self._conversation_history)
                r_messages.append({"role": "user", "content": user_content})
                current_payload["messages"] = r_messages
            else:
                err_summary = str(last_error)[:200].replace("\n", " ") if last_error else "Malformed output"
                self._log(
                    f"[自動リトライ {attempt - 1}/{max_attempts - 1}] 思考抑制・最小構造 Zero-Thought モードで再試行します..."
                )
                current_payload = dict(payload)
                current_payload.pop("response_format", None)
                current_payload.pop("reasoning_effort", None)
                if not is_reasoning:
                    current_payload["temperature"] = 0.0

                min_sys = (
                    f"Previous error: {err_summary}.\n"
                    'You must output ONLY raw valid JSON matching StrokeProgram schema. Start immediately with {"schema_version": 2, "operations": [...]. '
                    "Use normalized 0.0-1.0 coordinates. Absolutely no thoughts, explanations, or text outside the JSON."
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
                sanitized_plan = _apply_llm_style_constraints(
                    sanitized_plan,
                    brush_profile=brush_profile,
                    palette_name=palette_name,
                )
                total_pts = sum(len(s.points) for s in sanitized_plan.strokes)
                layers_str = ", ".join(sanitized_plan.layers)
                self._log(
                    f"描画計画生成成功: ストローク数={len(sanitized_plan.strokes)}, 総点数={total_pts}, レイヤー=[{layers_str}], "
                    f"互換フィールド(request_canvas_image)={sanitized_plan.request_canvas_image}"
                )

                # 端点だけの疑似 StrokeProgram は元の形状を誤伝達するため、履歴には集計値だけを保持する。
                # 次ステップの視覚的な正解は、常に最新キャンバス画像を権威ある入力として扱う。
                if max_iterations > 1:
                    layer_summaries: list[dict[str, Any]] = []
                    for layer_name in sanitized_plan.layers:
                        layer_strokes = [s for s in sanitized_plan.strokes if s.layer_name == layer_name]
                        if not layer_strokes:
                            continue
                        points = [point for stroke in layer_strokes for point in stroke.points]
                        colors = list(dict.fromkeys(stroke.color for stroke in layer_strokes))[:5]
                        layer_summaries.append(
                            {
                                "layer": layer_name,
                                "stroke_count": len(layer_strokes),
                                "point_count": len(points),
                                "eraser_count": sum(stroke.is_eraser for stroke in layer_strokes),
                                "bounds": [
                                    round(min(point.x for point in points) / valid_width, 3),
                                    round(min(point.y for point in points) / valid_height, 3),
                                    round(max(point.x for point in points) / valid_width, 3),
                                    round(max(point.y for point in points) / valid_height, 3),
                                ],
                                "representative_colors": colors,
                            }
                        )
                    history_summary = {
                        "type": "rendered_plan_summary",
                        "prompt": valid_prompt,
                        "seed": valid_seed,
                        "iteration": iteration,
                        "goal_reached": sanitized_plan.goal_reached,
                        "completion_score": sanitized_plan.completion_score,
                        "canvas": {"width": int(valid_width), "height": int(valid_height)},
                        "layers": layer_summaries,
                        "note": "Use the attached CURRENT CANVAS as authoritative visual state.",
                    }
                    # 画像を履歴へ複製すると、反復ごとに Base64 ペイロードが累積する。
                    # 次のキャンバス画像は常に最新リクエストへ個別添付し、履歴はテキスト要約だけ保持する。
                    self._conversation_history.append({"role": "user", "content": req_json})
                    self._conversation_history.append(
                        {"role": "assistant", "content": json.dumps(history_summary, ensure_ascii=False)}
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
                brush_profile=brush_profile,
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
        """400/422 のパラメータ非互換エラー（json_schema, temperature, max_tokens, response_format, reasoning_effort 等）および一時的障害を自動検知・適応して再試行する。"""
        current_payload = dict(payload)
        max_param_retries = 3

        for p_attempt in range(max_param_retries):
            if cancelled is not None and cancelled():
                raise LLMPlannerError("LLM API リクエストをキャンセルしました")
            try:
                return self._post(current_payload, cancelled=cancelled)
            except LLMPlannerError as exc:
                err_text = str(exc).lower()
                modified = False
                response_format_adapted = False

                # 0. 一時的サーバーエラー (429 Rate Limit, 500, 502, 503, 504, タイムアウト) への指数バックオフ再送
                is_transient = any(
                    code in err_text
                    for code in (
                        "http 429",
                        "http 500",
                        "http 502",
                        "http 503",
                        "http 504",
                        "rate limit",
                        "timed out",
                        "timeout",
                    )
                )
                if is_transient and p_attempt < max_param_retries - 1:
                    backoff_sec = min(4.0, 0.4 * (2**p_attempt))
                    self._log(
                        f"[一時通信エラー再試行] 一時的エラー ({exc}) を検知しました。{backoff_sec:.1f}s 後に再試行します..."
                    )
                    deadline = time.monotonic() + backoff_sec
                    while time.monotonic() < deadline:
                        if cancelled is not None and cancelled():
                            raise LLMPlannerError("LLM API リクエストをキャンセルしました") from exc
                        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
                    continue

                # 1. response_format: json_schema 非対応エラーの json_object / 除外への自動フォールバック
                if (
                    "response_format" in err_text or "json_schema" in err_text or "schema" in err_text
                ) and "response_format" in current_payload:
                    cur_rf = current_payload.get("response_format")
                    if isinstance(cur_rf, Mapping) and cur_rf.get("type") == "json_schema":
                        self._log(
                            "[パラメータ自動適応] モデルが json_schema をサポートしていないため json_object に切り替えます"
                        )
                        current_payload["response_format"] = {"type": "json_object"}
                        modified = True
                        response_format_adapted = True
                    else:
                        self._log("[パラメータ自動適応] モデルが response_format をサポートしていないため除外します")
                        current_payload.pop("response_format", None)
                        modified = True
                        response_format_adapted = True

                # 2. reasoning_effort 非対応エラーの自動パージ
                if (
                    "reasoning_effort" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "extra_forbidden"))
                    and "reasoning_effort" in current_payload
                ):
                    self._log("[パラメータ自動適応] モデルが reasoning_effort をサポートしていないため除外します")
                    current_payload.pop("reasoning_effort", None)
                    modified = True

                # 3. temperature 非対応エラーの自動パージ
                if (
                    "temperature" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "extra_forbidden"))
                    and "temperature" in current_payload
                ):
                    self._log("[パラメータ自動適応] モデルが temperature をサポートしていないため除外します")
                    current_payload.pop("temperature", None)
                    modified = True

                # 4. max_tokens -> max_completion_tokens への自動変換
                if (
                    "max_tokens" in err_text
                    and any(kw in err_text for kw in ("max_completion_tokens", "unsupported", "not support"))
                    and "max_tokens" in current_payload
                ):
                    self._log("[パラメータ自動適応] max_tokens を max_completion_tokens に変換します")
                    val = current_payload.pop("max_tokens")
                    current_payload["max_completion_tokens"] = val
                    modified = True

                # 5. max_completion_tokens -> max_tokens への逆変換 (旧型互換サーバー対応)
                if (
                    "max_completion_tokens" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "extra_forbidden"))
                    and "max_completion_tokens" in current_payload
                ):
                    self._log("[パラメータ自動適応] max_completion_tokens を max_tokens に変換します")
                    val = current_payload.pop("max_completion_tokens")
                    current_payload["max_tokens"] = val
                    modified = True

                # 6. response_format 一般非対応エラーの自動パージ
                if (
                    not response_format_adapted
                    and "response_format" in err_text
                    and any(kw in err_text for kw in ("unsupported", "not support", "invalid", "json_object"))
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
                        first_user_merged = False
                        for m in messages[1:]:
                            if m.get("role") == "user" and not first_user_merged:
                                first_user_merged = True
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

        raise LLMPlannerError("LLM API の互換パラメータ調整上限に達しました")

    def _post(
        self,
        payload: Mapping[str, Any],
        cancelled: Callable[[], bool] | None = None,
    ) -> Mapping[str, Any]:
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "AIStrokePainter/1.2.0",
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
                if getattr(response, "headers", None) is None:
                    # 単純な互換transport／テストdoubleは1回read契約の場合がある。
                    raw = response.read(self.MAX_RESPONSE_BYTES + 1)
                else:
                    chunks: list[bytes] = []
                    received_bytes = 0
                    while received_bytes <= self.MAX_RESPONSE_BYTES:
                        if cancelled is not None and cancelled():
                            raise LLMPlannerError("LLM API 応答の受信をキャンセルしました")
                        read_size = min(64 * 1024, self.MAX_RESPONSE_BYTES + 1 - received_bytes)
                        chunk = response.read(read_size)
                        if not chunk:
                            break
                        chunks.append(chunk)
                        received_bytes += len(chunk)
                    raw = b"".join(chunks)
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
            response_digest = hashlib.blake2b(raw, digest_size=6).hexdigest()
            self._log(f"JSON デコードエラー: {exc} (応答長={len(raw)} bytes, digest={response_digest})")
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
                    self._log(f"[思考プロセス (reasoning)] {len(reasoning)} 文字（本文はログへ記録しません）")

                # 本文 content のプレビュー
                content_val = msg.get("content")
                if isinstance(content_val, str) and content_val.strip():
                    c_lines = content_val.strip().splitlines()
                    self._log(
                        f"[LLM 応答本文プレビュー] {len(content_val)} 文字, {len(c_lines)} 行（本文はログへ記録しません）"
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
    brush_profile: str = "auto",
) -> str:
    """プロフェッショナルなデジタルイラスト作画戦略・レイヤー階層・空間アンカー・4層ライティングを含む高品質プロンプト。"""
    min_dim = min(width, height)
    aspect_ratio = width / max(1.0, height)
    if aspect_ratio >= 1.25:
        aspect_info = (
            f"=== TARGET CANVAS: {width:.0f}x{height:.0f} (Landscape Wide, Aspect {aspect_ratio:.2f}:1) ===\n"
            "Composition Directive: Use cinematic wide layout. Distribute scenery/motifs horizontally across x=0.0-1.0; "
            "place primary subject landmarks along Rule-of-Thirds vertical zones (x=0.33 or x=0.67).\n\n"
        )
    elif aspect_ratio <= 0.80:
        aspect_info = (
            f"=== TARGET CANVAS: {width:.0f}x{height:.0f} (Portrait Vertical, Aspect 1:{1.0 / aspect_ratio:.2f}) ===\n"
            "Composition Directive: Use dynamic vertical layout. Emphasize vertical depth/hierarchy (top zenith to bottom foreground) "
            "and vertical subject flow (character pose, towering scenery).\n\n"
        )
    else:
        aspect_info = (
            f"=== TARGET CANVAS: {width:.0f}x{height:.0f} (Balanced Standard, Aspect {aspect_ratio:.2f}:1) ===\n"
            "Composition Directive: Use harmonious balanced layout with strong central focal anchor and generous surrounding framing.\n\n"
        )

    flats_sz = f"{max(40, round(min_dim * 0.08))} to {max(150, round(min_dim * 0.20))} px"
    form_shad_sz = f"{max(25, round(min_dim * 0.03))} to {max(60, round(min_dim * 0.06))} px"
    detail_shad_sz = "8 to 22 px"
    main_line_sz = "3.5 to 6.0 px"
    detail_line_sz = "1.5 to 3.0 px (CRITICAL for eyes, lashes, double eyelids, nose tip, lips, hair tips)"
    hl_glint_sz = "2.0 to 3.5 px (eye specular glints) / 6 to 16 px (hair halo & rim light)"

    prompt_lower = prompt.lower()
    has_sakura = any(keyword in prompt_lower for keyword in ("sakura", "桜", "cherry blossom"))
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
            "=== SPATIAL COMPOSITION ANCHORS (Rule of Thirds & 3-Tier Depth) ===\n"
            "  - Zenith Sky: y=0.00 to 0.35 (Deep blue/sunset orange wash)\n"
            "  - Distant Horizon & Peaks: y=0.45 to 0.62 (Atmospheric blue-gray silhouettes)\n"
            "  - Main Hero Element (Sakura Tree / Lake): Trunk base at x=0.30 to 0.45, y=0.45 to 0.85; Canopy at y=0.20 to 0.55\n"
            "  - Foreground Ground / Petal Swarm: y=0.75 to 1.00 (Vibrant green/earth tones with scattered drifting petals)\n\n"
            "1. Layer 'Flats' (Complete Seamless Coverage & Base Volumes):\n"
            f"   - Sky Gradient: Paint multiple dense overlapping horizontal sweep strokes (size_px: {flats_sz}, brush: 'Airbrush Soft' or 'Basic-5 Size') "
            "from deep blue zenith (#2b5c8f) down to clear sky (#5c93cf), horizon haze (#b8d8f8), and soft white (#eef6ff). Leave NO white canvas gaps.\n"
            f"   - Distant & Midground Mountains: Paint sweeping mountain silhouettes with fill (style: 'directional', angle_deg: 25) or (size_px: {flats_sz}, brush: 'Basic-5 Size'). "
            "Use atmospheric perspective (distant peaks in soft blue-gray #6f829d, nearer peaks in deep pine/slate #283e50).\n"
            f"   - Rolling Hills & Ground: Dense green terrain wash (#4e7d58, #72a37c, #9ec4a5) with organic curving strokes. NEVER use orthogonal wireframe grid hatching on ground.\n"
            f"   - Sakura Blossom Canopy Clumps: Use fill (style: 'contour') for puffy, billowing clouds of pink foliage masses (size_px: {flats_sz}, brush: 'Wet Textured Soft' or 'Basic-5 Size') "
            "arranged above and around branches (colors: #ff9ebb, #ffb8cd, #ffd6e5). Overlap multiple puffy clusters to create huge 3D volume.\n"
            "2. Layer 'Shading' (3D Depth, Occlusion & Mountain Crags):\n"
            f"   - Mountain Ridges & Shadow Facets: Carve dramatic shadow slopes along mountain ridge lines (size_px: {form_shad_sz}, opacity: 0.6-0.8, brush: 'Dry Bristles', colors: #1a2733, #223445).\n"
            f"   - Cloud Undersides: Paint soft purplish shadow bulges under cloud masses with fill (style: 'contour', size_px: {form_shad_sz}, opacity: 0.5-0.7, colors: #92a4bc, #7b8ea7).\n"
            f"   - Blossom Canopy Deep Shadows: Paint deep magenta/purple-pink core shadows underneath blossom clusters with fill (style: 'contour', size_px: {detail_shad_sz}, colors: #a3436a, #842f53).\n"
            "3. Layer 'Lineart' (Organic Tree Anatomy & Crisp Ridge Contours):\n"
            f"   - Majestic Sakura Tree Trunk & Branches: Draw powerful, organic twisting tree trunks with S-curves and wide root flares (size_px: {main_line_sz}, brush: 'Ink-3 Gpen', colors: #342017, #24140d). "
            f"Branch hierarchically! Main thick trunk -> major bending limbs -> tapering fine secondary branches (size_px: {detail_line_sz}, pressure: 0.2->0.8->0.1) threading through the pink blossom canopy.\n"
            f"   - Mountain Crests & Sharp Contours: Outline sharp jagged crags and crisp cloud rim curves (size_px: {main_line_sz}, brush: 'Ink-3 Gpen').\n"
            "4. Layer 'Highlights' & 'FX' (Light Accents & Falling Petal Blizzard):\n"
            f"   - Falling Petal Blizzard: Use particle operations with 'shape': 'petal' or scatter individual curved petal strokes drifting on wind (size_px: {detail_line_sz}, colors: #ffffff, #ffe6f0, #ffd0e2) across foreground and midground.\n"
            f"   - Luminous Rim Lighting & Cloud Edges: Pure glowing white/pale-gold rim highlights on sunny mountain peaks and top cloud rims (size_px: {hl_glint_sz}, colors: #ffffff, #fffde6).\n"
        )
        if not has_sakura:
            domain_guidance = (
                domain_guidance.replace(
                    "Landscape, Mountains, Clouds & Sakura Trees", "Landscape and Natural Environment"
                )
                .replace("Sakura Tree / Lake", "Tree / Lake")
                .replace(" / Petal Swarm", "")
                .replace(" with scattered drifting petals", "")
                .replace("   - Sakura Blossom Canopy Clumps:", "   - Foliage Canopy Clumps:")
                .replace("pink foliage", "foliage")
                .replace("pink blossom canopy", "tree canopy")
                .replace("Blossom Canopy Deep Shadows", "Foliage Canopy Deep Shadows")
                .replace("Majestic Sakura Tree", "Majestic Foreground Tree")
                .replace(
                    "4. Layer 'Highlights' & 'FX' (Light Accents & Falling Petal Blizzard):\n",
                    "4. Layer 'Highlights' & 'FX' (Subject-appropriate Light and Atmosphere):\n",
                )
                .replace(
                    f"   - Falling Petal Blizzard: Use particle operations with 'shape': 'petal' or scatter individual curved petal strokes drifting on wind (size_px: {detail_line_sz}, colors: #ffffff, #ffe6f0, #ffd0e2) across foreground and midground.\n",
                    "   - Use sparse subject-appropriate atmospheric accents; do not add petals unless requested.\n",
                )
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
            "\n[DOMAIN ART DIRECTION: Anime / Manga Character Portrait (Exquisite 3D Anatomy & Facial Planes)]\n"
            "=== STRICT SPATIAL FACIAL ANCHORS (Proportions & Golden Coordinates) ===\n"
            "  - Face Center X: 0.50 (Symmetric and well-centered)\n"
            "  - Head Crown & Hair Top: y = 0.12 to 0.18\n"
            "  - Eyebrows: y = 0.38 to 0.42 (Left Brow x=0.34-0.45, Right Brow x=0.55-0.66)\n"
            "  - Eyes & Lashes (CRITICAL): y = 0.44 to 0.49 (Left Eye Center x=0.38, Right Eye Center x=0.62, Width=0.12)\n"
            "  - Nose Tip: y = 0.56 to 0.59, x = 0.50 (Tiny subtle dot or hook, NEVER a heavy vertical black bar)\n"
            "  - Lips / Mouth: y = 0.64 to 0.67, x = 0.46 to 0.54 (Delicate curved upper lip line with corner nodes)\n"
            "  - Chin V-Curve: y = 0.74 to 0.78, x = 0.50\n"
            "  - Ears: y = 0.44 to 0.58 (Aligned between eye line and nose tip)\n"
            "  - Neck & Clavicles: Neck y=0.74 to 0.85; Collarbones y=0.85 to 0.89; Shoulders x=0.20 to 0.80\n\n"
            "1. Layer 'Flats' (Flawless Base Volumes & 3-Layer Hair):\n"
            f"   - Skin Base: Smooth complete coverage of face, ears, neck, and shoulders with fill (style: 'wash', size_px: {flats_sz}, color: #fff0e6 / #fef2ea).\n"
            f"   - Inner/Back Hair Mass: Deep darker hair silhouette behind neck and shoulders (size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            f"   - Main Hair Silhouette: Volumetric hair masses framing head with distinct clump volumes (size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            "   - Sclera (Whites of eyes): Clean bright base (#f8f9fa, size_px: 12-20px) under eye sockets.\n"
            "   - Iris Base: Expressive oval discs for irises with fill (style: 'radial', size_px: 15-28px, colors: #e84a75 / #3b82f6 / #8b5cf6).\n"
            "2. Layer 'Shading' (3D Facial Planes, 4-Tier Lighting & Ambient Occlusion):\n"
            "   - Forehead & Temples Shading: Soft form shadow on forehead edges and temples (Airbrush Soft, size_px: 30-60px, opacity: 0.35).\n"
            "   - Bangs Cast Shadow: Soft cast shadow directly beneath front hair strands onto the forehead (color: #e09f90, size_px: 8-16px, opacity: 0.50).\n"
            "   - Eye Socket Depths & Sclera Top Shadow: Airbrush shadow under brow bone and across top half of eyeballs to convey spherical depth (size_px: 8-15px, opacity: 0.55).\n"
            "   - Soft Facial Blush & Cheekbone Form: Use fill (style: 'contour', color: #ff9fb2, size_px: 35-70px, opacity: 0.25-0.35) across cheek apples and nose bridge.\n"
            "   - Nose Bridge & Subnasal Shadow: Subtle side contour for nose bridge + small soft cast shadow right beneath nose tip (#d88c7d, size_px: 4-8px, opacity: 0.60).\n"
            "   - Upper Lip Tone & Lower Lip Groove AO: Soft shaded upper lip + dark ambient occlusion in groove under bottom lip (size_px: 4-8px, opacity: 0.65).\n"
            "   - Submandibular AO & Neck Gradient: Darkest contact shadow right under jawline (AO, size_px: 8-16px, opacity: 0.80) grading softly down the neck.\n"
            "   - Clavicle (Collarbone) Hollows: Delicate hollow shadows above and below collarbone ridges (#e09f90, size_px: 6-12px, opacity: 0.50).\n"
            f"   - Hair Under-Lock Shadows: Use fill (style: 'directional', angle_deg: 45) for deep shadow crevices between hair strands (size_px: {detail_shad_sz}, opacity: 0.55-0.75).\n"
            "3. Layer 'Lineart' (EXQUISITE MICRO-DETAILS - Must use 1.5 to 3.0 px for facial features!):\n"
            "   - Upper Eyelashes (CRUCIAL): Bold sweeping arch with sharp tapered outer flick (preset: 'Ink-3 Gpen', size_px: 3.0 to 4.8 px, color: #1c1018, pressure: [[x,y,0.3], [x,y,0.98], [x,y,0.12]]).\n"
            "   - Double Eyelid Crease: Delicate thin curve just above upper lash (preset: 'Ink-3 Gpen', size_px: 1.5 to 2.2 px, color: #7a5850, pressure: 0.35).\n"
            "   - Iris Outer Ring & Pupil: Crisp circular iris contour and deep pupil core (size_px: 2.0 to 2.8 px, color: #221220).\n"
            "   - Lower Lash / Tear Trough: 1-2 delicate dots/short dashes under iris (size_px: 1.5 to 2.0 px, color: #221220, pressure: 0.2-0.4).\n"
            "   - Eyebrows: Graceful, slender arched curves above eyes with natural taper (size_px: 2.0 to 3.2 px, color: #36222c, pressure: 0.2 -> 0.8 -> 0.15).\n"
            "   - Nose Tip: CRITICAL: DO NOT draw a heavy black vertical bridge! Draw ONLY a tiny dot or subtle hook at the nose tip with soft shading (size_px: 1.5 to 2.0 px, color: #b87c70, pressure: 0.3-0.5).\n"
            "   - Lips / Mouth: Delicate upper lip line (size_px: 1.8 to 2.5 px, color: #a3384c, pressure: 0.2 -> 0.8 -> 0.2) with tiny corner modiolus nodes.\n"
            "   - Ear Anatomy Contours: Outer helix curve, inner antihelix, and tragus lines (size_px: 2.2 to 3.5 px, color: #321c22).\n"
            f"   - Jawline, Chin & Neck: Elegant, smooth V/U-curve chin, slender neck lines, and sharp clavicle contours (size_px: {main_line_sz}, color: #321c22).\n"
            "   - Flowing Hair Strands: Primary hair clump contours with fine secondary tapering sub-strands (size_px: 1.8 to 3.5 px, pressure: 0.15 -> 0.85 -> 0.08) and playful ahoge / flyaways.\n"
            "4. Layer 'Highlights' (5-Point Specular Accents & Angel Halo):\n"
            "   - Eye Specular Glints: 1 crisp bright glint on top-left of each iris + 1 secondary reflective glint on bottom-right (size_px: 2.0 to 3.5 px, color: #ffffff, pressure: 1.0, preset: 'Ink-3 Gpen').\n"
            "   - Iris Crescent Light: Luminous light arc on bottom half of iris (#8bc34a / #64b5f6 / #f48fb1, size_px: 2.0 to 3.0 px, opacity: 0.85).\n"
            "   - Nose Tip Specular Glint: Tiny bright glint right above nose tip (size_px: 1.8 to 2.5 px, color: #ffffff, opacity: 0.9).\n"
            "   - Lip Gloss Glint: Lustrous specular sheen dot/dash on lower lip curve (size_px: 2.0 to 2.8 px, color: #ffffff).\n"
            "   - Clavicle Ridge Highlight: Subtle luminous line along top of collarbone ridges (size_px: 2.0 to 3.0 px, color: #ffffff, opacity: 0.75).\n"
            "   - Hair Angel Halo (Ring of Light): Luminous curved streak across hair crown (#ffffff / #fff2b2, size_px: 6 to 14 px, opacity: 0.75-0.95).\n"
            "   - Rim Light (Edge Lighting): Fine luminous highlights tracing outer silhouette against background lighting (size_px: 2.5 to 4.5 px, color: #ffffff / #ffe8d6).\n"
        )
    elif any(
        k in prompt_lower
        for k in ["cat", "dog", "animal", "creature", "dragon", "bird", "wolf", "猫", "犬", "動物", "獣", "竜"]
    ):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Animal / Creature Art]\n"
            f"1. Layer 'Flats': Base body volume & fur base masses with fill (style: 'wash' / 'contour', size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            f"2. Layer 'Shading': Musculature shadows & fur tone gradations with fill (style: 'directional', size_px: {form_shad_sz}, opacity: 0.6, brush: 'Dry Bristles').\n"
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
            f"2. Layer 'Shading': Deep occlusion between buildings and foggy street glow with fill (style: 'wash', size_px: {form_shad_sz}, brush: 'Airbrush Soft').\n"
            f"3. Layer 'Lineart': Sharp structural edges, perspective grid, antenna spires (size_px: {main_line_sz}, brush: 'Ink-3 Gpen').\n"
            f"4. Layer 'Highlights' & 'FX': Vibrant neon signs, window grids, laser light beams with fill (style: 'radial') and particles (size_px: {hl_glint_sz}, colors: #00f0ff, #ff007f, #ffe600).\n"
        )
    elif any(k in prompt_lower for k in ["flower", "rose", "bouquet", "petal", "花", "バラ", "薔薇"]):
        domain_guidance = (
            "\n[DOMAIN ART DIRECTION: Blooming Flowers & Botanical]\n"
            f"1. Layer 'Flats': Petal base color blocks & leaf masses with fill (style: 'contour', size_px: {flats_sz}, brush: 'Basic-5 Size').\n"
            f"2. Layer 'Shading': Petal inner spiral crevice shadows with fill (style: 'contour', size_px: {detail_shad_sz}, opacity: 0.6, brush: 'Wet Textured Soft').\n"
            f"3. Layer 'Lineart': Organic petal edges, curving stem, leaf vein contours (size_px: {detail_line_sz}, brush: 'Ink-3 Gpen', pressure: 0.2 -> 0.8 -> 0.1).\n"
            f"4. Layer 'Highlights': Dewdrops, petal edge rim highlights (size_px: {hl_glint_sz}, brush: 'Basic-5 Size', color: #ffffff).\n"
        )

    has_character_subject = any(
        keyword in prompt_lower
        for keyword in ("girl", "boy", "woman", "man", "character", "anime", "人物", "少女", "少年")
    )
    has_natural_environment = any(
        keyword in prompt_lower
        for keyword in ("landscape", "mountain", "forest", "ocean", "garden", "山", "森", "海", "庭")
    )
    has_urban_environment = any(
        keyword in prompt_lower for keyword in ("city", "street", "building", "skyline", "都市", "街", "ビル")
    )
    if has_character_subject and (has_natural_environment or has_urban_environment):
        environment_name = "natural landscape" if has_natural_environment else "urban environment"
        domain_guidance += (
            f"\n[MIXED SCENE REQUIREMENT]\nKeep the character as the identifiable foreground subject while also drawing the requested {environment_name} in background and midground layers. "
            "Do not replace either component with the other; reserve clear silhouette separation and value contrast.\n"
        )

    progressive_section = ""
    if max_iterations > 1:
        if max_iterations == 2:
            if iteration == 1:
                phase_title = "Step 1/2: Foundation, Backdrop & Base Color Blocking (Flats/Shading layer)"
                phase_task = (
                    "Focus strictly on painting complete, gapless broad base colors (Flats): sky gradient, terrain/ground, mountain silhouettes, "
                    "skin/hair masses, or foliage clumps with large brush sizes and fill operations. Do not draw lineart or fine details yet."
                )
            else:
                phase_title = (
                    "Step 2/2 [FINAL]: 3D Shading, Structural Lineart, Highlights & Polish (Lineart/Highlights/FX)"
                )
                phase_task = (
                    "Complete the artwork by layering shadow depths (Shading), drawing crisp expressive contours (Lineart), "
                    "and adding sparse subject-appropriate highlights and atmosphere (Highlights/FX) over the existing base."
                )
        elif max_iterations == 3:
            if iteration == 1:
                phase_title = "Step 1/3: Foundation, Backdrop & Base Color Masses (Flats layer)"
                phase_task = "Paint ONLY the broad foundation and seamless base color silhouettes (Flats layer) with large brush sizes and fill."
            elif iteration == 2:
                phase_title = "Step 2/3: 3D Form Sculpting, Ambient Occlusion & Shadows (Shading layer)"
                phase_task = "Paint shadow crevices, cloud depth, muscle/cloth shading, and ambient occlusion over the base colors."
            else:
                phase_title = "Step 3/3 [FINAL]: Expressive Lineart, Highlights & Subject-appropriate Polish (Lineart/Highlights/FX)"
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
                phase_title = f"Step {iteration}/{max_iterations} [FINAL]: Fine Lineart, Highlights & Polish (Lineart/Highlights/FX)"
                phase_task = "Finish the painting with sharp line details, subject-appropriate highlights, and restrained lighting FX."

        progressive_section = (
            f"\n=== MULTI-STEP PROGRESSIVE DRAWING MODE ===\n"
            f"Current Execution: {phase_title}\n"
            f"Goal for this step: {phase_task}\n"
            "Draw ONLY the strokes appropriate for THIS step. Build coherently upon previous steps.\n"
        )

    visual_feedback_section = (
        "\n=== AUTOMATIC VISUAL FEEDBACK & CRITIQUE ===\n"
        "In multi-step mode, inspect the attached canvas capture image. Check for:\n"
        "  1. White canvas gaps or unpainted regions -> fill them in Flats/Shading.\n"
        "  2. Low contrast or flat lighting -> add deep Ambient Occlusion (AO) on Shading.\n"
        "  3. Missing facial landmarks or contour clarity -> reinforce with precision Lineart.\n"
        "Keep `request_canvas_image` false; the field remains only for schema compatibility.\n"
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
        "Path points support pressure as normalized `[x, y, pressure]` (e.g. `[[0.2, 0.3, 0.2], [0.4, 0.2, 0.95], [0.7, 0.4, 0.08]]`):\n"
        "  - Tapering & Flick Strokes (Eyelashes, Hair Tips, Branches): Use low entry (0.2), peak core (0.9-1.0), and razor-sharp exit (0.05-0.15).\n"
        "  - Feathering & Soft Blending (Blush, Skin Shadows, Glows): Use gentle low pressure (0.15 to 0.35) with 'Airbrush Soft'.\n"
        "  - Structural Occlusion: Use firm pressure (0.85 to 1.0) for depth accents.\n"
        "  - Eye Highlights: Use pressure 1.0 on tiny strokes (size_px: 2.0 to 3.0) for crisp sparkling dots.\n\n"
        "=== LAYER HIERARCHY & 4-TIER LIGHTING (Back to Front) ===\n"
        f"1. Layer 'Flats' (Backdrop, Gradients & Seamless Color Blocking):\n"
        f"   - Must use LARGE brush sizes (size_px: {flats_sz}) with dense overlapping strokes or fill (style: 'wash' / 'contour') to fully cover backgrounds. Leave no unpainted gaps!\n"
        f"2. Layer 'Shading' (3D Volume, Cast Shadows & Occlusion - Blended with Multiply):\n"
        f"   - Use fill (style: 'contour' / 'wash' / 'directional') with 'watercolor' / 'airbrush' for rich smooth volume, or medium/fine brush sizes ({form_shad_sz} for general volume, {detail_shad_sz} for crevices) with darker/cooler tones.\n"
        f"3. Layer 'Lineart' (Contours, Tree Anatomy & Fine Features):\n"
        f"   - Use dynamic crisp brush sizes ({main_line_sz} for outer silhouettes, {detail_line_sz} for fine eyes/lashes/nose/mouth/hair tips, preset: 'Ink-3 Gpen').\n"
        f"4. Layer 'Highlights' & 'FX' (Specular Glints and Subject-appropriate Atmosphere - Blended with Addition):\n"
        f"   - Use accent brush sizes ({hl_glint_sz}) with luminous colors for requested motifs, cloud rim light, sun flecks, iris crescent light, and sparse particle FX. Never add petals unless the prompt requests flowers or blossoms.\n"
        "5. Eraser Paths (`brush.is_eraser: true`):\n"
        '   - Add path operations with `"brush":{"profile":"eraser","is_eraser":true,...}` to sculpt contours, fix color bleeds, or carve highlights.\n'
        "   - Any size_px target in the art direction must be encoded as brush.size with brush.size_mode='px'; preset names map to brush.preset_hint. Prefer ratio sizes for resolution independence.\n\n"
        f"{aspect_info}"
        f"=== PALETTE DIRECTION: {palette_name.upper()} ===\n"
        f"Harmonize colors to match the '{palette_name}' aesthetic: prioritize cohesive color theory (warm lights, cool shadows, vibrant SSS accents), distinct value contrast, and radiant specular highlights.\n"
        f"=== BRUSH OVERRIDE: {brush_profile.upper()} ===\n"
        f"The UI-selected brush profile is '{brush_profile}'. Use it for normal operations; eraser operations remain erasers.\n"
        f"{domain_guidance}"
        f"{progressive_section}"
        f"{visual_feedback_section}\n"
        "=== OUTPUT SCHEMA: STROKE PROGRAM V2 ===\n"
        "Return one compact JSON object. Coordinates are normalized 0.0-1.0. Prefer macro/fill/hatch/particles over hundreds of repeated raw points:\n"
        "```json\n"
        "{\n"
        '  "schema_version": 2,\n'
        f'  "prompt": {json.dumps(prompt or "illustration")},\n'
        '  "seed": 42,\n'
        '  "title": "Artwork Title",\n'
        f'  "iteration": {iteration},\n'
        '  "goal_reached": false,\n'
        '  "completion_score": 0.85,\n'
        f'  "canvas": {{"width": {width:.0f}, "height": {height:.0f}}},\n'
        '  "operations": [\n'
        '    {"kind":"macro","id":"sky_wash","layer":"Flats","name":"watercolor_wash","bounds":[0.0,0.0,1.0,0.55],"colors":["#2b5c8f","#5c93cf","#eef6ff"],"brush":{"profile":"watercolor"}},\n'
        '    {"kind":"macro","id":"mountains","layer":"Flats","name":"mountain_range","center":[0.5,0.52],"colors":["#6f829d","#4a5568","#283e50"],"brush":{"profile":"watercolor"}},\n'
        '    {"kind":"macro","id":"tree_trunk","layer":"Lineart","name":"branch_tree","center":[0.38,0.78],"radius":0.35,"colors":["#342017"],"brush":{"profile":"gpen"}},\n'
        '    {"kind":"macro","id":"blossoms","layer":"Flats","name":"flower_cluster","center":[0.38,0.48],"radius":0.28,"colors":["#ffb8cd","#ffd6e5","#a3436a"],"params":{"petal_type":"sakura"},"brush":{"profile":"watercolor"}},\n'
        '    {"kind":"path","id":"accent_branch","layer":"Lineart","points":[[0.35,0.65,0.2],[0.42,0.58,0.85],[0.50,0.55,0.1]],"smooth":true,"brush":{"profile":"gpen","color":"#24140d","size":0.004}},\n'
        '    {"kind":"particles","id":"falling_petals","layer":"FX","shape":"petal","bounds":[0.1,0.2,0.9,0.9],"count":16,"length":0.015,"angle_deg":75,"angle_jitter":30,"brush":{"profile":"watercolor","color":"#ffe6f0","size":0.003}}\n'
        "  ]\n"
        "}\n"
        "```\n"
        "=== CRITICAL RULES & ANTI-PATTERNS ===\n"
        "1. Operation kinds:\n"
        "   - macro: (HIGHLY RECOMMENDED for natural motifs! 'flower_cluster', 'sakura_canopy', 'branch_tree', 'mountain_range', 'watercolor_wash')\n"
        "   - path: (2-12 organic spline control points with tapering pressure [x,y,pressure])\n"
        "   - fill: (3+ polygon points, styles: 'wash' (organic watercolor flow), 'contour', 'radial', 'directional')\n"
        "   - hatch: (polygon, angle_deg, spacing, only for manga screen-tones)\n"
        "   - particles: (bounds/count, shape: 'petal'/'sparkle'/'drift'/'bokeh'/'line')\n"
        "2. Brush profiles: auto, gpen, marupen, brush, marker, pencil, watercolor, airbrush, eraser.\n"
        "3. Anti-Patterns (STRICTLY FORBIDDEN):\n"
        "   - NEVER draw plain rectangular boxes, wireframe border frames, or blind-curtain horizontal stripes across the entire canvas.\n"
        "   - NEVER use straight lines or coarse parallel stripes to depict trees, flowers, or mountains. Use macro primitives or curved paths instead.\n"
        "   - NEVER use hatch for smooth 3D shading, foliage, clouds, or landscape (causes artificial wireframe/zebra stripes). Use watercolor/airbrush wash or contour fill.\n"
        "   - NEVER draw isolated mathematical parabolic arcs across trees as fake highlights.\n"
        "   - NEVER draw solid white (#ffffff) normal brush strokes on Shading layer (which multiplies). For highlights, use Highlights/FX layers; for erasing, set is_eraser: true.\n"
        "4. Composition: Establish rich background washes and volume with macro/fill, then form shadows on Shading, then precise contours on Lineart and sparse particle accents on FX.\n"
        "5. First character of output must be '{'. Do not use Markdown fences."
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
    # 閉じられていない未閉鎖タグ（JSON開始 '{' 手前まで、または末尾までを除去）
    re.compile(r"<(?:think|thought|reasoning|thought_process|reflection)>[\s\S]*?(?=\{|\Z)", re.IGNORECASE),
    re.compile(r"\[(?:thought|reasoning|thought_process)\][\s\S]*?(?=\{|\Z)", re.IGNORECASE),
    re.compile(r"\|begin_of_thought\|[\s\S]*?(?=\{|\Z)", re.IGNORECASE),
    re.compile(r"【(?:思考|推論)】[\s\S]*?(?=\{|\Z)", re.IGNORECASE),
]


def _clean_thinking_tokens(text: str) -> str:
    """思考プロセスタグ（<think>, <thought>, <reasoning>, [THOUGHT] 等）および特殊制御文字を安全に除去する。"""
    cleaned = text
    # 制御文字・BOM・ゼロ幅文字の除去
    cleaned = re.sub(r"[\ufeff\u200b\u200c\u200d\u2060]", "", cleaned)
    cleaned = cleaned.replace("\u00a0", " ")
    for pattern in _THINKING_TAG_PATTERNS:
        cleaned = pattern.sub("", cleaned)
    return cleaned.strip()


# タグなしプレーンテキスト思考の冒頭パターン（CoT: "The user wants...", "Let me plan...", "Thinking process:" 等）
_PLAIN_THINKING_PATTERNS = [
    re.compile(
        r"^(?:The user wants|I need to|Let me plan|Let's create|Thinking Process|Plan:|Step 1:|To draw|In this drawing)[\s\S]*?(?=(?:```|\{\s*\"(?:schema_version|prompt|seed|title|iteration|layers|strokes|operations|strokes_summary|completed_layers)\"))",
        re.IGNORECASE,
    ),
    re.compile(
        r"^[\s\S]*?(?=(?:```json\s*\{|```\s*\{|\{\s*\"(?:schema_version|prompt|strokes|operations|strokes_summary)\"))",
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

    # 優先順位 2: DrawingPlan / StrokeProgram 主要キーを含む `{`
    plan_key_m = re.search(
        r"\{\s*\"(?:schema_version|prompt|seed|title|iteration|layers|strokes|operations|strokes_summary|completed_layers)\"",
        text,
        flags=re.IGNORECASE,
    )
    if plan_key_m:
        return plan_key_m.start()

    # 優先順位 3: ストローク/Operation 要素のキーを含む `{`
    stroke_key_m = re.search(
        r"\{\s*\"(?:kind|id|points|polygon|brush|brush_preset|layer_name|layer|start_xy)\"", text, flags=re.IGNORECASE
    )
    if stroke_key_m:
        return stroke_key_m.start()

    # 優先順位 4: 単純な最初の `{`
    return text.find("{")


_FULLWIDTH_MAP = str.maketrans(
    {
        "：": ":",
        "，": ",",
        "｛": "{",
        "｝": "}",
        "［": "[",
        "］": "]",
        "（": "(",
        "）": ")",
        "“": '"',
        "”": '"',
        "‘": "'",
        "’": "'",
        "｀": "`",
        "；": ";",
    }
)


def _escape_raw_newlines_in_json_strings(text: str) -> str:
    """JSON 文字列リテラル内部に直接改行が入っている場合、エスケープ \\n に安全に変換する。"""
    out: list[str] = []
    in_string = False
    escape = False

    for ch in text:
        if escape:
            out.append(ch)
            escape = False
            continue
        if ch == "\\":
            out.append(ch)
            escape = True
            continue
        if ch == '"':
            in_string = not in_string
            out.append(ch)
            continue
        if in_string and ch == "\n":
            out.append("\\n")
            continue
        if in_string and ch == "\r":
            continue
        if in_string and ch == "\t":
            out.append("\\t")
            continue
        out.append(ch)

    return "".join(out)


def _sanitize_json_text(text: str) -> str:
    """LLM 特有の構文乱れ（全角記号、スマートクォート、コメント、末尾カンマ、シングルクォート、Python/JS定数、非クォートキー、単位付き数値）を包括的にサニタイズする。"""
    if not text:
        return ""

    # 1. 全角記号・スマートクォート・不可視文字の半角 ASCII 正規化
    s = text.translate(_FULLWIDTH_MAP)
    s = re.sub(r"[\ufeff\u200b\u200c\u200d\u2060\u00a0]", " ", s)
    s = s.strip()

    # 2. ブロックコメント /* ... */ の除去
    s = re.sub(r"/\*[\s\S]*?\*/", "", s)

    # 3. 行コメント // ... および # ... の除去 (URL 中の "http://" や "https://", カラーコード "#ffffff" は保護)
    lines: list[str] = []
    for line in s.splitlines():
        # // コメントの除去 (http:// や https:// の後ではないもの)
        line_clean = re.sub(r'(?<![:"\'/])//.*$', "", line)
        # # コメントの除去 (引用符内や16進カラー #abcdef の直後ではないもの)
        line_clean = re.sub(r'(?<!["\'\w#])#(?![0-9a-fA-F]{3,8}\b).*$', "", line_clean)
        lines.append(line_clean)
    s = "\n".join(lines)

    # 4. 文字列リテラル内の生改行のエスケープ
    s = _escape_raw_newlines_in_json_strings(s)

    # 5. Python / JS 定数を JSON 標準定数に正規化
    s = re.sub(r"\bTrue\b", "true", s)
    s = re.sub(r"\bFalse\b", "false", s)
    s = re.sub(r"\bNone\b", "null", s)
    s = re.sub(r"\bundefined\b", "null", s)
    s = re.sub(r"\bNaN\b", "null", s)
    s = re.sub(r"\bInfinity\b", "null", s)
    s = re.sub(r"\b-Infinity\b", "null", s)

    # 6. 非クォートキーのダブルクォート化: { key: 123, sub_key-1: "val" } -> { "key": 123, "sub_key-1": "val" }
    s = re.sub(r"([{,]\s*)([a-zA-Z_][a-zA-Z0-9_-]*)\s*:", r'\1"\2":', s)

    # 7. 数値末尾の単位サフィックス除去 (e.g. "size": 200px -> "size": 200, "angle": 30deg -> 30, "opacity": 50% -> 0.5)
    s = re.sub(r"(:\s*-?\d+(?:\.\d+)?)\s*%\b", lambda m: f": {float(m.group(1).split(':')[1].strip()) / 100.0}", s)
    s = re.sub(r"(:\s*-?\d+(?:\.\d+)?)\s*(?:px|pt|deg|rad)\b", r"\1", s)

    # 8. 安全なシングルクォートキー・値の変換（単語内アポストロフィ don't / let's 等の破壊防止）
    s = re.sub(r"([{,]\s*)'([^'\\]*(?:\\.[^'\\]*)*)'\s*:", r'\1"\2":', s)
    s = re.sub(r":\s*'([^'\\]*(?:\\.[^'\\]*)*)'(\s*[,}\]])", r': "\1"\2', s)
    s = re.sub(r"(\[\s*)'([^'\\]*(?:\\.[^'\\]*)*)'", r'\1"\2"', s)
    s = re.sub(r",\s*'([^'\\]*(?:\\.[^'\\]*)*)'", r', "\1"', s)

    # 9. オブジェクト・配列末尾のカンマ（Trailing commas）の除去
    s = re.sub(r",\s*([}\]])", r"\1", s)

    return s.strip()


def _try_parse_any_json(text: str) -> Any | None:
    """json.loads と ast.literal_eval を駆使して文字列から安全に辞書またはリストを復元する。"""
    if not text or not text.strip():
        return None
    s = text.strip()

    # 1. 生テキストの直接 json.loads
    try:
        val: Any = json.loads(s)
        if isinstance(val, (dict, list)):
            return val
    except Exception:
        pass

    # 2. ast.literal_eval (Python リテラル、アポストロフィ混在、True/False/None をネイティブ解釈)
    try:
        parsed = ast.literal_eval(s)
        if isinstance(parsed, (dict, list)):
            normalized: Any = json.loads(json.dumps(parsed))
            if isinstance(normalized, (dict, list)):
                return normalized
    except Exception:
        pass

    # 3. サニタイズ適用後に再試行
    sanitized = _sanitize_json_text(s)
    if sanitized:
        try:
            val_san: Any = json.loads(sanitized)
            if isinstance(val_san, (dict, list)):
                return val_san
        except Exception:
            pass
        try:
            parsed_san = ast.literal_eval(sanitized)
            if isinstance(parsed_san, (dict, list)):
                norm_san: Any = json.loads(json.dumps(parsed_san))
                if isinstance(norm_san, (dict, list)):
                    return norm_san
        except Exception:
            pass

    return None


def _unwrap_drawing_container(obj: Any) -> Mapping[str, Any] | None:
    """ネストされたラッパー辞書や配列から DrawingPlan / StrokeProgram 辞書をアンラップして取り出す。"""
    if isinstance(obj, Mapping):
        if _looks_like_drawing_json(obj):
            return obj
        # ラッパーキーの探索
        for wrapper_key in (
            "plan",
            "stroke_program",
            "drawing_plan",
            "data",
            "result",
            "response",
            "output",
            "payload",
            "content",
        ):
            cand = obj.get(wrapper_key)
            if isinstance(cand, Mapping) and _looks_like_drawing_json(cand):
                return cand
            if isinstance(cand, Sequence) and not isinstance(cand, (str, bytes)) and len(cand) > 0:
                unwrapped = _unwrap_drawing_container(cand)
                if unwrapped is not None:
                    return unwrapped
        # 計画関連キーを含む辞書であればそのまま返却
        if any(k in obj for k in ("operations", "strokes", "strokes_summary", "schema_version", "canvas", "layers")):
            return obj

    elif isinstance(obj, Sequence) and not isinstance(obj, (str, bytes)):
        items = [item for item in obj if isinstance(item, Mapping)]
        if items:
            if any(
                "kind" in it or "polygon" in it or "bounds" in it or ("start_xy" in it and "end_xy" in it)
                for it in items
            ):
                return {"schema_version": 2, "operations": items}
            if any("points" in it for it in items):
                return {"schema_version": 1, "strokes": items}
            return {"schema_version": 2, "operations": items}

    return None


def _extract_balanced_json_blocks(text: str) -> list[str]:
    """テキスト内から括弧の対応 ( { ... } または [ ... ] ) が取れた完全な JSON ブロック群を抽出する。"""
    if not text:
        return []
    blocks: list[str] = []
    stack: list[str] = []
    start_pos: int = -1
    in_string: bool = False
    escape: bool = False

    for idx, ch in enumerate(text):
        if escape:
            escape = False
            continue
        if ch == "\\":
            escape = True
            continue
        if ch == '"':
            in_string = not in_string
            continue
        if in_string:
            continue

        if ch in ("{", "["):
            if not stack:
                start_pos = idx
            stack.append(ch)
        elif ch in ("}", "]"):
            if stack and ((ch == "}" and stack[-1] == "{") or (ch == "]" and stack[-1] == "[")):
                stack.pop()
                if not stack and start_pos >= 0:
                    candidate = text[start_pos : idx + 1].strip()
                    if candidate and candidate not in blocks:
                        blocks.append(candidate)
                    start_pos = -1

    def _block_score(b: str) -> int:
        score = 0
        if "schema_version" in b:
            score += 10
        if "operations" in b:
            score += 8
        if "strokes_summary" in b:
            score += 7
        if "strokes" in b:
            score += 6
        if "canvas" in b:
            score += 4
        return -score

    return sorted(blocks, key=_block_score)


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


def _sanitize_and_rescue_program_dict(
    value: Mapping[str, Any],
    canvas_w: float = 1000.0,
    canvas_h: float = 1000.0,
) -> dict[str, Any]:
    """LLM 出力の表記揺れを正規化する。欠落したジオメトリ自体は創作しない。"""
    d = dict(value)

    # 1. canvas の安全な取得
    canvas = d.get("canvas")
    if isinstance(canvas, Mapping):
        w = float(canvas.get("width", canvas_w))
        h = float(canvas.get("height", canvas_h))
    else:
        w = float(d.get("canvas_width", canvas_w))
        h = float(d.get("canvas_height", canvas_h))
    if w <= 1.0:
        w = canvas_w
    if h <= 1.0:
        h = canvas_h

    # 2. operations の探索と正規化（レイヤーネスト構造の自動フラット化を含む）
    raw_ops: list[Any] = []
    direct_ops = d.get("operations")
    if isinstance(direct_ops, Sequence) and not isinstance(direct_ops, (str, bytes)):
        raw_ops.extend(direct_ops)
    else:
        for alt_key in (
            "strokes_summary",
            "summary_strokes",
            "strokes",
            "paths",
            "lines",
            "commands",
            "items",
            "shapes",
            "draw_list",
            "elements",
            "actions",
            "steps",
        ):
            cand = d.get(alt_key)
            if isinstance(cand, Sequence) and not isinstance(cand, (str, bytes)) and len(cand) > 0:
                raw_ops.extend(cand)
                break

    # レイヤーネスト構造 (例: {"layers": [{"name": "Flats", "operations": [...]}, ...]}) の展開
    layers_raw = d.get("layers") or d.get("layer_list")
    if isinstance(layers_raw, Sequence) and not isinstance(layers_raw, (str, bytes)):
        for l_item in layers_raw:
            if isinstance(l_item, Mapping):
                l_name = str(l_item.get("name") or l_item.get("layer") or l_item.get("layer_name") or "").strip()
                l_ops = (
                    l_item.get("operations")
                    or l_item.get("strokes")
                    or l_item.get("paths")
                    or l_item.get("elements")
                    or l_item.get("draw_list")
                )
                if isinstance(l_ops, Sequence) and not isinstance(l_ops, (str, bytes)):
                    for sub_op in l_ops:
                        if isinstance(sub_op, Mapping):
                            sub_d = dict(sub_op)
                            if l_name and not sub_d.get("layer") and not sub_d.get("layer_name"):
                                sub_d["layer"] = l_name
                            raw_ops.append(sub_d)

    clean_ops: list[dict[str, Any]] = []
    seen_ids: set[str] = set()

    for idx, item in enumerate(raw_ops):
        if not isinstance(item, Mapping):
            continue
        item_d = dict(item)
        raw_kind = str(item_d.get("kind", "")).strip().lower()

        # 点座標の正規化ヘルパー
        def _norm_pts(raw_point_list: Any) -> list[list[float]]:
            res: list[list[float]] = []
            if not isinstance(raw_point_list, Sequence) or isinstance(raw_point_list, (str, bytes)):
                return res
            for pt in raw_point_list:
                try:
                    if isinstance(pt, Sequence) and not isinstance(pt, (str, bytes)) and len(pt) >= 2:
                        px = float(pt[0])
                        py = float(pt[1])
                        if px > 1.0 and w > 1.0:
                            px = px / w
                        if py > 1.0 and h > 1.0:
                            py = py / h
                        pp = float(pt[2]) if len(pt) >= 3 else 0.8
                        res.append([max(0.0, min(1.0, px)), max(0.0, min(1.0, py)), max(0.0, min(1.0, pp))])
                    elif isinstance(pt, Mapping) and "x" in pt and "y" in pt:
                        px = float(pt["x"])
                        py = float(pt["y"])
                        if px > 1.0 and w > 1.0:
                            px = px / w
                        if py > 1.0 and h > 1.0:
                            py = py / h
                        pp = float(pt.get("pressure", 0.8))
                        res.append([max(0.0, min(1.0, px)), max(0.0, min(1.0, py)), max(0.0, min(1.0, pp))])
                except (ValueError, TypeError):
                    continue
            return res

        # 簡易図形の自動変換 (rect, box, circle, ellipse 等)
        if raw_kind in ("rect", "rectangle", "box"):
            kind = "fill"
            # rect: [x, y, w, h] または bounds: [x0, y0, x1, y1]
            raw_rect = item_d.get("rect") or item_d.get("bounds") or item_d.get("box")
            if isinstance(raw_rect, Sequence) and len(raw_rect) >= 4:
                try:
                    rx0, ry0, rx1, ry1 = float(raw_rect[0]), float(raw_rect[1]), float(raw_rect[2]), float(raw_rect[3])
                    if (
                        raw_kind in ("rect", "rectangle")
                        and "bounds" not in item_d
                        and rx1 <= 1.0
                        and ry1 <= 1.0
                        and rx0 + rx1 <= 1.01
                    ):
                        # [x, y, width, height] 形式
                        rx1, ry1 = rx0 + rx1, ry0 + ry1
                    if rx0 > 1.0 and w > 1.0:
                        rx0, rx1 = rx0 / w, rx1 / w
                    if ry0 > 1.0 and h > 1.0:
                        ry0, ry1 = ry0 / h, ry1 / h
                    item_d["polygon"] = [[rx0, ry0], [rx1, ry0], [rx1, ry1], [rx0, ry1]]
                except (ValueError, TypeError):
                    pass
        elif raw_kind in ("circle", "ellipse", "disc"):
            kind = "fill"
            raw_center = item_d.get("center") or (item_d.get("cx", 0.5), item_d.get("cy", 0.5))
            try:
                cx = float(raw_center[0]) if isinstance(raw_center, Sequence) and len(raw_center) >= 2 else 0.5
                cy = float(raw_center[1]) if isinstance(raw_center, Sequence) and len(raw_center) >= 2 else 0.5
                if cx > 1.0 and w > 1.0:
                    cx = cx / w
                if cy > 1.0 and h > 1.0:
                    cy = cy / h
                r_val = float(item_d.get("radius") or item_d.get("r") or 0.05)
                if r_val > 1.0 and min(w, h) > 1.0:
                    r_val = r_val / min(w, h)
                # 16頂点の近似円ポリゴン
                circle_poly: list[list[float]] = []
                for deg_idx in range(16):
                    ang = (deg_idx / 16.0) * 2.0 * math.pi
                    circle_poly.append(
                        [max(0.0, min(1.0, cx + r_val * math.cos(ang))), max(0.0, min(1.0, cy + r_val * math.sin(ang)))]
                    )
                item_d["polygon"] = circle_poly
            except (ValueError, TypeError):
                pass
        elif raw_kind in ("fill", "wash", "area", "region", "polygon", "background", "base"):
            kind = "fill"
        elif raw_kind in ("hatch", "crosshatch", "shading", "shading_hatch"):
            kind = "hatch"
        elif raw_kind in ("particles", "particle", "dots", "sparks", "swarms", "bokeh", "fx", "scatter"):
            kind = "particles"
        elif raw_kind in (
            "macro",
            "flower_cluster",
            "sakura_canopy",
            "branch_tree",
            "mountain_range",
            "watercolor_wash",
            "rose_bloom",
            "wildflower",
            "tree",
            "flower",
            "mountain",
        ):
            kind = "macro"
        else:
            kind = "path"

        op_id = str(item_d.get("id") or "").strip()
        if not op_id or op_id in seen_ids:
            raw_fingerprint = json.dumps(item_d, ensure_ascii=True, sort_keys=True, default=str)
            deterministic_suffix = uuid.uuid5(
                uuid.NAMESPACE_URL,
                f"ai-stroke/llm-rescue/{idx}/{raw_fingerprint}",
            ).hex[:8]
            op_id = f"{op_id or 'op'}_{idx + 1}_{deterministic_suffix}"
        seen_ids.add(op_id)

        layer_val = str(item_d.get("layer") or item_d.get("layer_name") or "").strip()
        if not layer_val:
            layer_val = (
                "Flats"
                if kind in ("fill", "macro")
                else "Shading"
                if kind == "hatch"
                else "FX"
                if kind == "particles"
                else "Lineart"
            )

        # brush の正規化（文字列型指定も自動吸収）
        raw_brush = item_d.get("brush")
        if isinstance(raw_brush, Mapping):
            b_dict = dict(raw_brush)
        elif isinstance(raw_brush, str) and raw_brush.strip():
            b_dict = {"profile": infer_brush_profile(raw_brush), "preset_hint": raw_brush.strip()}
        else:
            b_dict = {}

        # brush 直下または operation 直下のフィールドを統合
        p_name = str(
            b_dict.get("profile")
            or item_d.get("profile")
            or b_dict.get("preset_hint")
            or item_d.get("brush_preset")
            or ("marker" if kind == "fill" else "pencil" if kind == "hatch" else "auto")
        ).strip()
        canonical_p = canonical_brush_profile(p_name)
        if canonical_p == "auto" and p_name.lower() != "auto":
            inferred_p = infer_brush_profile(p_name)
            if inferred_p != "auto":
                canonical_p = inferred_p
        preset_h = b_dict.get("preset_hint") or item_d.get("preset_hint") or item_d.get("brush_preset")
        if not preset_h and canonical_p == "auto" and p_name.lower() != "auto":
            preset_h = p_name

        c_val = normalize_hex_color(
            b_dict.get("color") or item_d.get("color") or "#232323",
            fallback="#232323",
        )

        sz_val = (
            b_dict.get("size")
            or b_dict.get("size_ratio")
            or item_d.get("size")
            or item_d.get("size_ratio")
            or b_dict.get("thickness")
            or item_d.get("thickness")
            or b_dict.get("width")
            or item_d.get("width")
        )
        sz_mode_raw = str(b_dict.get("size_mode") or item_d.get("size_mode") or "").strip().lower()
        try:
            f_sz = float(sz_val) if sz_val is not None else None
        except (ValueError, TypeError):
            f_sz = None

        if sz_val is None or f_sz is None:
            if "size_px" in b_dict or "size_px" in item_d:
                try:
                    px_val = float(b_dict.get("size_px") or item_d.get("size_px") or 8.0)
                except (ValueError, TypeError):
                    px_val = 8.0
                sz_val = max(0.5, px_val)
                sz_mode = "px"
            else:
                sz_val = 0.035 if kind == "fill" else 0.0025 if kind == "hatch" else 0.006
                sz_mode = "ratio"
        else:
            if sz_mode_raw == "px" or (sz_mode_raw != "ratio" and f_sz > 1.0):
                sz_val = max(0.5, f_sz)
                sz_mode = "px"
            else:
                sz_val = max(0.0001, min(1.0, f_sz))
                sz_mode = "ratio"

        try:
            op_opacity = float(b_dict.get("opacity") or item_d.get("opacity") or 1.0)
        except (ValueError, TypeError):
            op_opacity = 1.0
        op_opacity = max(0.0, min(1.0, op_opacity))

        raw_eraser = b_dict.get("is_eraser", item_d.get("is_eraser"))
        if raw_eraser is None:
            is_eraser = (
                canonical_p == "eraser" or "eraser" in str(preset_h or "").lower() or str(layer_val).lower() == "eraser"
            )
        else:
            is_eraser = raw_eraser in (True, "true", "True", 1, "1")

        clean_brush: dict[str, Any] = {
            "profile": canonical_p,
            "color": c_val,
            "size": max(0.0001, sz_val),
            "size_mode": sz_mode,
            "opacity": op_opacity,
            "is_eraser": is_eraser,
        }
        if preset_h:
            clean_brush["preset_hint"] = str(preset_h)

        if kind == "path":
            raw_pts = (
                item_d.get("points")
                or item_d.get("coords")
                or item_d.get("control_points")
                or item_d.get("vertices")
                or item_d.get("nodes")
            )
            if raw_pts is None:
                if "start_xy" in item_d and "end_xy" in item_d:
                    raw_pts = [item_d["start_xy"], item_d["end_xy"]]
                elif "start" in item_d and "end" in item_d:
                    raw_pts = [item_d["start"], item_d["end"]]
                elif "from" in item_d and "to" in item_d:
                    raw_pts = [item_d["from"], item_d["to"]]
                elif "x1" in item_d and "y1" in item_d and "x2" in item_d and "y2" in item_d:
                    raw_pts = [[item_d["x1"], item_d["y1"]], [item_d["x2"], item_d["y2"]]]
                else:
                    continue
            pts = _norm_pts(raw_pts)
            if len(pts) < 2:
                continue
            raw_closed = item_d.get("closed", False)
            raw_smooth = item_d.get("smooth", True)
            clean_ops.append(
                {
                    "kind": "path",
                    "id": op_id,
                    "layer": layer_val,
                    "points": pts,
                    "brush": clean_brush,
                    "closed": raw_closed in (True, "true", "True", 1),
                    "smooth": raw_smooth not in (False, "false", "False", 0),
                }
            )
        elif kind == "fill":
            raw_poly = item_d.get("polygon") or item_d.get("points") or item_d.get("coords") or item_d.get("vertices")
            if raw_poly is None:
                if "start_xy" in item_d and "end_xy" in item_d:
                    sx, sy = item_d["start_xy"]
                    ex, ey = item_d["end_xy"]
                    raw_poly = [[sx, sy], [ex, sy], [ex, ey], [sx, ey]]
                else:
                    continue
            poly = _norm_pts(raw_poly)
            if len(poly) < 3:
                continue
            style_str = str(item_d.get("style", "wash")).strip().lower()
            if style_str not in {"wash", "scanline", "feathered", "contour", "radial", "directional"}:
                style_str = "wash"
            angle_value = item_d.get("angle_deg", item_d.get("angle", 0.0))
            try:
                angle_deg = float(angle_value) if angle_value is not None else 0.0
            except (ValueError, TypeError):
                angle_deg = 0.0
            try:
                raw_sp = item_d.get("spacing", 0.72)
                spacing_val = max(0.2, min(1.0, float(raw_sp) if raw_sp is not None else 0.72))
            except (ValueError, TypeError):
                spacing_val = 0.72
            clean_ops.append(
                {
                    "kind": "fill",
                    "id": op_id,
                    "layer": layer_val,
                    "polygon": poly,
                    "brush": clean_brush,
                    "spacing": spacing_val,
                    "style": style_str,
                    "angle_deg": angle_deg % 360.0,
                }
            )
        elif kind == "hatch":
            raw_poly = item_d.get("polygon") or item_d.get("points") or item_d.get("coords") or item_d.get("vertices")
            if raw_poly is None:
                if "start_xy" in item_d and "end_xy" in item_d:
                    sx, sy = item_d["start_xy"]
                    ex, ey = item_d["end_xy"]
                    raw_poly = [[sx, sy], [ex, sy], [ex, ey], [sx, ey]]
                else:
                    continue
            poly = _norm_pts(raw_poly)
            if len(poly) < 3:
                continue
            raw_cross = item_d.get("cross", False)
            try:
                raw_ha = item_d.get("angle_deg", 30.0)
                h_angle = float(raw_ha) if raw_ha is not None else 30.0
            except (ValueError, TypeError):
                h_angle = 30.0
            try:
                raw_hs = item_d.get("spacing", 0.012)
                h_spacing = max(0.001, min(0.5, float(raw_hs) if raw_hs is not None else 0.012))
            except (ValueError, TypeError):
                h_spacing = 0.012
            clean_ops.append(
                {
                    "kind": "hatch",
                    "id": op_id,
                    "layer": layer_val,
                    "polygon": poly,
                    "brush": clean_brush,
                    "angle_deg": h_angle % 180.0,
                    "spacing": h_spacing,
                    "cross": raw_cross in (True, "true", "True", 1),
                }
            )
        elif kind == "particles":
            raw_b = item_d.get("bounds", (0.0, 0.0, 1.0, 1.0))
            if isinstance(raw_b, Sequence) and not isinstance(raw_b, (str, bytes)) and len(raw_b) == 4:
                try:
                    b_floats = [max(0.0, min(1.0, float(v))) for v in raw_b if v is not None]
                    if len(b_floats) != 4:
                        b_floats = [0.0, 0.0, 1.0, 1.0]
                except (ValueError, TypeError):
                    b_floats = [0.0, 0.0, 1.0, 1.0]
                if b_floats[2] <= b_floats[0] or b_floats[3] <= b_floats[1]:
                    b_floats = [0.0, 0.0, 1.0, 1.0]
            else:
                b_floats = [0.0, 0.0, 1.0, 1.0]
            shape_str = str(item_d.get("shape", "petal")).strip().lower()
            if shape_str not in {"petal", "line", "sparkle", "drift", "bokeh"}:
                shape_str = "petal"
            try:
                raw_cnt = item_d.get("count", 20)
                p_count = max(1, min(500, int(raw_cnt) if raw_cnt is not None else 20))
            except (ValueError, TypeError):
                p_count = 20
            try:
                raw_len = item_d.get("length", 0.015)
                p_length = max(0.0005, min(0.5, float(raw_len) if raw_len is not None else 0.015))
            except (ValueError, TypeError):
                p_length = 0.015
            try:
                raw_pa = item_d.get("angle_deg", 90.0)
                p_angle = float(raw_pa) if raw_pa is not None else 90.0
            except (ValueError, TypeError):
                p_angle = 90.0
            try:
                raw_jit = item_d.get("angle_jitter", 35.0)
                p_jitter = max(0.0, min(180.0, float(raw_jit) if raw_jit is not None else 35.0))
            except (ValueError, TypeError):
                p_jitter = 35.0
            clean_ops.append(
                {
                    "kind": "particles",
                    "id": op_id,
                    "layer": layer_val,
                    "bounds": b_floats,
                    "count": p_count,
                    "brush": clean_brush,
                    "length": p_length,
                    "angle_deg": p_angle,
                    "angle_jitter": p_jitter,
                    "shape": shape_str,
                }
            )
        elif kind == "macro":
            macro_n = str(item_d.get("name") or (raw_kind if raw_kind != "macro" else "flower_cluster")).strip().lower()
            raw_c = item_d.get("center") or (0.5, 0.5)
            try:
                cx = float(raw_c[0]) if isinstance(raw_c, Sequence) and len(raw_c) >= 2 else 0.5
                cy = float(raw_c[1]) if isinstance(raw_c, Sequence) and len(raw_c) >= 2 else 0.5
                if cx > 1.0 and w > 1.0:
                    cx = cx / w
                if cy > 1.0 and h > 1.0:
                    cy = cy / h
                c_tuple = [max(0.0, min(1.0, cx)), max(0.0, min(1.0, cy))]
            except (ValueError, TypeError):
                c_tuple = [0.5, 0.5]
            try:
                raw_r = float(item_d.get("radius") or item_d.get("r") or 0.2)
                if raw_r > 1.0 and min(w, h) > 1.0:
                    raw_r = raw_r / min(w, h)
                r_val = max(0.001, min(2.0, raw_r))
            except (ValueError, TypeError):
                r_val = 0.2
            raw_b = item_d.get("bounds", (0.0, 0.0, 1.0, 1.0))
            if isinstance(raw_b, Sequence) and len(raw_b) >= 4:
                try:
                    b_x0, b_y0, b_x1, b_y1 = float(raw_b[0]), float(raw_b[1]), float(raw_b[2]), float(raw_b[3])
                    if b_x0 > 1.0 and w > 1.0:
                        b_x0 = b_x0 / w
                    if b_y0 > 1.0 and h > 1.0:
                        b_y0 = b_y0 / h
                    if b_x1 > 1.0 and w > 1.0:
                        b_x1 = b_x1 / w
                    if b_y1 > 1.0 and h > 1.0:
                        b_y1 = b_y1 / h
                    b_floats = [
                        max(0.0, min(1.0, b_x0)),
                        max(0.0, min(1.0, b_y0)),
                        max(0.0, min(1.0, b_x1)),
                        max(0.0, min(1.0, b_y1)),
                    ]
                except (ValueError, TypeError):
                    b_floats = [0.0, 0.0, 1.0, 1.0]
            else:
                b_floats = [0.0, 0.0, 1.0, 1.0]
            raw_cols = item_d.get("colors", ())
            cols_list = (
                [normalize_hex_color(c) for c in raw_cols if isinstance(c, (str, int))]
                if isinstance(raw_cols, Sequence)
                else []
            )
            clean_ops.append(
                {
                    "kind": "macro",
                    "id": op_id,
                    "name": macro_n,
                    "layer": layer_val,
                    "brush": clean_brush,
                    "center": c_tuple,
                    "radius": r_val,
                    "bounds": b_floats,
                    "colors": cols_list,
                    "params": dict(item_d.get("params", {})) if isinstance(item_d.get("params"), Mapping) else {},
                }
            )

    if not clean_ops:
        raise PlanValidationError("有効なジオメトリを持つ operation がありません")

    return {
        "schema_version": 2,
        "prompt": str(d.get("prompt", "")),
        "seed": int(
            d.get("seed", 0) if isinstance(d.get("seed"), (int, float)) and not isinstance(d.get("seed"), bool) else 0
        ),
        "title": str(d.get("title", "")),
        "iteration": int(
            d.get("iteration", 1)
            if isinstance(d.get("iteration"), int) and not isinstance(d.get("iteration"), bool)
            else 1
        ),
        "canvas": {"width": w, "height": h},
        "operations": clean_ops,
        "metadata": dict(d.get("metadata", {})) if isinstance(d.get("metadata"), Mapping) else {},
        "goal_reached": d.get("goal_reached", False) is True,
        "completion_score": max(
            0.0,
            min(
                1.0,
                float(
                    d.get("completion_score", 0.0)
                    if isinstance(d.get("completion_score"), (int, float))
                    and not isinstance(d.get("completion_score"), bool)
                    else 0.0
                ),
            ),
        ),
    }


def _mapping_to_drawing_plan(
    value: Mapping[str, Any],
    *,
    prompt: str,
    seed: int,
    width: float,
    height: float,
) -> DrawingPlan:
    """v2 StrokeProgram を優先し、strokes_summary や既存 v1 DrawingPlan も互換入力として受理する。"""
    unwrapped = _unwrap_drawing_container(value)
    if unwrapped is not None:
        value = unwrapped

    # 1. 既存 v1 strokes 形式で純粋な DrawingPlan の場合
    if (
        "strokes" in value
        and isinstance(value["strokes"], list)
        and "operations" not in value
        and "strokes_summary" not in value
    ):
        try:
            sanitized = _sanitize_repaired_dict(value)
            val_to_use = sanitized if sanitized is not None else dict(value)
            return DrawingPlan.from_dict(val_to_use)
        except Exception:
            pass

    # 2. v2 StrokeProgram 形式（直接パース試行）
    try:
        if (
            value.get("schema_version") == 2
            and "operations" in value
            and not any(k in value for k in ("strokes_summary", "summary_strokes"))
        ):
            program = StrokeProgram.from_dict(value)
            program = replace(
                program,
                prompt=prompt or program.prompt,
                seed=seed,
                canvas_width=width,
                canvas_height=height,
            )
            return compile_stroke_program(program)
    except Exception:
        pass

    # 3. サニタイズ・救済レイヤーを通して StrokeProgram を構築
    rescued_dict = _sanitize_and_rescue_program_dict(value, canvas_w=width, canvas_h=height)
    program = StrokeProgram.from_dict(rescued_dict)
    program = replace(
        program,
        prompt=prompt or program.prompt,
        seed=seed,
        canvas_width=width,
        canvas_height=height,
    )
    return compile_stroke_program(program)


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

    # 2. 直接 StrokeProgram / DrawingPlan 辞書の場合
    if _looks_like_drawing_json(response):
        try:
            return _mapping_to_drawing_plan(
                response,
                prompt=prompt,
                seed=seed,
                width=width,
                height=height,
            )
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
            if _looks_like_drawing_json(value):
                plan = _mapping_to_drawing_plan(
                    value,
                    prompt=prompt,
                    seed=seed,
                    width=width,
                    height=height,
                )
                if plan.strokes:
                    if log_func is not None and source_name != "message.content":
                        log_func(f"通知: ソース [{source_name}] から描画 JSON オブジェクトを救出しました")
                    return plan
        except Exception as exc:
            last_error = exc
            continue

    # 4. 断片ストローク・ハーベスターによる救出（全候補テキストからストローク/operationsを探索）
    for source_name, text in candidates:
        if not text.strip():
            continue
        harvested = _harvest_stroke_fragments(text, log_func=log_func)
        if harvested is not None and _looks_like_drawing_json(harvested):
            if log_func is not None:
                log_func(f"通知: ソース [{source_name}] から描画断片を直接救出・合成しました")
            try:
                return _mapping_to_drawing_plan(harvested, prompt=prompt, seed=seed, width=width, height=height)
            except Exception as exc:
                last_error = exc

    # どの候補からも完全な DrawingPlan が得られなかった場合
    first_text = candidates[0][1]
    if log_func is not None:
        response_digest = hashlib.blake2b(first_text.encode("utf-8"), digest_size=6).hexdigest()
        log_func(f"LLM 応答パース再試行: {len(first_text)} 文字, digest={response_digest}")

    try:
        value = _extract_json_object(first_text, log_func=log_func)
        return _mapping_to_drawing_plan(value, prompt=prompt, seed=seed, width=width, height=height)
    except Exception as exc:
        raise LLMPlannerError(f"LLM が有効な DrawingPlan JSON を返しませんでした: {exc}") from (last_error or exc)


def _looks_like_drawing_json(value: Any) -> TypeGuard[Mapping[str, Any]]:
    if not isinstance(value, Mapping):
        return False
    # 配列としてストロークや operations を含む計画
    if any(
        isinstance(value.get(k), list)
        for k in (
            "operations",
            "strokes",
            "strokes_summary",
            "summary_strokes",
            "commands",
            "actions",
            "elements",
            "shapes",
            "paths",
            "lines",
            "draw_list",
        )
    ):
        return True
    # スキーマバージョンと計画情報を持つオブジェクト
    return bool(
        "schema_version" in value
        and any(k in value for k in ("prompt", "title", "canvas", "iteration", "completed_layers", "stroke_count"))
    )


def _extract_json_object(content: str, log_func: Callable[[str], None] | None = None) -> Mapping[str, Any]:
    """思考除去・多段ダイレクトパース・AST解析・括弧バランサー・途切れ修復・断片救出を駆使して JSON を抽出する。"""
    cleaned_tags = _clean_thinking_tokens(content.strip())
    stripped_thinking = _strip_plain_text_thinking(cleaned_tags)

    text_sources = [
        cleaned_tags,
        stripped_thinking,
        content,
    ]

    # 1. 完全なコードブロック ```json ... ``` の抽出と直接パース (AST含む)
    for text_source in text_sources:
        if not text_source.strip():
            continue
        fence_matches = list(re.finditer(r"```(?:json)?\s*([\s\S]*?)\s*```", text_source, flags=re.IGNORECASE))
        for match in reversed(fence_matches):
            candidate = match.group(1).strip()
            parsed = _try_parse_any_json(candidate)
            if parsed is not None:
                unwrapped = _unwrap_drawing_container(parsed)
                if unwrapped is not None and _looks_like_drawing_json(unwrapped):
                    return unwrapped
            repaired = _attempt_json_repair(candidate)
            if repaired is not None and _looks_like_drawing_json(repaired):
                if log_func is not None:
                    log_func("通知: コードブロック内の途切れた JSON を自動修復しました")
                return repaired

    # 2. バランス括弧ブロック抽出 (最外層 { ... } を正確にスキャン)
    for text_source in text_sources:
        if not text_source.strip():
            continue
        blocks = _extract_balanced_json_blocks(text_source)
        for block in blocks:
            parsed = _try_parse_any_json(block)
            if parsed is not None:
                unwrapped = _unwrap_drawing_container(parsed)
                if unwrapped is not None and _looks_like_drawing_json(unwrapped):
                    return unwrapped

    # 3. テキスト全体のダイレクトパース試行
    for target in text_sources:
        if not target.strip():
            continue
        parsed = _try_parse_any_json(target)
        if parsed is not None:
            unwrapped = _unwrap_drawing_container(parsed)
            if unwrapped is not None and _looks_like_drawing_json(unwrapped):
                return unwrapped

        # 最適開始アンカーからの raw_decode 試行
        start_idx = _find_best_json_start(target)
        if start_idx >= 0:
            try:
                val, _ = json.JSONDecoder().raw_decode(target[start_idx:])
                unwrapped = _unwrap_drawing_container(val)
                if unwrapped is not None and _looks_like_drawing_json(unwrapped):
                    return unwrapped
            except Exception:
                pass

    # 4. 閉じられていない未完コードブロック (Unclosed Fences) の救出
    for text_source in text_sources:
        if not text_source.strip():
            continue
        unclosed_m = re.search(r"```(?:json)?\s*(\{[\s\S]*)$", text_source, flags=re.IGNORECASE)
        if unclosed_m:
            candidate = unclosed_m.group(1).strip()
            repaired = _attempt_json_repair(candidate)
            if repaired is not None and _looks_like_drawing_json(repaired):
                if log_func is not None:
                    log_func("通知: 閉じられていないコードブロックから途切れ JSON を自動修復しました")
                return repaired

    # 5. 途中で途切れた JSON の高度な末尾修復 & スタック解析
    for target in text_sources:
        if not target.strip():
            continue
        repaired = _attempt_json_repair(target)
        if repaired is not None and _looks_like_drawing_json(repaired):
            if log_func is not None:
                log_func("警告: トークン上限等で途切れた JSON を自動修復して読み込みました")
            return repaired

    # 6. 断片ストローク・ハーベスター（Stroke Fragment Harvester）による救出
    for target in text_sources:
        if not target.strip():
            continue
        harvested = _harvest_stroke_fragments(target, log_func=log_func)
        if harvested is not None and _looks_like_drawing_json(harvested):
            if log_func is not None:
                log_func("通知: テキストからストローク/Operation断片を直接救出しました")
            return harvested

    raise json.JSONDecodeError("DrawingPlan JSON オブジェクトを抽出できませんでした", content, 0)


def _harvest_stroke_fragments(text: str, log_func: Callable[[str], None] | None = None) -> dict[str, Any] | None:
    """崩壊した JSON や長文テキストから個々の operation / stroke / summary 断片を正規表現・個別パースで救出して計画辞書を合成する。"""
    if not text:
        return None

    # prompt の抽出試行
    prompt_m = re.search(r'"prompt"\s*:\s*"([^"\\]*(?:\\.[^"\\]*)*)"', text)
    found_prompt = prompt_m.group(1) if prompt_m else "ai illustration"

    # seed の抽出試行
    seed_m = re.search(r'"seed"\s*:\s*(\d+)', text)
    found_seed = int(seed_m.group(1)) if seed_m else 42

    operations: list[dict[str, Any]] = []
    strokes: list[dict[str, Any]] = []
    decoder = json.JSONDecoder()

    # 1. operation / summary / stroke オブジェクトの探索
    op_pattern = re.compile(
        r'\{\s*(?:"id"|"kind"|"polygon"|"bounds"|"points"|"start_xy"|"start"|"brush"|"brush_preset"|"color"|"size_px"|"layer_name"|"layer"|"opacity")',
        re.IGNORECASE,
    )
    for m in op_pattern.finditer(text):
        idx = m.start()
        try:
            obj, _ = decoder.raw_decode(text[idx:])
            if isinstance(obj, Mapping):
                if (
                    "kind" in obj
                    or "polygon" in obj
                    or "bounds" in obj
                    or ("start_xy" in obj and "end_xy" in obj)
                    or ("brush" in obj and "points" not in obj)
                ):
                    op_id = obj.get("id") or f"op_{len(operations) + 1}"
                    if not any(o.get("id") == op_id for o in operations):
                        op_dict = dict(obj)
                        op_dict["id"] = op_id
                        operations.append(op_dict)
                elif "points" in obj and isinstance(obj["points"], list) and len(obj["points"]) >= 2:
                    st_id = obj.get("id") or f"stroke_{len(strokes) + 1}"
                    if not any(s.get("id") == st_id for s in strokes):
                        st_dict = dict(obj)
                        st_dict["id"] = st_id
                        strokes.append(st_dict)
                elif "color" in obj and ("start_xy" in obj or "size_px" in obj):
                    op_id = obj.get("id") or f"op_{len(operations) + 1}"
                    if not any(o.get("id") == op_id for o in operations):
                        op_dict = dict(obj)
                        op_dict["id"] = op_id
                        operations.append(op_dict)
        except (json.JSONDecodeError, ValueError):
            continue

    if operations:
        return {
            "schema_version": 2,
            "prompt": found_prompt,
            "seed": found_seed,
            "title": f"Rescued AI Program - {found_prompt[:20]}",
            "iteration": 1,
            "operations": operations,
        }

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
    """修復された JSON 辞書内の operations / strokes / strokes_summary 配列を検査し、不完全な点を補完または未完ストロークを除去する。"""
    d = dict(val)

    # 1. operations 配列のサニタイズ
    if "operations" in d and isinstance(d["operations"], list):
        clean_ops: list[dict[str, Any]] = []
        for op in d["operations"]:
            if not isinstance(op, Mapping):
                continue
            op_dict = dict(op)
            clean_ops.append(op_dict)
        if clean_ops:
            d["operations"] = clean_ops
            return d

    # 2. strokes_summary 配列のサニタイズ
    if "strokes_summary" in d and isinstance(d["strokes_summary"], list):
        clean_summary = [s for s in d["strokes_summary"] if isinstance(s, Mapping)]
        if clean_summary:
            d["strokes_summary"] = clean_summary
            return d

    # 3. strokes 配列のサニタイズ
    strokes_raw = d.get("strokes")
    if isinstance(strokes_raw, list):
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
            if len(clean_pts) >= 2:
                st_clean = dict(st)
                st_clean["points"] = clean_pts
                if not st_clean.get("id"):
                    st_clean["id"] = f"stroke_{idx}"
                clean_strokes.append(st_clean)

        if clean_strokes:
            d["strokes"] = clean_strokes
            return dict(d)

    if _looks_like_drawing_json(d):
        return dict(d)
    return None


def _attempt_json_repair(text: str) -> Mapping[str, Any] | None:
    """トークン上限等で末尾が切れた JSON の最適開始アンカー特定、未完ストローク/operation切落し、構文修復を行う。"""
    start = _find_best_json_start(text)
    if start < 0:
        return None
    s = _sanitize_json_text(text[start:].strip())

    repaired_candidates: list[Mapping[str, Any]] = []

    def _eval_candidate(c_dict: Mapping[str, Any]) -> None:
        if _looks_like_drawing_json(c_dict):
            sanitized = _sanitize_repaired_dict(c_dict)
            if sanitized is not None and sanitized not in repaired_candidates:
                repaired_candidates.append(sanitized)

    # 1. 過去数個の完全なオブジェクト `}` まで巻き戻して閉じる多段救済
    brace_indices = [i for i, ch in enumerate(s) if ch == "}"]
    for last_brace in reversed(brace_indices[-6:]):
        candidate_truncated = s[: last_brace + 1].strip()
        for suffix in ("]}", "]}]}", "}]}", "]}", "}", '"]}', '"]}]}', ""):
            try:
                val = json.loads(candidate_truncated + suffix)
                if isinstance(val, Mapping):
                    _eval_candidate(val)
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
        for base in (s_cleaned, s):
            try:
                val = json.loads(base + closing_suffix)
                if isinstance(val, Mapping):
                    _eval_candidate(val)
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
        for base in (s_cleaned, s):
            try:
                val = json.loads(base + suffix)
                if isinstance(val, Mapping):
                    _eval_candidate(val)
            except json.JSONDecodeError:
                pass

    if repaired_candidates:
        # 最も operations / strokes の要素数が多い候補を採用
        def _candidate_score(cand: Mapping[str, Any]) -> int:
            ops = cand.get("operations") or cand.get("strokes") or cand.get("strokes_summary") or []
            return len(ops) if isinstance(ops, Sequence) else 0

        return max(repaired_candidates, key=_candidate_score)

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
    is_flat_or_shading = (
        "flat" in layer_lower or "back" in layer_lower or "draft" in layer_lower or "shad" in layer_lower
    )

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
    safe_seed = seed

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
    effective_count = count if count is not None and count > 0 else AUTO_LLM_STROKE_BUDGET
    strokes_source = sample_strokes_by_priority(list(plan.strokes), effective_count)

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
    goal_reached = bool(getattr(plan, "goal_reached", False) is True or metadata_val.get("goal_reached", False) is True)
    completion_score = max(
        0.0,
        min(
            1.0,
            float(
                getattr(plan, "completion_score", 0.0)
                if getattr(plan, "completion_score", None) is not None
                else metadata_val.get("completion_score", 0.0)
            ),
        ),
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


def _apply_llm_style_constraints(
    plan: DrawingPlan,
    *,
    brush_profile: str,
    palette_name: str,
) -> DrawingPlan:
    """UI で選んだブラシとパレットを、モデルの自由記述より優先して確実に適用する。"""
    normalized_profile = canonical_brush_profile(brush_profile)
    recolored = recolor_strokes_to_palette(list(plan.strokes), palette_name)
    constrained: list[Stroke] = []
    for stroke in recolored:
        if normalized_profile == "auto" or stroke.is_eraser:
            constrained.append(stroke)
            continue
        point_count = len(stroke.points)
        constrained_points = tuple(
            StrokePoint(
                point.x,
                point.y,
                pressure_profile(
                    index / max(1, point_count - 1),
                    normalized_profile,
                    base=point.pressure,
                ),
                point.time_ms,
            )
            for index, point in enumerate(stroke.points)
        )
        constrained.append(
            Stroke(
                id=stroke.id,
                points=constrained_points,
                brush_preset=brush_preset_for_profile(normalized_profile),
                color=stroke.color,
                size_px=stroke.size_px,
                layer_name=stroke.layer_name,
                opacity=stroke.opacity,
                is_eraser=False,
            )
        )
    metadata = dict(plan.metadata)
    metadata["style_constraints"] = {
        "brush_profile": normalized_profile,
        "palette": palette_name,
        "palette_locked": True,
    }
    return DrawingPlan(
        prompt=plan.prompt,
        seed=plan.seed,
        strokes=tuple(constrained),
        title=plan.title,
        iteration=plan.iteration,
        layers=plan.layers,
        request_canvas_image=plan.request_canvas_image,
        metadata=metadata,
        canvas_width=plan.canvas_width,
        canvas_height=plan.canvas_height,
        goal_reached=plan.goal_reached,
        completion_score=plan.completion_score,
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
