"""Text-to-Image (画像生成 AI) API 連携クライアント。

OpenAI (DALL-E 3 / DALL-E 2 / 互換 API)、Stable Diffusion WebUI (Automatic1111 / Forge)、
ComfyUI、およびカスタム HTTP 画像生成エンドポイントをサポートし、プロンプトから高精細画像を生成する。
"""

from __future__ import annotations

import base64
import binascii
from collections.abc import Callable, Mapping
import contextlib
from dataclasses import dataclass
import ipaddress
import json
import re
import time
from typing import Any, cast
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .image_converter import sanitize_reference_image

DEFAULT_OPENAI_IMAGES_URL = "https://api.openai.com/v1/images/generations"
DEFAULT_SD_WEBUI_URL = "http://127.0.0.1:7860/sdapi/v1/txt2img"


class ImageGenerationError(RuntimeError):
    """画像生成 API の通信、認証、または画像デコードに関するエラー。"""


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
class ImageGeneratorSettings:
    """画像生成クライアントの設定。"""

    provider: str = "openai"  # "openai", "sd_webui", "custom_http"
    endpoint_url: str = DEFAULT_OPENAI_IMAGES_URL
    api_key: str = ""
    model: str = "dall-e-3"
    size: str = "1024x1024"  # "1024x1024", "1024x1792", "1792x1024", "512x512"
    quality: str = "standard"  # "standard", "hd"
    style: str = "vivid"  # "vivid", "natural"
    timeout_seconds: float = 120.0
    negative_prompt: str = ""


MAX_IMAGE_RESPONSE_BYTES = 50 * 1024 * 1024  # 50 MB


def sanitize_api_key_log(text: str) -> str:
    """API Key、Bearer トークン、および認証情報を伏字化する。"""
    masked = re.sub(r"Bearer\s+[A-Za-z0-9_\-\.]{8,}", "Bearer [REDACTED]", text, flags=re.IGNORECASE)
    masked = re.sub(r"sk-[A-Za-z0-9_\-\.]{10,}", "sk-[REDACTED]", masked)
    masked = re.sub(
        r'(["\']?(?:api[_-]?key|authorization|token)["\']?\s*[:=]\s*["\'])([^"\']{6,})(["\'])',
        r"\1[REDACTED]\3",
        masked,
        flags=re.IGNORECASE,
    )
    masked = re.sub(
        r"https?://([^:]+):([^@]+)@",
        r"https://\1:[REDACTED]@",
        masked,
        flags=re.IGNORECASE,
    )
    return masked


def _read_bounded_stream(
    response: Any,
    max_bytes: int,
    cancel_check: Callable[[], bool] | None = None,
) -> bytes:
    if getattr(response, "headers", None) is None:
        chunks: list[bytes] = []
        received = 0
        while received <= max_bytes:
            if cancel_check is not None and cancel_check():
                raise ImageGenerationError("画像データの受信をキャンセルしました")
            try:
                part = cast(bytes, response.read(64 * 1024))
            except TypeError:
                part = cast(bytes, response.read())
                if not part:
                    break
                if len(part) + received > max_bytes:
                    raise ImageGenerationError(
                        f"画像生成 API の応答が上限 ({max_bytes} bytes) を超えています"
                    ) from None
                chunks.append(part)
                break
            if not part:
                break
            chunks.append(part)
            received += len(part)
            if received > max_bytes:
                raise ImageGenerationError(f"画像生成 API の応答が上限 ({max_bytes} bytes) を超えています")
        return b"".join(chunks)
    chunks = []
    received_bytes = 0
    while received_bytes <= max_bytes:
        if cancel_check is not None and cancel_check():
            raise ImageGenerationError("画像データの受信をキャンセルしました")
        read_size = min(64 * 1024, max_bytes + 1 - received_bytes)
        try:
            chunk = cast(bytes, response.read(read_size))
        except TypeError:
            chunk = cast(bytes, response.read())
            if not chunk:
                break
            if len(chunk) + received_bytes > max_bytes:
                raise ImageGenerationError(f"画像生成 API の応答が上限 ({max_bytes} bytes) を超えています") from None
            chunks.append(chunk)
            received_bytes += len(chunk)
            break
        if not chunk:
            break
        chunks.append(chunk)
        received_bytes += len(chunk)
    if received_bytes > max_bytes:
        raise ImageGenerationError(f"画像生成 API の応答が上限 ({max_bytes} bytes) を超えています")
    return b"".join(chunks)


