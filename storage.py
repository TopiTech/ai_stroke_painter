"""ユーザーのデータ領域へ DrawingPlan JSON を安全に保存・読込する。"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Optional, Union

from .domain import DrawingPlan


def app_data_dir() -> Path:
    base = os.environ.get("APPDATA") or os.environ.get("XDG_DATA_HOME")
    root = Path(base) if base else Path.home() / ".local" / "share"
    output = root / "ai_stroke_painter"
    output.mkdir(parents=True, exist_ok=True)
    return output


def save_plan(plan: DrawingPlan, directory: Optional[Union[str, Path]] = None) -> Path:
    """同時保存でも上書きせず、作成した JSON のパスを返す。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    output = Path(directory) if directory is not None else app_data_dir()
    output.mkdir(parents=True, exist_ok=True)
    contents = json.dumps(plan.as_dict(), ensure_ascii=False, indent=2) + "\n"
    timestamp = time.time_ns()
    for suffix in range(1000):
        name = "plan_%d%s.json" % (timestamp, "" if suffix == 0 else "_%d" % suffix)
        path = output / name
        try:
            with path.open("x", encoding="utf-8") as handle:
                handle.write(contents)
            return path
        except FileExistsError:
            continue
    raise RuntimeError("計画 JSON の一意な保存先を確保できませんでした")


def load_plan(path: Union[str, Path]) -> DrawingPlan:
    """保存済み JSON を検証して DrawingPlan として返す。"""
    source = Path(path)
    try:
        contents = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError("計画 JSON の形式が不正です: %s" % source) from exc
    return DrawingPlan.from_dict(contents)
