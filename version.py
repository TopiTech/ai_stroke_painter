"""実行中プラグインを生成結果から一意に追跡するための版情報。"""

from __future__ import annotations

from functools import lru_cache
import hashlib
from pathlib import Path

PLUGIN_VERSION = "0.5.0"
GENERATION_TRACE_VERSION = 1


@lru_cache(maxsize=1)
def source_fingerprint() -> str:
    """インストール済みPythonソースの短い決定論的指紋を返す。"""
    package_root = Path(__file__).resolve().parent
    digest = hashlib.sha256()
    paths = sorted(package_root.glob("*.py")) + sorted((package_root / "procedural").glob("*.py"))
    for path in paths:
        if not path.is_file() or "__pycache__" in path.parts:
            continue
        relative = path.relative_to(package_root).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(2, "big"))
        digest.update(relative)
        digest.update(path.read_bytes())
    return digest.hexdigest()[:16]


def generation_trace(*, generator: str, requested_count: int | None) -> dict[str, object]:
    return {
        "version": GENERATION_TRACE_VERSION,
        "plugin_version": PLUGIN_VERSION,
        "source_fingerprint": source_fingerprint(),
        "generator": generator,
        "count_mode": "auto" if requested_count is None else "manual",
        "requested_count": requested_count,
    }
