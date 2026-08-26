"""ユーザーのデータ領域へ DrawingPlan JSON および SVG を安全に保存・読込する。"""

from __future__ import annotations

import json
import os
from pathlib import Path
import time

from .domain import DrawingPlan
from .stroke_program import (
    PROGRAM_SCHEMA_VERSION,
    StrokeProgram,
    compile_stroke_program,
    drawing_plan_to_stroke_program,
)

MAX_PLAN_FILE_BYTES = 50 * 1024 * 1024


def app_data_dir() -> Path:
    base = os.environ.get("APPDATA") or os.environ.get("XDG_DATA_HOME")
    root = Path(base) if base else Path.home() / ".local" / "share"
    output = root / "ai_stroke_painter"
    output.mkdir(parents=True, exist_ok=True)
    return output


def _resolve_output_dir(directory: str | Path | None) -> Path:
    if directory is None:
        return app_data_dir()
    requested = Path(directory)
    if requested.is_absolute():
        resolved = requested.resolve()
        if ".." in Path(directory).parts:
            raise ValueError(f"保存先に親ディレクトリ参照を含めることはできません: {directory}")
        return resolved
    output = (app_data_dir() / requested).resolve()
    allowed = app_data_dir().resolve()
    try:
        output.relative_to(allowed)
    except ValueError as exc:
        if output != allowed:
            raise ValueError(f"保存先は {allowed} 配下である必要があります: {output}") from exc
    return output


def save_plan(plan: DrawingPlan, directory: str | Path | None = None) -> Path:
    """同時保存でも上書きせず、作成した JSON のパスを返す。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    output = _resolve_output_dir(directory)
    output.mkdir(parents=True, exist_ok=True)
    contents = json.dumps(plan.as_dict(), ensure_ascii=False, indent=2) + "\n"
    if len(contents.encode("utf-8")) > MAX_PLAN_FILE_BYTES:
        raise ValueError(f"計画 JSON が上限 ({MAX_PLAN_FILE_BYTES // (1024 * 1024)}MB) を超えています")
    timestamp = time.time_ns()
    for suffix in range(1000):
        name = f"plan_{timestamp}{'' if suffix == 0 else f'_{suffix}'}.json"
        path = output / name
        try:
            with path.open("x", encoding="utf-8") as handle:
                handle.write(contents)
            return path
        except FileExistsError:
            continue
    raise RuntimeError("計画 JSON の一意な保存先を確保できませんでした")


def save_svg(plan: DrawingPlan, directory: str | Path | None = None) -> Path:
    """計画を SVG ベクターファイルとして保存する。"""
    if not isinstance(plan, DrawingPlan):
        raise TypeError("plan は DrawingPlan である必要があります")
    output = _resolve_output_dir(directory)
    output.mkdir(parents=True, exist_ok=True)
    contents = plan.to_svg()
    if len(contents.encode("utf-8")) > MAX_PLAN_FILE_BYTES:
        raise ValueError(f"SVG が上限 ({MAX_PLAN_FILE_BYTES // (1024 * 1024)}MB) を超えています")
    timestamp = time.time_ns()
    for suffix in range(1000):
        name = f"artwork_{timestamp}{'' if suffix == 0 else f'_{suffix}'}.svg"
        path = output / name
        try:
            with path.open("x", encoding="utf-8") as handle:
                handle.write(contents)
            return path
        except FileExistsError:
            continue
    raise RuntimeError("SVG の一意な保存先を確保できませんでした")


def load_plan(path: str | Path) -> DrawingPlan:
    """保存済み v1 DrawingPlan / v2 StrokeProgram を検証して描画可能な計画として返す。"""
    source = Path(path)
    if not source.is_file():
        raise FileNotFoundError(f"計画ファイルが見つかりません: {source}")
    if source.stat().st_size > MAX_PLAN_FILE_BYTES:
        raise ValueError(f"計画 JSON は {MAX_PLAN_FILE_BYTES // (1024 * 1024)}MB 以下である必要があります")
    try:
        contents = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"計画 JSON の形式が不正です: {source}") from exc
    if isinstance(contents, dict) and (
        contents.get("schema_version") == PROGRAM_SCHEMA_VERSION or "operations" in contents
    ):
        return compile_stroke_program(StrokeProgram.from_dict(contents))
    return DrawingPlan.from_dict(contents)


def save_program(program: StrokeProgram, directory: str | Path | None = None) -> Path:
    """編集可能な高水準 StrokeProgram を上書きせず JSON 保存する。"""
    if not isinstance(program, StrokeProgram):
        raise TypeError("program は StrokeProgram である必要があります")
    output = _resolve_output_dir(directory)
    output.mkdir(parents=True, exist_ok=True)
    contents = json.dumps(program.as_dict(), ensure_ascii=False, indent=2) + "\n"
    if len(contents.encode("utf-8")) > MAX_PLAN_FILE_BYTES:
        raise ValueError(f"プログラム JSON が上限 ({MAX_PLAN_FILE_BYTES // (1024 * 1024)}MB) を超えています")
    timestamp = time.time_ns()
    for suffix in range(1000):
        name = f"program_{timestamp}{'' if suffix == 0 else f'_{suffix}'}.json"
        path = output / name
        try:
            with path.open("x", encoding="utf-8") as handle:
                handle.write(contents)
            return path
        except FileExistsError:
            continue
    raise RuntimeError("StrokeProgram JSON の一意な保存先を確保できませんでした")


def load_program(path: str | Path) -> StrokeProgram:
    """v2 JSON を読込み、v1 DrawingPlan は path operation 群へ移行する。"""
    source = Path(path)
    if not source.is_file():
        raise FileNotFoundError(f"プログラムファイルが見つかりません: {source}")
    if source.stat().st_size > MAX_PLAN_FILE_BYTES:
        raise ValueError(f"計画 JSON は {MAX_PLAN_FILE_BYTES // (1024 * 1024)}MB 以下である必要があります")
    try:
        contents = json.loads(source.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ValueError(f"計画 JSON の形式が不正です: {source}") from exc
    if isinstance(contents, dict) and (
        contents.get("schema_version") == PROGRAM_SCHEMA_VERSION or "operations" in contents
    ):
        return StrokeProgram.from_dict(contents)
    return drawing_plan_to_stroke_program(DrawingPlan.from_dict(contents))
