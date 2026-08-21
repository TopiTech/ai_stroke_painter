"""Krita の「Python プラグインをインポート」で使える ZIP を作成する。"""

from __future__ import annotations

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


PACKAGE_NAME = "ai_stroke_painter"
PACKAGE_FILES = (
    "Manual.html",
    "__init__.py",
    "docker.py",
    "domain.py",
    "krita_adapter.py",
    "llm_planner.py",
    "planner.py",
    "ports.py",
    "storage.py",
)


def build(output: Path) -> Path:
    source = Path(__file__).resolve().parent
    manifest = source / (PACKAGE_NAME + ".desktop")
    if not manifest.is_file():
        raise FileNotFoundError("Krita manifest が見つかりません: %s" % manifest)
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, "w", ZIP_DEFLATED) as archive:
        archive.write(manifest, manifest.name)
        archive.writestr(f"{PACKAGE_NAME}/", "")
        for relative_path in PACKAGE_FILES:
            path = source / relative_path
            if path.is_file():
                archive.write(path, f"{PACKAGE_NAME}/{Path(relative_path).as_posix()}")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", "--output", type=Path, default=Path("dist") / (PACKAGE_NAME + ".zip"))
    args = parser.parse_args()
    print(build(args.output.resolve()))


if __name__ == "__main__":
    main()