def _validate_endpoint_url(url_str: str, _api_key: str = "") -> str:
    url = url_str.strip()
    if not url:
        raise ImageGenerationError("エンドポイント URL が空です")
    parsed = urlsplit(url)
    scheme = parsed.scheme.lower()
    if scheme not in ("http", "https"):
        raise ImageGenerationError("エンドポイント URL は http:// または https:// で始まる必要があります")
    if parsed.username or parsed.password:
        raise ImageGenerationError("エンドポイント URL 内にユーザー名・パスワードを含めることはできません")
    hostname = (parsed.hostname or "").lower()
    if not hostname:
        raise ImageGenerationError("エンドポイント URL にホスト名が含まれていません")

    is_loopback = False
    if hostname in ("localhost", "127.0.0.1", "::1", "0.0.0.0", "::") or hostname.endswith(".localhost"):
        is_loopback = True
    else:
        try:
            ip = ipaddress.ip_address(hostname)
            if ip.is_loopback:
                is_loopback = True
        except ValueError:
            pass

    if scheme == "http" and not is_loopback:
        raise ImageGenerationError(
            f"外部エンドポイント '{hostname}' への接続には暗号化された HTTPS (https://) が必須です。"
        )
    return url


def _validate_image_download_url(url_str: str, source_endpoint: str) -> str:
    """Validate an image URL returned by an API without opening private hosts."""
    url = _validate_endpoint_url(url_str)
    source_origin = _url_origin(source_endpoint)
    target_origin = _url_origin(url)
    if source_origin is not None and source_origin == target_origin:
        return url

    parsed = urlsplit(url)
    hostname = (parsed.hostname or "").lower().rstrip(".")
    if parsed.scheme.lower() != "https":
        raise ImageGenerationError("API が返した画像 URL は HTTPS または同一オリジンである必要があります")
    if hostname in {"localhost", "localhost.localdomain"}:
        raise ImageGenerationError("API が返した画像 URL のローカルホスト接続を拒否しました")
    try:
        address = ipaddress.ip_address(hostname)
    except ValueError:
        address = None
    if address is not None and not address.is_global:
        raise ImageGenerationError("API が返した画像 URL のプライベート／予約済みアドレス接続を拒否しました")
    return url


