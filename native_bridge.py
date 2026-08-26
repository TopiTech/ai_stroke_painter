"""連続ストロークをローカル Native helper へ渡す認証付き JSON-lines bridge。"""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
import ipaddress
import json
import math
import os
import socket
from typing import Any

from .domain import Stroke
from .ports import NativeStrokeBridgePort

NATIVE_BRIDGE_PROTOCOL_VERSION = 1
MAX_BRIDGE_MESSAGE_BYTES = 2 * 1024 * 1024
MAX_BRIDGE_RESPONSE_BYTES = 64 * 1024


class NativeBridgeUnavailable(ConnectionError):
    """helper が起動していないため互換描画へ戻せる場合。"""


class NativeBridgeProtocolError(RuntimeError):
    """helper が不正または不完全な応答を返した場合。"""


def _local_host(host: str) -> bool:
    normalized = host.strip().lower().rstrip(".")
    if normalized == "localhost":
        return True
    try:
        return ipaddress.ip_address(normalized).is_loopback
    except ValueError:
        return False


def _object_identifier(value: Any) -> str:
    for attribute in ("uniqueId", "uuid", "name"):
        getter = getattr(value, attribute, None)
        if callable(getter):
            try:
                result = getter()
            except Exception:
                continue
            if result is not None and str(result).strip():
                return str(result)
    return f"python:{id(value)}"


@dataclass(frozen=True)
class JsonLineNativeStrokeBridge(NativeStrokeBridgePort):
    """明示されたloopback helperへ1ストロークを1メッセージで送る。"""

    port: int
    auth_token: str
    host: str = "127.0.0.1"
    timeout_seconds: float = 2.0

    def __post_init__(self) -> None:
        if not _local_host(self.host):
            raise ValueError("Native Bridge はループバックアドレスのみ使用できます")
        if isinstance(self.port, bool) or not isinstance(self.port, int) or not 1 <= self.port <= 65_535:
            raise ValueError("Native Bridge port は 1 から 65535 の整数である必要があります")
        if (
            not isinstance(self.auth_token, str)
            or not 16 <= len(self.auth_token) <= 4_096
            or any(ord(character) < 32 for character in self.auth_token)
        ):
            raise ValueError("Native Bridge auth token は制御文字を含まない16から4096文字である必要があります")
        if isinstance(self.timeout_seconds, bool):
            raise ValueError("Native Bridge timeout は数値である必要があります")
        timeout = float(self.timeout_seconds)
        if not math.isfinite(timeout) or not 0.05 <= timeout <= 30.0:
            raise ValueError("Native Bridge timeout は0.05から30秒の範囲である必要があります")

    def submit_stroke(self, document: Any, target: Any, stroke: Stroke) -> int:
        if not isinstance(stroke, Stroke):
            raise TypeError("stroke は Stroke である必要があります")
        payload = {
            "protocol_version": NATIVE_BRIDGE_PROTOCOL_VERSION,
            "type": "continuous_stroke",
            "auth_token": self.auth_token,
            "document_id": _object_identifier(document),
            "target_id": _object_identifier(target),
            "stroke": stroke.as_dict(),
        }
        message = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8") + b"\n"
        if len(message) > MAX_BRIDGE_MESSAGE_BYTES:
            raise NativeBridgeProtocolError("Native Bridge メッセージが上限を超えています")
        try:
            connection = socket.create_connection((self.host, self.port), timeout=float(self.timeout_seconds))
        except (ConnectionError, OSError, TimeoutError) as exc:
            # 接続が成立していない場合だけ互換描画へ戻せる。送信開始後はhelper側で
            # 適用済みか判定不能なため、重複描画を避けて必ず fail closed にする。
            raise NativeBridgeUnavailable("Native Bridge helper に接続できません") from exc
        try:
            with connection:
                connection.settimeout(float(self.timeout_seconds))
                connection.sendall(message)
                response_bytes = self._read_response(connection)
        except NativeBridgeProtocolError:
            raise
        except (ConnectionError, OSError, TimeoutError) as exc:
            raise NativeBridgeProtocolError("Native Bridge 送信後の完了状態を確認できません") from exc

        try:
            response = json.loads(response_bytes.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise NativeBridgeProtocolError("Native Bridge 応答がJSONではありません") from exc
        return self._validate_response(response, stroke)

    @staticmethod
    def _read_response(connection: socket.socket) -> bytes:
        chunks: list[bytes] = []
        total = 0
        while True:
            chunk = connection.recv(min(4096, MAX_BRIDGE_RESPONSE_BYTES + 1 - total))
            if not chunk:
                break
            newline = chunk.find(b"\n")
            if newline >= 0:
                sliced = chunk[:newline]
                chunks.append(sliced)
                total += len(sliced)
                if total > MAX_BRIDGE_RESPONSE_BYTES:
                    raise NativeBridgeProtocolError("Native Bridge 応答が上限を超えています")
                break
            chunks.append(chunk)
            total += len(chunk)
            if total > MAX_BRIDGE_RESPONSE_BYTES:
                raise NativeBridgeProtocolError("Native Bridge 応答が上限を超えています")
        response = b"".join(chunks)
        if len(response) > MAX_BRIDGE_RESPONSE_BYTES:
            raise NativeBridgeProtocolError("Native Bridge 応答が上限を超えています")
        if not response:
            raise NativeBridgeProtocolError("Native Bridge 応答が空です")
        return response

    @staticmethod
    def _validate_response(response: Any, stroke: Stroke) -> int:
        if not isinstance(response, Mapping):
            raise NativeBridgeProtocolError("Native Bridge 応答はオブジェクトである必要があります")
        if response.get("ok") is not True:
            error = response.get("error", "unknown native bridge error")
            error_text = error if isinstance(error, str) else str(error)
            raise NativeBridgeProtocolError(f"Native Bridge が描画を拒否しました: {error_text[:300]}")
        protocol_version = response.get("protocol_version")
        if protocol_version is not None and protocol_version != NATIVE_BRIDGE_PROTOCOL_VERSION:
            raise NativeBridgeProtocolError("Native Bridge プロトコルバージョンが不一致です")
        accepted = response.get("accepted_point_count")
        if isinstance(accepted, bool) or not isinstance(accepted, int) or accepted != len(stroke.points):
            raise NativeBridgeProtocolError("Native Bridge の受理点数が要求と一致しません")
        return accepted


def discover_native_bridge() -> JsonLineNativeStrokeBridge | None:
    """環境変数が完全に設定された場合だけ bridge を有効化する。"""
    raw_port = os.environ.get("AI_STROKE_BRIDGE_PORT", "").strip()
    token = os.environ.get("AI_STROKE_BRIDGE_TOKEN", "").strip()
    if not raw_port and not token:
        return None
    if not raw_port or not token:
        return None
    try:
        port = int(raw_port)
        return JsonLineNativeStrokeBridge(port=port, auth_token=token)
    except (TypeError, ValueError) as exc:
        raise ValueError("Native Bridge 環境変数の設定が不正です") from exc
