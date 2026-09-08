"""Krita 6 内で連続描画・レイヤー・トランザクション境界を確認するスモークテスト。"""

from __future__ import annotations

import json
from typing import Any

from .domain import DrawingPlan, Stroke, StrokePoint
from .krita_adapter import KritaCanvasAdapter


def _close_disposable_document(document: Any) -> None:
    if hasattr(document, "setModified"):
        document.setModified(False)
    close = getattr(document, "close", None)
    if callable(close):
        close()


def run_krita_smoke_test() -> dict[str, Any]:
    """新規256px文書だけを使用し、成功結果を辞書で返す。既存文書は変更しない。"""
    try:
        from krita import Krita
    except ImportError as exc:
        raise RuntimeError("このスモークテストはKrita内蔵Pythonで実行してください") from exc

    app = Krita.instance()
    if app is None:
        raise RuntimeError("Krita application instance を取得できません")
    document = app.createDocument(
        256,
        256,
        "AI Stroke Painter Smoke Test",
        "RGBA",
        "U8",
        "sRGB-elle-V2-srgbtrc.icc",
        120.0,
    )
    if document is None:
        raise RuntimeError("Kritaスモーク用ドキュメントを作成できません")
    window = app.activeWindow()
    view = window.addView(document) if window is not None and hasattr(window, "addView") else None
    adapter = KritaCanvasAdapter(layer_mode="multi_layer", event_interval=1)
    plan = DrawingPlan(
        prompt="Krita integration smoke",
        seed=1,
        strokes=(
            Stroke(
                "smoke-fill",
                (
                    StrokePoint(24, 80, 1.0, 0),
                    StrokePoint(232, 80, 1.0, 10),
                    StrokePoint(232, 176, 1.0, 20),
                    StrokePoint(24, 176, 1.0, 30),
                ),
                brush_preset="Basic-5 Size",
                color="#4a90e2",
                size_px=36,
                layer_name="Flats",
            ),
            Stroke(
                "smoke-line",
                (
                    StrokePoint(32, 196, 0.1, 0),
                    StrokePoint(96, 40, 0.95, 10),
                    StrokePoint(160, 40, 0.8, 20),
                    StrokePoint(224, 196, 0.1, 30),
                ),
                brush_preset="Ink-3 Gpen",
                color="#202030",
                size_px=6,
                layer_name="Lineart",
            ),
        ),
        canvas_width=256,
        canvas_height=256,
    )
    session_open = False
    try:
        adapter.begin_render_session(document)
        session_open = True
        rendered = adapter.render(document, plan, layer_prefix="AI Stroke Smoke", view=view)
        if rendered != len(plan.strokes):
            raise RuntimeError(f"描画本数が一致しません: {rendered}/{len(plan.strokes)}")
        document.waitForDone()
        group_names = [str(child.name()) for child in document.rootNode().childNodes()]
        if not any(name.startswith("AI Stroke Smoke") for name in group_names):
            raise RuntimeError("出力グループが作成されませんでした")
        projection = document.rootNode().projectionPixelData(0, 0, 256, 256)
        pixel_bytes = bytes(projection) if projection is not None else b""
        if not pixel_bytes or not any(pixel_bytes):
            raise RuntimeError("Krita投影に描画済みピクセルがありません")
        adapter.end_render_session(commit=True)
        session_open = False
        return {
            "passed": True,
            "rendered_strokes": rendered,
            "layers": group_names,
            "projection_bytes": len(pixel_bytes),
        }
    finally:
        if session_open:
            adapter.end_render_session(commit=False)
        _close_disposable_document(document)


def main() -> int:
    try:
        result = run_krita_smoke_test()
    except Exception as exc:
        print(json.dumps({"passed": False, "error": str(exc)}, ensure_ascii=False))
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