class ImageGeneratorClient:
    """Text-to-Image 画像生成 API を呼び出し、PNG/JPEG 画像バイトを返すクライアント。"""

    def __init__(
        self,
        settings: ImageGeneratorSettings | None = None,
        log_callback: Callable[[str], None] | None = None,
    ) -> None:
        self.settings = settings or ImageGeneratorSettings()
        self._log_cb = log_callback

    def _log(self, message: str) -> None:
        if self._log_cb:
            self._log_cb(sanitize_api_key_log(message))

    def generate_image(
        self,
        prompt: str,
        *,
        cancel_check: Callable[[], bool] | None = None,
        target_aspect: float = 1.0,
    ) -> bytes:
        """プロンプトから画像を生成し、画像 bytes (PNG/JPEG) を返す。"""
        if not prompt or not prompt.strip():
            raise ImageGenerationError("画像生成プロンプトが空です")

        endpoint = _validate_endpoint_url(self.settings.endpoint_url, self.settings.api_key)
        provider = self.settings.provider.lower().strip()

        if cancel_check and cancel_check():
            raise ImageGenerationError("画像生成がキャンセルされました")

        self._log(f"画像生成リクエスト開始 [Provider: {provider}, Model: {self.settings.model}]...")

        if provider == "openai" or "openai.com" in endpoint:
            return self._call_openai_images(endpoint, prompt, target_aspect=target_aspect, cancel_check=cancel_check)
        elif provider == "sd_webui" or "/sdapi/" in endpoint:
            return self._call_sd_webui(endpoint, prompt, cancel_check=cancel_check)
        else:
            # 汎用/OpenAI互換リクエストを試行
            return self._call_openai_images(endpoint, prompt, target_aspect=target_aspect, cancel_check=cancel_check)

    def _call_openai_images(
        self,
        endpoint: str,
        prompt: str,
        *,
        target_aspect: float = 1.0,
        cancel_check: Callable[[], bool] | None = None,
    ) -> bytes:
        # アスペクト比に応じた適切な解像度を選択 (DALL-E 3: 1024x1024, 1024x1792, 1792x1024)
        chosen_size = self.settings.size
        model_name = self.settings.model.strip() or "dall-e-3"
        if "dall-e-3" in model_name.lower():
            if target_aspect >= 1.35:
                chosen_size = "1792x1024"
            elif target_aspect <= 0.75:
                chosen_size = "1024x1792"
            else:
                chosen_size = "1024x1024"

        payload: dict[str, Any] = {
            "model": model_name,
            "prompt": prompt.strip(),
            "n": 1,
            "size": chosen_size,
            "response_format": "b64_json",
        }
        if "dall-e-3" in model_name.lower():
            payload["quality"] = self.settings.quality
            payload["style"] = self.settings.style

        data_bytes = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "AIStrokePainter/2.0",
        }
        if self.settings.api_key.strip():
            headers["Authorization"] = f"Bearer {self.settings.api_key.strip()}"

        req = Request(endpoint, data=data_bytes, headers=headers, method="POST")
        opener = build_opener(_SameOriginRedirectHandler())

        try:
            start_time = time.time()
            with opener.open(req, timeout=max(5.0, self.settings.timeout_seconds)) as resp:
                resp_bytes = _read_bounded_stream(resp, MAX_IMAGE_RESPONSE_BYTES, cancel_check=cancel_check)
                elapsed = time.time() - start_time
                status_code = getattr(resp, "status", getattr(resp, "code", 200))
                self._log(f"画像生成 API 応答受信 ({elapsed:.1f}s, HTTP {status_code})")

            result_json = json.loads(resp_bytes.decode("utf-8", errors="replace"))
            if not isinstance(result_json, Mapping):
                raise ImageGenerationError("画像生成 API の応答は JSON オブジェクトである必要があります")

            data_list = result_json.get("data", [])
            if not isinstance(data_list, list) or not data_list:
                raise ImageGenerationError("画像生成 API の応答に画像データが含まれていません")

            first_item = data_list[0]
            if not isinstance(first_item, Mapping):
                raise ImageGenerationError("画像生成 API のデータ項目が不正です")

            if "b64_json" in first_item:
                b64_str = first_item["b64_json"]
                if not isinstance(b64_str, str):
                    raise ImageGenerationError("b64_json は文字列である必要があります")
                if len(b64_str) > MAX_IMAGE_RESPONSE_BYTES * 2:
                    raise ImageGenerationError("画像データが上限を超えています")
                image_bytes = base64.b64decode(b64_str, validate=True)
                return sanitize_reference_image(image_bytes, max_dimension=1024)
            elif "url" in first_item:
                raw_url = first_item["url"]
                if not isinstance(raw_url, str) or not raw_url.strip():
                    raise ImageGenerationError("画像 URL が無効です")
                validated_img_url = _validate_image_download_url(raw_url, endpoint)
                self._log("画像 URL から画像データをダウンロード中...")
                img_req = Request(validated_img_url, headers={"User-Agent": "AIStrokePainter/2.0"})
                with opener.open(img_req, timeout=30.0) as img_resp:
                    raw_img = _read_bounded_stream(img_resp, MAX_IMAGE_RESPONSE_BYTES, cancel_check=cancel_check)
                return sanitize_reference_image(raw_img, max_dimension=1024)
            else:
                raise ImageGenerationError("画像生成 API の応答形式が不明です (b64_json/url がありません)")

        except ImageGenerationError:
            raise
        except _CrossOriginRedirectError as e:
            raise ImageGenerationError("画像ダウンロードで別オリジンへのリダイレクトを拒否しました") from e
        except HTTPError as e:
            err_body = ""
            with contextlib.suppress(Exception):
                err_body = e.read().decode("utf-8", errors="replace")[:300]
            raise ImageGenerationError(
                f"画像生成 API エラー (HTTP {e.code}): {sanitize_api_key_log(err_body or str(e.reason))}"
            ) from e
        except (URLError, TimeoutError, OSError) as e:
            raise ImageGenerationError(f"画像生成 API 接続エラー: {e}") from e
        except json.JSONDecodeError as e:
            raise ImageGenerationError(f"画像生成 API の応答 JSON 解析に失敗しました: {e}") from e
        except (binascii.Error, ValueError, TypeError, KeyError, AttributeError) as e:
            raise ImageGenerationError(f"画像生成データの処理に失敗しました: {e}") from e

    def _call_sd_webui(
        self,
        endpoint: str,
        prompt: str,
        *,
        cancel_check: Callable[[], bool] | None = None,
    ) -> bytes:
        payload: dict[str, Any] = {
            "prompt": prompt.strip(),
            "negative_prompt": self.settings.negative_prompt.strip()
            or "low quality, worst quality, deformed, bad anatomy, blurry",
            "steps": 25,
            "width": 1024 if "1024" in self.settings.size else 512,
            "height": 1024 if "1024" in self.settings.size else 512,
            "cfg_scale": 7.0,
            "sampler_name": "Euler a",
        }
        data_bytes = json.dumps(payload).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json",
            "User-Agent": "AIStrokePainter/2.0",
        }
        if self.settings.api_key.strip():
            headers["Authorization"] = f"Bearer {self.settings.api_key.strip()}"

        req = Request(endpoint, data=data_bytes, headers=headers, method="POST")
        opener = build_opener(_SameOriginRedirectHandler())

        try:
            start_time = time.time()
            with opener.open(req, timeout=max(5.0, self.settings.timeout_seconds)) as resp:
                resp_bytes = _read_bounded_stream(resp, MAX_IMAGE_RESPONSE_BYTES, cancel_check=cancel_check)
                elapsed = time.time() - start_time
                status_code = getattr(resp, "status", getattr(resp, "code", 200))
                self._log(f"SD WebUI 応答受信 ({elapsed:.1f}s, HTTP {status_code})")

            result_json = json.loads(resp_bytes.decode("utf-8", errors="replace"))
            if not isinstance(result_json, Mapping):
                raise ImageGenerationError("SD WebUI 応答は JSON オブジェクトである必要があります")

            images_list = result_json.get("images", [])
            if not isinstance(images_list, list) or not images_list:
                raise ImageGenerationError("SD WebUI 応答に画像が含まれていません")

            b64_str = images_list[0]
            if not isinstance(b64_str, str):
                raise ImageGenerationError("SD WebUI の画像データ形式が不正です")

            if "," in b64_str:
                b64_str = b64_str.split(",", 1)[1]
            if len(b64_str) > MAX_IMAGE_RESPONSE_BYTES * 2:
                raise ImageGenerationError("画像データが上限を超えています")
            image_bytes = base64.b64decode(b64_str, validate=True)
            return sanitize_reference_image(image_bytes, max_dimension=1024)

        except ImageGenerationError:
            raise
        except _CrossOriginRedirectError as e:
            raise ImageGenerationError("SD WebUI で別オリジンへのリダイレクトを拒否しました") from e
        except HTTPError as e:
            err_body = ""
            with contextlib.suppress(Exception):
                err_body = e.read().decode("utf-8", errors="replace")[:300]
            raise ImageGenerationError(
                f"SD WebUI エラー (HTTP {e.code}): {sanitize_api_key_log(err_body or str(e.reason))}"
            ) from e
        except (URLError, TimeoutError, OSError) as e:
            raise ImageGenerationError(f"SD WebUI 接続エラー: {e}") from e
        except json.JSONDecodeError as e:
            raise ImageGenerationError(f"SD WebUI の応答 JSON 解析に失敗しました: {e}") from e
        except (binascii.Error, ValueError, TypeError, KeyError, AttributeError) as e:
            raise ImageGenerationError(f"SD WebUI 画像データの処理に失敗しました: {e}") from e
