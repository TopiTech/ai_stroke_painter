"""Krita の「Python プラグインをインポート」で使える ZIP を作成する。"""

from __future__ import annotations

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


PACKAGE_NAME = "ai_stroke_painter"
EXCLUDED_NAMES = {"__pycache__", ".git", "dist"}
EXCLUDED_FILES = {
    ".gitignore",
    "ARCHITECTURE.md",
    "README.md",
    "build_plugin.py",
    "self_test.py",
    PACKAGE_NAME + ".desktop",
}


def build(output: Path) -> Path:
    source = Path(__file__).resolve().parent
    manifest = source / (PACKAGE_NAME + ".desktop")
    if not manifest.is_file():
        raise FileNotFoundError("Krita manifest が見つかりません: %s" % manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        archive.write(manifest, manifest.name)
        for path in sorted(source.rglob("*")):
            if not path.is_file() or any(part in EXCLUDED_NAMES for part in path.parts):
                continue
            if path.name in EXCLUDED_FILES or path.suffix in {".pyc", ".pyo"}:
                continue
            archive.write(path, str(Path(PACKAGE_NAME) / path.relative_to(source)))
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("dist") / (PACKAGE_NAME + ".zip"))
    args = parser.parse_args()
    print(build(args.output.resolve()))


if __name__ == "__main__":
    main()
