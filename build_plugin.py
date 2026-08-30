"""Krita の「Python プラグインをインポート」で使える ZIP を作成する。"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

PACKAGE_NAME = "ai_stroke_painter"
PACKAGE_FILES = (
    "Manual.html",
    "__init__.py",
    "brushes.py",
    "docker.py",
    "domain.py",
    "image_converter.py",
    "image_generator.py",
    "krita_adapter.py",
    "krita_smoke.py",
    "llm_planner.py",
    "native_bridge.py",
    "planner.py",
    "ports.py",
    "prompt_analyzer.py",
    "quality.py",
    "quality_check.py",
    "qt_compat.py",
    "scene_spec.py",
    "storage.py",
    "stroke_program.py",
    "version.py",
    "procedural/__init__.py",
    "procedural/base.py",
    "procedural/character.py",
    "procedural/color_plan.py",
    "procedural/composition.py",
    "procedural/creature.py",
    "procedural/geometry.py",
    "procedural/landscape.py",
    "procedural/manga_fx.py",
    "procedural/render_graph.py",
    "procedural/semantic_budget.py",
)


def build(output: Path) -> Path:
    source = Path(__file__).resolve().parent
    manifest = source / f"{PACKAGE_NAME}.desktop"
    if not manifest.is_file():
        raise FileNotFoundError(f"Krita manifest が見つかりません: {manifest}")
    missing = [relative_path for relative_path in PACKAGE_FILES if not (source / relative_path).is_file()]
    if missing:
        missing_list = ", ".join(missing)
        raise FileNotFoundError(f"配布パッケージの必須ファイルが見つかりません: {missing_list}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent, delete=False
    ) as temp_handle:
        temp_path = Path(temp_handle.name)
    try:
        with ZipFile(temp_path, "w", ZIP_DEFLATED) as archive:
            archive.write(manifest, manifest.name)
            archive.writestr(f"{PACKAGE_NAME}/", "")
            for relative_path in PACKAGE_FILES:
                path = source / relative_path
                archive.write(path, f"{PACKAGE_NAME}/{Path(relative_path).as_posix()}")
        os.replace(temp_path, output)
    finally:
        temp_path.unlink(missing_ok=True)
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("dist") / f"{PACKAGE_NAME}.zip")
    args = parser.parse_args()
    print(build(args.output.resolve()))


if __name__ == "__main__":
    main()
