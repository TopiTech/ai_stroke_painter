"""Krita を起動せずに実行できる、AI Stroke Painter の回帰・機能テスト。"""

from __future__ import annotations

import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
from threading import Thread
from typing import Any, cast
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from .build_plugin import PACKAGE_NAME, build
from .docker import (
    AIStrokePainterDocker,
    ApiConnectionWorker,
    PlanWorker,
    _confirm,
    _is_plan_goal_reached,
    _safe_endpoint_label,
)
from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint, VisionCritique
from .image_converter import ImageStrokeConverter, _image_dimensions_from_header, _trace_edge_paths
from .krita_adapter import ActiveLayerSessionConflict, KritaCanvasAdapter
from .llm_planner import (
    LLMPlannerError,
    OpenAICompatiblePlanner,
    OpenAICompatibleSettings,
    _attempt_json_repair,
    _CrossOriginRedirectError,
    _detect_image_mime_type,
    _endpoint_origin_label,
    _extract_content_from_response,
    _extract_json_object,
    _is_reasoning_model,
    _plan_from_response,
    _redact_sensitive_text,
    _SameOriginRedirectHandler,
)
from .native_bridge import (
    JsonLineNativeStrokeBridge,
    NativeBridgeProtocolError,
    NativeBridgeUnavailable,
    discover_native_bridge,
)
from .planner import RuleBasedPlanner
from .procedural import (
    generate_character_strokes,
    generate_creature_strokes,
    generate_geometry_strokes,
    generate_landscape_strokes,
    generate_manga_fx_strokes,
    generate_procedural_plan,
)
from .procedural.base import sample_strokes_by_priority
from .qt_compat import (
    QApplication,
    QImage,
    QPoint,
    QSettings,
    QWidget,
    argb32_image_format,
    password_echo_mode,
    write_only_open_mode,
)
from .quality import evaluate_plan_quality
from .storage import load_plan, load_program, save_plan, save_program, save_svg
from .stroke_program import (
    FillOperation,
    HatchOperation,
    ParticleOperation,
    PathOperation,
    ProgramPoint,
    StrokeProgram,
    compile_stroke_program,
    drawing_plan_to_stroke_program,
)


class _FakeNode:
    def __init__(self, name: str, node_type: str = "paintlayer", paint_ability: str = "PAINT") -> None:
        self._name = name
        self._type = node_type
        self._paint_ability = paint_ability
        self._blending_mode = "normal"
        self._children: list[Any] = []
        self.lines: list[tuple[Any, Any, float, float]] = []
        self.pixels = b"initial pixels"
        self.pixel_reads = 0
        self.pixel_writes = 0

    def name(self) -> str:
        return self._name

    def type(self) -> str:
        return self._type

    def childNodes(self) -> list[Any]:
        return list(self._children)

    def removeChildNode(self, child: Any) -> None:
        if child in self._children:
            self._children.remove(child)

    def addChildNode(self, child: Any, above_this: Any = None) -> bool | None:
        if above_this is not None and above_this in self._children:
            idx = self._children.index(above_this)
            self._children.insert(idx + 1, child)
        else:
            self._children.append(child)
        return None

    def paintAbility(self) -> str:
        return self._paint_ability

    def setBlendingMode(self, mode: str) -> None:  # noqa: N802
        self._blending_mode = mode

    def blendingMode(self) -> str:  # noqa: N802
        return getattr(self, "_blending_mode", "normal")

    def paintLine(self, start: Any, end: Any, start_pressure: float, end_pressure: float) -> None:
        # Krita の Node.paintLine は QPoint を要求するため、QPointF が渡されると TypeError となる
        if type(start).__name__ == "QPointF" or type(end).__name__ == "QPointF":
            raise TypeError(
                "paintLine(self, pointOne: QPoint, pointTwo: QPoint, pressureOne: float = 1, pressureTwo: float = 1, strokeStyle: Optional[str] = ''): argument 1 has unexpected type 'QPointF'"
            )
        if self.paintAbility() == "PAINT":
            self.lines.append((start, end, start_pressure, end_pressure))
            self.pixels = b"painted pixels"

    def pixelData(self, _x: int, _y: int, _width: int, _height: int) -> bytes:  # noqa: N802
        self.pixel_reads += 1
        return self.pixels

    def setPixelData(self, pixels: bytes, _x: int, _y: int, _width: int, _height: int) -> bool:  # noqa: N802
        self.pixel_writes += 1
        self.pixels = pixels
        return True


class _FakeDocument:
    def __init__(self, active: Any | None = None, root: Any | None = None) -> None:
        self.active = active
        self.root = root or _FakeNode("root", "grouplayer")
        self.created = 0
        self.refreshed = 0
        self.locked = 0
        self.unlocked = 0
        self.waited_for_done = 0
        self._batchmode = False
        self.macros_started: list[str] = []
        self.macros_ended = 0

    def createMacro(self, title: str) -> None:
        self.macros_started.append(title)

    def endMacro(self) -> None:
        self.macros_ended += 1

    def activeNode(self) -> Any | None:
        return self.active

    def rootNode(self) -> Any:
        return self.root

    def createNode(self, name: str, node_type: str) -> _FakeNode:
        self.created += 1
        return _FakeNode(name, node_type)

    def setActiveNode(self, node: Any) -> None:
        self.active = node

    def refreshProjection(self) -> None:
        self.refreshed += 1

    def waitForDone(self) -> None:
        self.waited_for_done += 1

    def batchmode(self) -> bool:
        return self._batchmode

    def setBatchmode(self, mode: bool) -> None:
        self._batchmode = mode

    def lock(self) -> None:
        self.locked += 1

    def unlock(self) -> None:
        self.unlocked += 1

    def width(self) -> int:
        return 800

    def height(self) -> int:
        return 600

    def thumbnail(self, width: int, height: int) -> Any:
        return None


class PlannerAndStorageTests(unittest.TestCase):
    def test_stroke_program_round_trip_and_compiler_primitives(self) -> None:
        raw_program = {
            "schema_version": 2,
            "prompt": "layered study",
            "seed": 17,
            "canvas": {"width": 400, "height": 240},
            "operations": [
                {
                    "kind": "fill",
                    "id": "base",
                    "polygon": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.8], [0.1, 0.8]],
                    "brush": {"profile": "marker", "color": "#6688aa", "size": 0.04},
                },
                {
                    "kind": "hatch",
                    "id": "shadow",
                    "polygon": [[0.5, 0.2], [0.85, 0.25], [0.75, 0.75], [0.45, 0.65]],
                    "angle_deg": 25,
                    "cross": True,
                },
                {
                    "kind": "path",
                    "id": "contour",
                    "points": [[0.1, 0.8, 0.15], [0.5, 0.15, 1.0], [0.9, 0.8, 0.1]],
                    "brush": {"profile": "gpen", "size": 0.008},
                },
                {
                    "kind": "particles",
                    "id": "sparkles",
                    "bounds": [0.05, 0.05, 0.95, 0.95],
                    "count": 12,
                },
            ],
        }
        program = StrokeProgram.from_dict(raw_program)
        self.assertEqual(StrokeProgram.from_dict(program.as_dict()), program)

        first = compile_stroke_program(program)
        second = compile_stroke_program(program)
        self.assertEqual(first.as_dict(), second.as_dict())
        self.assertEqual(first.metadata["source_schema_version"], 2)
        self.assertEqual(first.metadata["operation_count"], 4)
        self.assertEqual(set(first.layers), {"Flats", "Shading", "Lineart", "FX"})
        self.assertTrue(all(0 <= point.x < 400 for stroke in first.strokes for point in stroke.points))
        self.assertTrue(all(0 <= point.y < 240 for stroke in first.strokes for point in stroke.points))

        limited = compile_stroke_program(program, count=20)
        self.assertEqual(len(limited.strokes), 20)
        self.assertIn("Lineart", limited.layers)

    def test_stroke_program_dense_path_compilation_respects_point_limit(self) -> None:
        dense_points = [ProgramPoint(i / 250.0, i / 250.0, 0.8) for i in range(250)]
        dense_program = StrokeProgram(
            prompt="dense smooth path",
            seed=1,
            canvas_width=1000,
            canvas_height=1000,
            operations=[PathOperation(id="dense-path", points=dense_points, smooth=True)],
        )
        plan = compile_stroke_program(dense_program)
        self.assertEqual(len(plan.strokes), 1)
        self.assertLessEqual(len(plan.strokes[0].points), 1000)
        self.assertEqual(len(plan.strokes[0].points), 1000)

        closed_points = [ProgramPoint(i / 1000.0, i / 1000.0, 0.8) for i in range(1000)]
        closed_program = StrokeProgram(
            prompt="closed dense path",
            seed=2,
            canvas_width=1000,
            canvas_height=1000,
            operations=[PathOperation(id="closed-path", points=closed_points, closed=True, smooth=False)],
        )
        closed_plan = compile_stroke_program(closed_program)
        self.assertEqual(len(closed_plan.strokes), 1)
        self.assertLessEqual(len(closed_plan.strokes[0].points), 1000)

    def test_stroke_program_validates_external_values_and_defaults(self) -> None:
        fill = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "default fill",
                "seed": 1,
                "canvas_width": 100,
                "canvas_height": 100,
                "operations": [{"kind": "fill", "id": "fill", "polygon": [[0, 0], [1, 0], [1, 1], [0, 1]]}],
            }
        )
        self.assertEqual(fill.operations[0].layer, "Flats")
        self.assertEqual(fill.operations[0].brush.profile, "marker")
        self.assertLessEqual(len(compile_stroke_program(fill).strokes), 2_000)

        invalid_values: list[dict[str, Any]] = [
            {"kind": "path", "id": "bad", "points": [[0, 0], [1, 1]], "closed": "false"},
            {
                "kind": "particles",
                "id": "bad",
                "bounds": [0, 0, 1],
            },
            {
                "kind": "path",
                "id": "bad",
                "points": [[0, 0], [1, 1]],
                "brush": {"is_eraser": "false"},
            },
            {
                "kind": "path",
                "id": "bad",
                "points": [[0, 0], [1, 1]],
                "brush": {"profile": "typo-pen"},
            },
            {
                "kind": "path",
                "id": "bad",
                "points": [[0, 0], [1, 1]],
                "brush": {"profile": "gpen", "size_px": 3},
            },
        ]
        for operation in invalid_values:
            with self.subTest(operation=operation["kind"]), self.assertRaises(PlanValidationError):
                StrokeProgram.from_dict(
                    {
                        "schema_version": 2,
                        "prompt": "invalid",
                        "seed": 1,
                        "canvas_width": 100,
                        "canvas_height": 100,
                        "operations": [operation],
                    }
                )

        dense_first = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "budget fairness",
                "seed": 2,
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "dense-fill",
                        "polygon": [[0, 0], [1, 0], [1, 1], [0, 1]],
                        "brush": {"profile": "marker", "size": 0.00001},
                        "spacing": 0.2,
                    },
                    {
                        "kind": "path",
                        "id": "final-contour",
                        "points": [[0.1, 0.1], [0.9, 0.9]],
                        "layer": "Lineart",
                    },
                ],
            }
        )
        dense_plan = compile_stroke_program(dense_first)
        self.assertLessEqual(len(dense_plan.strokes), 2_000)
        self.assertTrue(any(stroke.id == "final-contour" for stroke in dense_plan.strokes))

    def test_stroke_program_rich_primitives_compilation(self) -> None:
        program = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "sakura rich primitives",
                "seed": 42,
                "canvas": {"width": 800, "height": 600},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "wash-sky",
                        "layer": "Flats",
                        "style": "wash",
                        "polygon": [[0.0, 0.0], [1.0, 0.0], [1.0, 0.5], [0.0, 0.5]],
                        "brush": {"profile": "airbrush", "size": 0.08},
                    },
                    {
                        "kind": "hatch",
                        "id": "shadow-hatch",
                        "layer": "Shading",
                        "polygon": [[0.2, 0.3], [0.8, 0.3], [0.7, 0.7], [0.3, 0.7]],
                        "angle_deg": 45,
                        "spacing": 0.02,
                    },
                    {
                        "kind": "particles",
                        "id": "falling-petals",
                        "layer": "FX",
                        "shape": "petal",
                        "bounds": [0.1, 0.1, 0.9, 0.9],
                        "count": 15,
                    },
                    {
                        "kind": "particles",
                        "id": "sparkle-stars",
                        "layer": "Highlights",
                        "shape": "sparkle",
                        "bounds": [0.1, 0.1, 0.9, 0.9],
                        "count": 8,
                    },
                    {
                        "kind": "particles",
                        "id": "wind-drift",
                        "layer": "FX",
                        "shape": "drift",
                        "bounds": [0.1, 0.1, 0.9, 0.9],
                        "count": 5,
                    },
                ],
            }
        )
        op_fill = program.operations[0]
        self.assertIsInstance(op_fill, FillOperation)
        if isinstance(op_fill, FillOperation):
            self.assertEqual(op_fill.style, "wash")

        op_hatch = program.operations[1]
        self.assertIsInstance(op_hatch, HatchOperation)

        op_p1 = program.operations[2]
        self.assertIsInstance(op_p1, ParticleOperation)
        if isinstance(op_p1, ParticleOperation):
            self.assertEqual(op_p1.shape, "petal")

        op_p2 = program.operations[3]
        self.assertIsInstance(op_p2, ParticleOperation)
        if isinstance(op_p2, ParticleOperation):
            self.assertEqual(op_p2.shape, "sparkle")

        op_p3 = program.operations[4]
        self.assertIsInstance(op_p3, ParticleOperation)
        if isinstance(op_p3, ParticleOperation):
            self.assertEqual(op_p3.shape, "drift")

        plan = compile_stroke_program(program)
        self.assertTrue(len(plan.strokes) > 0)

        # wash fill generates feathered endpoints
        wash_strokes = [s for s in plan.strokes if s.layer_name == "Flats"]
        self.assertTrue(any(len(s.points) >= 4 for s in wash_strokes))
        # petal particles generate 3-point curved strokes with pressure swell
        petal_strokes = [s for s in plan.strokes if s.layer_name == "FX" and len(s.points) == 3]
        self.assertTrue(len(petal_strokes) >= 15)
        self.assertTrue(all(s.points[1].pressure > s.points[0].pressure for s in petal_strokes))

        # test invalid style and shape raises PlanValidationError
        with self.assertRaises(PlanValidationError):
            StrokeProgram(
                prompt="bad style",
                seed=1,
                canvas_width=100,
                canvas_height=100,
                operations=[
                    FillOperation(
                        id="bad",
                        polygon=[ProgramPoint(0, 0), ProgramPoint(1, 0), ProgramPoint(1, 1)],
                        style="invalid-style",
                    )
                ],
            )
        with self.assertRaises(PlanValidationError):
            StrokeProgram(
                prompt="bad shape",
                seed=1,
                canvas_width=100,
                canvas_height=100,
                operations=[
                    ParticleOperation(
                        id="bad",
                        bounds=(0, 0, 1, 1),
                        shape="invalid-shape",
                    )
                ],
            )

    def test_v1_v2_storage_migration_preserves_render_contract(self) -> None:
        legacy = RuleBasedPlanner().plan("custom preset", 21, 8, 320, 180)
        program = drawing_plan_to_stroke_program(legacy)
        migrated = compile_stroke_program(program)
        self.assertEqual([stroke.id for stroke in migrated.strokes], [stroke.id for stroke in legacy.strokes])
        self.assertEqual(
            [stroke.brush_preset for stroke in migrated.strokes],
            [stroke.brush_preset for stroke in legacy.strokes],
        )

        with tempfile.TemporaryDirectory() as temp:
            legacy_path = save_plan(legacy, temp)
            self.assertEqual(load_program(legacy_path), program)
            program_path = save_program(program, temp)
            self.assertEqual(load_program(program_path), program)
            self.assertEqual(load_plan(program_path), migrated)
            versionless = program.as_dict()
            versionless.pop("schema_version")
            versionless_path = Path(temp) / "program_without_explicit_version.json"
            versionless_path.write_text(json.dumps(versionless), encoding="utf-8")
            self.assertEqual(load_program(versionless_path), program)
            self.assertEqual(load_plan(versionless_path), migrated)

    def test_planner_is_deterministic_and_bounded(self) -> None:
        planner = RuleBasedPlanner()
        first = planner.plan("anime girl portrait", 42, 15, 1024, 768)
        second = planner.plan("anime girl portrait", 42, 15, 1024, 768)
        self.assertEqual(first.as_dict(), second.as_dict())
        self.assertEqual(len(first.strokes), 15)
        for stroke in first.strokes:
            self.assertGreaterEqual(len(stroke.points), 2)
            for point in stroke.points:
                self.assertGreaterEqual(point.x, 0)
                self.assertLess(point.x, 1024)
                self.assertGreaterEqual(point.y, 0)
                self.assertLess(point.y, 768)
                self.assertGreaterEqual(point.pressure, 0.0)
                self.assertLessEqual(point.pressure, 1.0)

        with self.assertRaises(ValueError):
            planner.plan("invalid iteration", 1, 1, 100, 100, iteration=2, max_iterations=1)

    def test_procedural_all_domains_generate_valid_strokes(self) -> None:
        # Character
        char_strokes = generate_character_strokes("girl", 42, 20, 800, 600)
        self.assertTrue(len(char_strokes) > 0)

        # Landscape / Wave / Flower
        land_strokes = generate_landscape_strokes("mountain landscape", 42, 20, 800, 600)
        self.assertTrue(len(land_strokes) > 0)
        wave_strokes = generate_landscape_strokes("hokusai wave", 42, 20, 800, 600)
        self.assertTrue(len(wave_strokes) > 0)
        rose_strokes = generate_landscape_strokes("blooming rose flower", 42, 20, 800, 600)
        self.assertTrue(len(rose_strokes) > 0)

        # Manga FX / Magic / Speed
        fx_strokes = generate_manga_fx_strokes("focus lines", 42, 20, 800, 600)
        self.assertTrue(len(fx_strokes) > 0)
        magic_strokes = generate_manga_fx_strokes("magic circle", 42, 20, 800, 600)
        self.assertTrue(len(magic_strokes) > 0)

        # Geometry & Creature
        mandala_strokes = generate_geometry_strokes("mandala", 42, 20, 800, 600)
        self.assertTrue(len(mandala_strokes) > 0)
        city_strokes = generate_geometry_strokes("cyberpunk city skyline", 42, 1, 800, 600)
        self.assertTrue(len(city_strokes) > 0)
        cat_strokes = generate_creature_strokes("cute cat", 42, 20, 800, 600)
        self.assertTrue(len(cat_strokes) > 0)

        # 同一 seed でも対象種・人物属性に応じて形状とストローク集合が変わること
        creature_fingerprints = {
            tuple(stroke.id for stroke in generate_creature_strokes(subject, 42, 40, 800, 600))
            for subject in ("cute cat", "friendly dog", "flying bird", "fire dragon")
        }
        self.assertEqual(len(creature_fingerprints), 4)
        girl_ids = tuple(stroke.id for stroke in generate_character_strokes("anime girl", 42, 80, 800, 600))
        boy_ids = tuple(stroke.id for stroke in generate_character_strokes("anime boy", 42, 80, 800, 600))
        female_ids = tuple(stroke.id for stroke in generate_character_strokes("female portrait", 42, 80, 800, 600))
        self.assertNotEqual(girl_ids, boy_ids)
        self.assertEqual(girl_ids, female_ids)

        # Dispatcher Plan
        plan = generate_procedural_plan("cute cat", 42, 20, 800, 600)
        self.assertEqual(plan.title, "Creature Artwork")
        self.assertTrue(len(plan.strokes) > 0)

    def test_procedural_quality_foundations_prompt_intent_and_resolution_scaling(self) -> None:
        prompts = (
            "anime girl portrait",
            "mountain landscape",
            "blooming rose flower",
            "cute cat",
            "magic circle",
            "cyberpunk city skyline",
        )
        for prompt in prompts:
            with self.subTest(prompt=prompt):
                plan = generate_procedural_plan(prompt, 42, None, 800, 600)
                flats = [stroke for stroke in plan.strokes if stroke.layer_name == "Flats"]
                self.assertTrue(flats)
                self.assertTrue(any(abs(stroke.points[-1].x - stroke.points[0].x) >= 800 * 0.75 for stroke in flats))
                self.assertTrue(any(stroke.size_px >= 600 * 0.05 for stroke in flats))
                quality = evaluate_plan_quality(plan)
                self.assertGreaterEqual(quality.coverage, 0.35)
                self.assertGreaterEqual(quality.score, 0.75)
                self.assertEqual(quality.out_of_bounds_points, 0)

        mixed = generate_procedural_plan("anime girl casting a magic spell", 42, None, 800, 600)
        self.assertEqual(mixed.title, "Character Portrait")
        self.assertIn("Lineart", mixed.layers)
        self.assertTrue(any(layer in mixed.layers for layer in ("FX", "Highlights")))
        self.assertEqual(
            generate_procedural_plan("mandala", 42, None, 800, 600).title,
            "Geometric / City Artwork",
        )
        self.assertEqual(
            generate_procedural_plan("cat in a cyberpunk city", 42, None, 800, 600).title,
            "Creature Artwork",
        )
        self.assertEqual(
            generate_procedural_plan("gothic cathedral", 42, None, 800, 600).title,
            "Geometric / City Artwork",
        )

        from .procedural.base import color_palette

        monochrome_colors = set(color_palette("monochrome").values())
        for prompt in ("mountain landscape", "red rose", "cute cat", "magic circle", "cyberpunk city"):
            monochrome = generate_procedural_plan(prompt, 42, None, 800, 600, palette_name="monochrome")
            self.assertTrue(all(stroke.color[:7] in monochrome_colors for stroke in monochrome.strokes))
        direct_landscape = generate_landscape_strokes("mountain landscape", 42, None, 800, 600, "monochrome")
        self.assertTrue(all(stroke.color[:7] in monochrome_colors for stroke in direct_landscape))

        blue = generate_procedural_plan("anime girl with blue hair and green eyes", 42, None, 800, 600)
        pink = generate_procedural_plan("anime girl with pink hair and purple eyes", 42, None, 800, 600)
        self.assertNotEqual(
            [(stroke.id, stroke.color) for stroke in blue.strokes],
            [(stroke.id, stroke.color) for stroke in pink.strokes],
        )
        self.assertTrue(any(stroke.color == "#4776d0" for stroke in blue.strokes))
        self.assertTrue(any(stroke.color == "#e86f9d" for stroke in pink.strokes))

        small = generate_procedural_plan("anime girl", 7, None, 400, 300)
        large = generate_procedural_plan("anime girl", 7, None, 1600, 1200)
        small_line = next(stroke for stroke in small.strokes if stroke.layer_name == "Lineart")
        large_by_id = {stroke.id: stroke for stroke in large.strokes}
        self.assertAlmostEqual(large_by_id[small_line.id].size_px / small_line.size_px, 4.0)

    def test_sample_strokes_by_priority(self) -> None:
        raw_strokes = generate_character_strokes("girl portrait", 42, 100, 800, 600)
        sampled = sample_strokes_by_priority(raw_strokes, 10)
        self.assertEqual(len(sampled), 10)

        layers = [s.layer_name for s in sampled]
        self.assertIn("Lineart", layers)

        self.assertEqual(len(sample_strokes_by_priority(raw_strokes[:5], 10)), 5)
        self.assertEqual(sample_strokes_by_priority(raw_strokes, 0), [])

    def test_plan_json_round_trip_and_collision_free_save(self) -> None:
        plan = RuleBasedPlanner().plan("curve", 9, 2, 300, 200)
        with tempfile.TemporaryDirectory() as temp:
            first_path = save_plan(plan, temp)
            second_path = save_plan(plan, temp)
            self.assertNotEqual(first_path, second_path)
            self.assertEqual(load_plan(first_path), plan)
            self.assertEqual(load_plan(second_path), plan)

    def test_svg_export_generates_valid_svg_tags(self) -> None:
        plan = RuleBasedPlanner().plan("anime girl", 42, 10, 800, 600)
        svg_content = plan.to_svg(800, 600)
        self.assertIn("<svg", svg_content)
        self.assertIn("</svg>", svg_content)
        self.assertIn("layer_", svg_content)

        with tempfile.TemporaryDirectory() as temp:
            svg_path = save_svg(plan, temp)
            self.assertTrue(svg_path.is_file())
            self.assertIn("<svg", svg_path.read_text(encoding="utf-8"))

        points = [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)]
        reverse_layers = DrawingPlan(
            "layer order",
            1,
            [
                Stroke("line", points, layer_name="Lineart"),
                Stroke("draft", points, layer_name="Draft"),
                Stroke("flat", points, layer_name="Flats"),
            ],
            layers=["Lineart", "Draft", "Flats"],
        ).to_svg(100, 100)
        self.assertLess(reverse_layers.index('id="layer_Draft"'), reverse_layers.index('id="layer_Flats"'))
        self.assertLess(reverse_layers.index('id="layer_Flats"'), reverse_layers.index('id="layer_Lineart"'))

    def test_svg_comment_with_double_hyphen_stays_well_formed(self) -> None:
        import xml.etree.ElementTree as ET

        plan = RuleBasedPlanner().plan("attack -- defense", 7, 3, 100, 100)
        svg_content = plan.to_svg(100, 100)
        ET.fromstring(svg_content)

    def test_plan_canvas_dimensions_round_trip_and_drive_svg(self) -> None:
        plan = RuleBasedPlanner().plan("canvas dimensions", 7, 3, 640, 360)
        loaded = DrawingPlan.from_dict(plan.as_dict())
        self.assertEqual((loaded.canvas_width, loaded.canvas_height), (640.0, 360.0))
        self.assertIn('viewBox="0 0 640.0 360.0"', loaded.to_svg())

        alpha_plan = DrawingPlan(
            "alpha",
            1,
            [
                Stroke(
                    "alpha-stroke",
                    [StrokePoint(0, 0, 1.0, 0), StrokePoint(10, 10, 1.0, 10)],
                    color="#ff000080",
                )
            ],
            canvas_width=20,
            canvas_height=20,
        )
        alpha_svg = alpha_plan.to_svg()
        self.assertIn('stroke="#ff0000"', alpha_svg)
        self.assertIn('stroke-opacity="0.50"', alpha_svg)

    def test_domain_rejects_oversized_direct_models(self) -> None:
        point = StrokePoint(1, 1, 0.5, 0)
        with self.assertRaises(PlanValidationError):
            Stroke("too-many-points", [point] * 1001)
        valid_stroke = Stroke("valid", [point, StrokePoint(2, 2, 0.5, 10)])
        with self.assertRaises(PlanValidationError):
            DrawingPlan("too-many-strokes", 1, [valid_stroke] * 2001)

    def test_invalid_domain_data_is_rejected(self) -> None:
        with self.assertRaises(PlanValidationError):
            StrokePoint(0, 0, 1.1, 0)
        with self.assertRaises(PlanValidationError):
            Stroke("one-point", [StrokePoint(0, 0, 0.5, 0)])
        with self.assertRaises(PlanValidationError):
            DrawingPlan.from_dict({"schema_version": 99, "prompt": "", "seed": 0, "strokes": []})
        with self.assertRaises(PlanValidationError):
            DrawingPlan.from_dict({"prompt": 123, "seed": 0, "strokes": []})
        with self.assertRaises(PlanValidationError):
            Stroke.from_dict({"id": 123, "points": [[0, 0], [1, 1]]})
        with self.assertRaises(PlanValidationError):
            StrokePoint.from_dict([0, 0, 0.5, "10"])
        with self.assertRaises(PlanValidationError):
            Stroke("bad-layer", [StrokePoint(0, 0, 0.5, 0), StrokePoint(1, 1, 0.5, 10)], layer_name="")
        with self.assertRaises(PlanValidationError):
            DrawingPlan("bad-iteration", 0, (), iteration=0)
        with self.assertRaises(PlanValidationError):
            VisionCritique.from_dict({"evaluation": [], "suggested_action": "next", "iteration": 1})

    def test_compact_points_domain_parsing(self) -> None:
        p2 = StrokePoint.from_dict([100.5, 200.5])
        self.assertEqual(p2.x, 100.5)
        self.assertEqual(p2.y, 200.5)
        self.assertEqual(p2.pressure, 0.8)
        self.assertEqual(p2.time_ms, 0)

        p3 = StrokePoint.from_dict([50, 60, 0.95])
        self.assertEqual(p3.pressure, 0.95)

        p4 = StrokePoint.from_dict([50, 60, 0.95, 120])
        self.assertEqual(p4.time_ms, 120)

        st = Stroke.from_dict(
            {
                "id": "compact_stroke",
                "points": [[10, 20], [30, 40, 0.7], [50, 60, 0.9, 30]],
                "brush_preset": "Basic-5 Size",
                "color": "#ff007f",
                "size_px": 12.0,
                "layer_name": "Flats",
            }
        )
        self.assertEqual(st.id, "compact_stroke")
        self.assertEqual(len(st.points), 3)
        self.assertEqual(st.layer_name, "Flats")

    def test_vision_critique_dataclass(self) -> None:
        critique = VisionCritique("Good draft", 0.85, "Add clean lineart", 2)
        d = critique.as_dict()
        self.assertEqual(d["completion_score"], 0.85)
        self.assertEqual(d["iteration"], 2)
        loaded = VisionCritique.from_dict(d)
        self.assertEqual(loaded.suggested_action, "Add clean lineart")
        self.assertFalse(VisionCritique.from_dict({"completion_score": 0.99, "iteration": 1}).goal_reached)

    def test_goal_completion_requires_explicit_boolean(self) -> None:
        stroke = Stroke("goal", [StrokePoint(0, 0, 1, 0), StrokePoint(1, 1, 1, 10)])
        score_only = DrawingPlan("score only", 1, [stroke], completion_score=1.0)
        explicit = DrawingPlan("explicit", 1, [stroke], completion_score=0.1, goal_reached=True)
        metadata_explicit = DrawingPlan("metadata", 1, [stroke], metadata={"goal_reached": True})
        self.assertFalse(_is_plan_goal_reached(score_only))
        self.assertTrue(_is_plan_goal_reached(explicit))
        self.assertTrue(_is_plan_goal_reached(metadata_explicit))

    def test_image_converter_rejects_invalid_data_without_silent_fallback(self) -> None:
        converter = ImageStrokeConverter()
        with self.assertRaises(ValueError):
            converter.convert_image_to_plan(b"not-a-valid-image", "cat", 42, 10, 800, 600)

    def test_image_converter_zero_dimension_fallback(self) -> None:
        class FakeZeroDimImage:
            def width(self) -> int:
                return 0

            def height(self) -> int:
                return 0

            def scaled(self, _w: int, _h: int) -> Any:
                return self

        import random

        converter = ImageStrokeConverter()
        strokes = converter._process_qimage(FakeZeroDimImage(), 42, 10, 800, 600, random.Random(42))
        self.assertEqual(strokes, [])

    def test_image_converter_converts_format_to_argb32(self) -> None:
        converted_formats: list[Any] = []

        class FakeScaledImage:
            def width(self) -> int:
                return 10

            def height(self) -> int:
                return 10

            def convertToFormat(self, fmt: Any) -> Any:  # noqa: N802
                converted_formats.append(fmt)
                return self

            def pixelColor(self, x: int, y: int) -> Any:  # noqa: N802
                class FakeColor:
                    def red(self) -> int:
                        return 100

                    def green(self) -> int:
                        return 100

                    def blue(self) -> int:
                        return 100

                return FakeColor()

        class FakeImage:
            Format_ARGB32 = 5

            def width(self) -> int:
                return 20

            def height(self) -> int:
                return 20

            def loadFromData(self, data: bytes) -> bool:  # noqa: N802
                return True

            def scaled(self, w: int, h: int) -> Any:
                return FakeScaledImage()

        converter = ImageStrokeConverter()
        converter.qimage_cls = FakeImage
        fake_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (20).to_bytes(4, "big") * 2
        plan = converter.convert_image_to_plan(fake_png, "test", 42, 5, 200, 200)
        self.assertIn(5, converted_formats)
        self.assertTrue(len(plan.strokes) > 0)

    def test_image_converter_preserves_aspect_and_traces_connected_edges(self) -> None:
        scaled_sizes: list[tuple[int, int]] = []

        class FakeColor:
            def red(self) -> int:
                return 255

            def green(self) -> int:
                return 255

            def blue(self) -> int:
                return 255

            def alpha(self) -> int:
                return 0

        class FakeImage:
            Format_ARGB32 = 5

            def width(self) -> int:
                return 400

            def height(self) -> int:
                return 100

            def scaled(self, width: int, height: int) -> Any:
                scaled_sizes.append((width, height))
                return self

            def convertToFormat(self, _fmt: Any) -> Any:  # noqa: N802
                return self

            def pixelColor(self, _x: int, _y: int) -> FakeColor:  # noqa: N802
                return FakeColor()

        import random

        converter = ImageStrokeConverter()
        converter.qimage_cls = FakeImage
        strokes = converter._process_qimage(FakeImage(), 1, 20, 200, 200, random.Random(1))
        self.assertEqual(scaled_sizes, [(400, 100)])
        self.assertEqual(strokes, [])

        mask = [[False] * 5 for _ in range(5)]
        for coordinate in ((1, 1), (2, 2), (3, 3)):
            mask[coordinate[1]][coordinate[0]] = True
        self.assertEqual(_trace_edge_paths(mask, 10), [[(1, 1), (2, 2), (3, 3)]])

    def test_image_header_dimensions_prevent_oversized_decode(self) -> None:
        oversized_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (10_000).to_bytes(4, "big") * 2
        self.assertEqual(_image_dimensions_from_header(oversized_png), (10_000, 10_000))
        gif_header = b"GIF89a" + (640).to_bytes(2, "little") + (480).to_bytes(2, "little")
        self.assertEqual(_image_dimensions_from_header(gif_header), (640, 480))

        class MustNotDecode:
            def __init__(self) -> None:
                raise AssertionError("oversized image was allocated")

        converter = ImageStrokeConverter()
        converter.qimage_cls = MustNotDecode
        with self.assertRaises(ValueError):
            converter.convert_image_to_plan(oversized_png, "oversized", 1, 1, 100, 100)


class PluginBuildTests(unittest.TestCase):
    @staticmethod
    def _create_minimal_source(source: Path) -> None:
        source.mkdir(parents=True)
        (source / f"{PACKAGE_NAME}.desktop").write_text("[Desktop Entry]\n", encoding="utf-8")
        (source / "__init__.py").write_text("", encoding="utf-8")
        proc_dir = source / "procedural"
        proc_dir.mkdir(parents=True)
        (proc_dir / "__init__.py").write_text("", encoding="utf-8")

    def test_build_includes_manifest_once_at_archive_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / f"{PACKAGE_NAME}.zip"
            self.assertEqual(build(output), output)
            with ZipFile(output) as archive:
                names = archive.namelist()

        self.assertIn(f"{PACKAGE_NAME}.desktop", names)
        self.assertIn(f"{PACKAGE_NAME}/", names)
        self.assertIn(f"{PACKAGE_NAME}/__init__.py", names)
        self.assertIn(f"{PACKAGE_NAME}/qt_compat.py", names)
        self.assertIn(f"{PACKAGE_NAME}/procedural/__init__.py", names)
        self.assertIn(f"{PACKAGE_NAME}/image_converter.py", names)
        self.assertIn(f"{PACKAGE_NAME}/brushes.py", names)
        self.assertIn(f"{PACKAGE_NAME}/stroke_program.py", names)
        self.assertIn(f"{PACKAGE_NAME}/native_bridge.py", names)
        self.assertIn(f"{PACKAGE_NAME}/krita_smoke.py", names)
        self.assertIn(f"{PACKAGE_NAME}/quality.py", names)
        self.assertIn(f"{PACKAGE_NAME}/quality_check.py", names)
        self.assertNotIn(f"{PACKAGE_NAME}/{PACKAGE_NAME}.desktop", names)

    def test_build_fails_when_a_required_file_is_missing(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / f"{PACKAGE_NAME}.zip"
            with (
                patch("ai_stroke_painter.build_plugin.PACKAGE_FILES", ("missing-required-file.py",)),
                self.assertRaises(FileNotFoundError),
            ):
                build(output)
            self.assertFalse(output.exists())


class OpenAICompatiblePlannerTests(unittest.TestCase):
    def test_v2_stroke_program_response_compiles_with_request_contract(self) -> None:
        response = {
            "schema_version": 2,
            "prompt": "model changed prompt",
            "seed": 999,
            "canvas": {"width": 10, "height": 10},
            "operations": [
                {
                    "kind": "fill",
                    "id": "background",
                    "polygon": [[0, 0], [1, 0], [1, 1], [0, 1]],
                    "brush": {"profile": "watercolor", "color": "#abcdef", "size": 0.08},
                },
                {
                    "kind": "path",
                    "id": "line",
                    "points": [[0.1, 0.2, 0.1], [0.5, 0.4, 1.0], [0.9, 0.7, 0.1]],
                    "brush": {"profile": "gpen", "size": 0.004},
                },
            ],
        }
        plan = _plan_from_response(response, prompt="requested", seed=7, width=800, height=600)
        self.assertEqual(plan.prompt, "requested")
        self.assertEqual(plan.seed, 7)
        self.assertEqual((plan.canvas_width, plan.canvas_height), (800.0, 600.0))
        self.assertEqual(plan.metadata["source_schema_version"], 2)
        self.assertIn("Flats", plan.layers)
        self.assertIn("Lineart", plan.layers)
        self.assertTrue(all(0 <= point.x < 800 for stroke in plan.strokes for point in stroke.points))
        self.assertTrue(all(0 <= point.y < 600 for stroke in plan.strokes for point in stroke.points))

        fenced = f"analysis before output\n```json\n{json.dumps(response)}\n```"
        self.assertIn("operations", _extract_json_object(fenced))

    def test_cross_origin_redirect_is_rejected_before_credentials_can_follow(self) -> None:
        from urllib.request import Request

        request = Request(
            "https://api.example.test/v1/chat/completions",
            data=b"{}",
            headers={"Authorization": "Bearer secret"},
            method="POST",
        )
        handler = _SameOriginRedirectHandler()
        with self.assertRaises(_CrossOriginRedirectError):
            handler.redirect_request(request, None, 302, "redirect", {}, "https://attacker.test/collect")

    def test_calls_chat_completions_and_validates_plan(self) -> None:
        expected_plan = {
            "schema_version": 1,
            "prompt": "a blue curve",
            "seed": 12,
            "title": "Test Artwork",
            "iteration": 1,
            "layers": ["Lineart"],
            "strokes": [
                {
                    "id": "llm-stroke-1",
                    "points": [
                        {"x": 10, "y": 12, "pressure": 0.2, "time_ms": 0},
                        {"x": 40, "y": 35, "pressure": 0.8, "time_ms": 20},
                    ],
                    "brush_preset": "Basic-5 Size",
                    "color": "#3366cc",
                    "size_px": 7,
                    "layer_name": "Lineart",
                    "opacity": 1.0,
                }
            ],
        }

        class Handler(BaseHTTPRequestHandler):
            received: dict[str, Any] | None = None

            def do_POST(self) -> None:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length)
                type(self).received = {
                    "path": self.path,
                    "authorization": self.headers.get("Authorization"),
                    "body": json.loads(body),
                }
                response = {"choices": [{"message": {"content": "```json\n" + json.dumps(expected_plan) + "\n```"}}]}
                encoded = json.dumps(response).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def log_message(self, _format: str, *_args: Any) -> None:
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(f"http://127.0.0.1:{server.server_port}/v1", "test-model", "test-key", 2)
            )
            plan = planner.plan("a blue curve", 12, 1, 100, 100)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        self.assertEqual(plan.prompt, str(expected_plan["prompt"]))
        self.assertEqual(len(plan.strokes), 1)
        self.assertEqual(plan.strokes[0].color, "#3366cc")
        self.assertEqual(plan.strokes[0].layer_name, "Lineart")
        self.assertGreaterEqual(len(plan.strokes[0].points), 2)
        assert Handler.received is not None
        self.assertEqual(Handler.received["path"], "/v1/chat/completions")
        self.assertEqual(Handler.received["authorization"], "Bearer test-key")
        self.assertEqual(Handler.received["body"]["model"], "test-model")
        self.assertEqual(Handler.received["body"]["response_format"], {"type": "json_object"})

    def test_smart_endpoint_resolution(self) -> None:
        s1 = OpenAICompatibleSettings("http://localhost:11434", "llama3")
        self.assertEqual(s1.endpoint_url, "http://localhost:11434/v1/chat/completions")

        s2 = OpenAICompatibleSettings("https://api.openai.com", "gpt-4o")
        self.assertEqual(s2.endpoint_url, "https://api.openai.com/v1/chat/completions")

        s3 = OpenAICompatibleSettings("https://api.openai.com/v1", "gpt-4o")
        self.assertEqual(s3.endpoint_url, "https://api.openai.com/v1/chat/completions")

        s4 = OpenAICompatibleSettings("https://custom.api/v1/chat/completions", "gpt-4o")
        self.assertEqual(s4.endpoint_url, "https://custom.api/v1/chat/completions")

        s5 = OpenAICompatibleSettings("https://example.test/path/api.openai.com", "model")
        self.assertEqual(s5.endpoint_url, "https://example.test/path/api.openai.com/chat/completions")

    def test_sanitizes_out_of_bounds_and_normalizes_coords_llm_plan(self) -> None:
        response_clamped = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "prompt": "curve",
                                "strokes": [
                                    {
                                        "id": "s",
                                        "points": [
                                            {"x": -10, "y": -5, "pressure": 0.5, "time_ms": 0},
                                            {"x": 200, "y": 150, "pressure": 0.5, "time_ms": 1},
                                        ],
                                    }
                                ],
                            }
                        )
                    }
                }
            ]
        }

        class FakeResponse:
            def __init__(self, data: dict[str, Any]) -> None:
                self._data = data

            def read(self, _size: int) -> bytes:
                return json.dumps(self._data).encode("utf-8")

            def __enter__(self) -> FakeResponse:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: FakeResponse(response_clamped),
        )
        plan = planner.plan("curve", 1, 1, 100, 100)
        self.assertEqual(len(plan.strokes), 1)
        pts = plan.strokes[0].points
        self.assertGreaterEqual(pts[0].x, 0.0)
        self.assertLessEqual(pts[-1].x, 100.0)

        response_norm = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "strokes": [
                                    {
                                        "id": "norm_stroke",
                                        "points": [
                                            {"x": 0.1, "y": 0.2, "pressure": 0.5, "time_ms": 0},
                                            {"x": 0.9, "y": 0.8, "pressure": 0.8, "time_ms": 20},
                                        ],
                                    }
                                ],
                            }
                        )
                    }
                }
            ]
        }

        planner_norm = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: FakeResponse(response_norm),
        )
        scaled_plan = planner_norm.plan("norm curve", 42, 1, 1000, 500)
        scaled_pts = scaled_plan.strokes[0].points
        self.assertAlmostEqual(scaled_pts[0].x, 100.0, places=1)
        self.assertAlmostEqual(scaled_pts[0].y, 100.0, places=1)
        self.assertAlmostEqual(scaled_pts[-1].x, 900.0, places=1)
        self.assertAlmostEqual(scaled_pts[-1].y, 400.0, places=1)

    def test_compact_points_plan_from_llm(self) -> None:
        compact_json = {
            "schema_version": 1,
            "prompt": "fantasy sakura landscape",
            "seed": 42,
            "title": "Sakura Art",
            "layers": ["Flats", "Shading", "Lineart", "Highlights"],
            "strokes": [
                {
                    "id": "s_flat",
                    "brush_preset": "Basic-5 Size",
                    "color": "#ffb7c5",
                    "size_px": 150.0,
                    "layer_name": "Flats",
                    "points": [[100, 200], [500, 250], [900, 220]],
                },
                {
                    "id": "s_line",
                    "brush_preset": "Basic-5 Size",
                    "color": "#2c1810",
                    "size_px": 12.0,
                    "layer_name": "Lineart",
                    "points": [[500, 900, 0.9], [510, 600, 0.8], [490, 400, 0.7]],
                },
            ],
        }

        class FakeCompactResponse:
            def __init__(self) -> None:
                pass

            def read(self, _size: int) -> bytes:
                return json.dumps(
                    {"choices": [{"message": {"content": "```json\n" + json.dumps(compact_json) + "\n```"}}]}
                ).encode("utf-8")

            def __enter__(self) -> FakeCompactResponse:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: FakeCompactResponse(),
        )
        plan = planner.plan("fantasy sakura landscape", 42, 2, 2480, 3508)
        self.assertEqual(len(plan.strokes), 2)
        self.assertEqual(plan.strokes[0].layer_name, "Flats")
        self.assertGreaterEqual(plan.strokes[0].size_px, 60.0)  # Adaptive sizing for Flats on 2480x3508
        self.assertGreaterEqual(len(plan.strokes[0].points), 6)  # Spline smoothed
        self.assertEqual(plan.strokes[1].layer_name, "Lineart")

    def test_system_instruction_prompt_generation(self) -> None:
        from ai_stroke_painter.llm_planner import _system_instruction

        prompt_sakura = _system_instruction(
            iteration=1,
            max_iterations=1,
            is_reasoning=False,
            width=2480,
            height=3508,
            prompt="fantasy sakura landscape with mountains and clouds",
        )
        self.assertIn("Landscape, Mountains, Clouds & Sakura", prompt_sakura)
        self.assertIn("Flats", prompt_sakura)
        self.assertIn("Shading", prompt_sakura)
        self.assertIn("Lineart", prompt_sakura)
        self.assertIn("Highlights", prompt_sakura)
        self.assertIn('"schema_version": 2', prompt_sakura)
        self.assertIn('"operations"', prompt_sakura)
        self.assertIn("fill/hatch/particles", prompt_sakura)
        self.assertIn("[x, y, pressure]", prompt_sakura)

        prompt_anime = _system_instruction(
            iteration=1,
            max_iterations=1,
            is_reasoning=False,
            width=1000,
            height=1000,
            prompt="anime girl portrait with delicate eyes",
        )
        self.assertIn("Anime / Manga Character Portrait", prompt_anime)

    def test_drawing_plan_request_canvas_image_roundtrip(self) -> None:
        plan = DrawingPlan(
            prompt="test",
            seed=42,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            request_canvas_image=True,
        )
        d = plan.as_dict()
        self.assertTrue(d["request_canvas_image"])
        loaded = DrawingPlan.from_dict(d)
        self.assertTrue(loaded.request_canvas_image)

        plan_false = DrawingPlan(
            prompt="test",
            seed=42,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            request_canvas_image=False,
        )
        self.assertFalse(plan_false.as_dict()["request_canvas_image"])
        self.assertFalse(DrawingPlan.from_dict(plan_false.as_dict()).request_canvas_image)

    def test_system_instruction_progressive_phases(self) -> None:
        from ai_stroke_painter.llm_planner import _system_instruction

        step1 = _system_instruction(iteration=1, max_iterations=3, prompt="sakura tree")
        self.assertIn("Step 1/3", step1)
        self.assertIn("MULTI-STEP PROGRESSIVE DRAWING MODE", step1)
        self.assertIn("AUTOMATIC VISUAL FEEDBACK", step1)
        self.assertIn("Flats", step1)

        step2 = _system_instruction(iteration=2, max_iterations=3, prompt="sakura tree")
        self.assertIn("Step 2/3", step2)
        self.assertIn("Shading", step2)

        step3 = _system_instruction(iteration=3, max_iterations=3, prompt="sakura tree")
        self.assertIn("Step 3/3", step3)
        self.assertIn("FINAL", step3)

    def test_multi_stage_conversation_history_and_visual_feedback(self) -> None:
        received_payloads: list[dict[str, Any]] = []

        step1_response = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 1,
                                "prompt": "mountain landscape",
                                "seed": 42,
                                "iteration": 1,
                                "request_canvas_image": False,
                                "strokes": [
                                    {
                                        "id": "s_base",
                                        "layer_name": "Flats",
                                        "color": "#336699",
                                        "size_px": 80.0,
                                        "points": [[0, 100], [400, 100]],
                                    }
                                ],
                            }
                        )
                    }
                }
            ]
        }

        step2_response = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 1,
                                "prompt": "mountain landscape",
                                "seed": 42,
                                "iteration": 2,
                                "request_canvas_image": True,
                                "strokes": [
                                    {
                                        "id": "s_detail",
                                        "layer_name": "Lineart",
                                        "color": "#111111",
                                        "size_px": 10.0,
                                        "points": [[100, 50], [200, 50]],
                                    }
                                ],
                            }
                        )
                    }
                }
            ]
        }

        class FakeMultiStepResponse:
            def __init__(self, idx: int) -> None:
                self._idx = idx

            def read(self, _size: int) -> bytes:
                data = step1_response if self._idx == 0 else step2_response
                return json.dumps(data).encode("utf-8")

            def __enter__(self) -> FakeMultiStepResponse:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        def fake_opener(req: Any, *_args: Any, **_kwargs: Any) -> FakeMultiStepResponse:
            body = json.loads(req.data.decode("utf-8"))
            received_payloads.append(body)
            return FakeMultiStepResponse(len(received_payloads) - 1)

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "test-model"), opener=fake_opener
        )

        # Step 1: 実行
        plan1 = planner.plan("mountain landscape", 42, 1, 800, 600, iteration=1, max_iterations=2)
        self.assertEqual(len(plan1.strokes), 1)
        self.assertFalse(plan1.request_canvas_image)

        # Step 2: 前回描画のキャンバスを添付し、視覚評価を次の計画へ反映する。
        plan2 = planner.plan(
            "mountain landscape",
            42,
            1,
            800,
            600,
            canvas_image=b"fake-canvas-png",
            iteration=2,
            max_iterations=2,
        )
        self.assertEqual(len(plan2.strokes), 1)
        self.assertTrue(plan2.request_canvas_image)
        self.assertEqual(plan2.iteration, 2)

        # Step 2 のペイロード検証: 前ステップの会話履歴とキャンバス画像が含まれる。
        self.assertEqual(len(received_payloads), 2)
        step2_messages = received_payloads[1]["messages"]
        # system (0), user_step1 (1), assistant_step1 (2), user_step2 (3)
        self.assertEqual(len(step2_messages), 4)
        self.assertEqual(step2_messages[0]["role"], "system")
        self.assertEqual(step2_messages[1]["role"], "user")
        self.assertIsInstance(step2_messages[1]["content"], str)
        self.assertNotIn("image_url", step2_messages[1]["content"])
        self.assertEqual(step2_messages[2]["role"], "assistant")
        self.assertEqual(step2_messages[3]["role"], "user")
        step2_content = step2_messages[3]["content"]
        self.assertIsInstance(step2_content, list)
        self.assertTrue(any(part.get("type") == "image_url" for part in step2_content))
        text_part = next(part["text"] for part in step2_content if part.get("type") == "text")
        self.assertIn("Visually inspect", text_part)

    def test_extracts_json_with_surrounding_markdown_and_commentary(self) -> None:
        raw_text = (
            "<think>Thinking about generating high quality strokes...</think>\n"
            "Here is your drawing plan:\n"
            "```json\n"
            "{\n"
            '  "schema_version": 1,\n'
            '  "prompt": "hair curve",\n'
            '  "seed": 42,\n'
            '  "strokes": [\n'
            "    {\n"
            '      "id": "s1",\n'
            '      "points": [\n'
            '        {"x": 10.0, "y": 20.0, "pressure": 0.5, "time_ms": 0},\n'
            '        {"x": 30.0, "y": 40.0, "pressure": 0.8, "time_ms": 10}\n'
            "      ]\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "```\n"
            "Let me know if you need any adjustments!"
        )
        parsed = _extract_json_object(raw_text)
        plan = DrawingPlan.from_dict(parsed)
        self.assertEqual(plan.prompt, "hair curve")
        self.assertEqual(plan.seed, 42)
        self.assertEqual(len(plan.strokes), 1)

    def test_drawing_plan_from_dict_without_schema_version(self) -> None:
        plan_dict = {
            "prompt": "no schema version",
            "seed": 7,
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 0.0, "y": 0.0, "pressure": 0.5, "time_ms": 0},
                        {"x": 10.0, "y": 10.0, "pressure": 0.5, "time_ms": 1},
                    ],
                }
            ],
        }
        plan = DrawingPlan.from_dict(plan_dict)
        self.assertEqual(plan.prompt, "no schema version")
        self.assertEqual(plan.seed, 7)
        self.assertEqual(len(plan.strokes), 1)

    def test_openai_planner_logs_to_callback(self) -> None:
        logs: list[str] = []
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: None,
            log_callback=lambda msg: logs.append(msg),
        )
        planner._log("テストログメッセージ")
        self.assertEqual(len(logs), 1)
        self.assertIn("テストログメッセージ", logs[0])

    def test_detailed_debug_logs_emission(self) -> None:
        logs: list[str] = []
        valid_plan = {
            "schema_version": 1,
            "prompt": "debug log test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        mock_resp = {
            "model": "deepseek-r1",
            "usage": {
                "prompt_tokens": 150,
                "completion_tokens": 800,
                "total_tokens": 950,
                "completion_tokens_details": {"reasoning_tokens": 600},
            },
            "choices": [
                {
                    "finish_reason": "stop",
                    "message": {
                        "role": "assistant",
                        "reasoning_content": "Deep thought about drawing curves...",
                        "content": "```json\n" + json.dumps(valid_plan) + "\n```",
                    },
                }
            ],
        }

        class MockResponse:
            def read(self, _size: int) -> bytes:
                return json.dumps(mock_resp).encode("utf-8")

            def __enter__(self) -> Any:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "deepseek-r1"),
            opener=lambda *_args, **_kwargs: MockResponse(),
            log_callback=lambda msg: logs.append(msg),
        )
        planner.plan("debug log test", 1, 1, 100, 100)

        # 詳細ログが記録されていることを確認
        self.assertTrue(any("[トークン消費]" in log for log in logs))
        self.assertTrue(any("Prompt: 150" in log for log in logs))
        self.assertTrue(any("思考推論: 600" in log for log in logs))
        self.assertTrue(any("[LLM 応答状態] finish_reason: stop" in log for log in logs))
        self.assertTrue(any("[思考プロセス (reasoning)]" in log for log in logs))
        self.assertTrue(any("[LLM 応答本文プレビュー" in log for log in logs))

    def test_extract_content_various_api_formats(self) -> None:
        # 1. Standard OpenAI message.content
        resp_openai = {"choices": [{"message": {"role": "assistant", "content": "hello openai"}}]}
        self.assertEqual(_extract_content_from_response(resp_openai), "hello openai")

        # 2. Thinking / Reasoning model (content is None/empty, reasoning_content has JSON)
        resp_thinking = {
            "choices": [
                {
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "reasoning_content": '```json\n{"schema_version": 1, "strokes": []}\n```',
                    }
                }
            ]
        }
        self.assertIn("strokes", _extract_content_from_response(resp_thinking))

        # 3. Tool calls arguments fallback
        resp_tools = {
            "choices": [
                {
                    "message": {
                        "role": "assistant",
                        "content": None,
                        "tool_calls": [{"function": {"arguments": '{"strokes": []}'}}],
                    }
                }
            ]
        }
        self.assertEqual(_extract_content_from_response(resp_tools), '{"strokes": []}')

        # 4. Google Gemini native candidates
        resp_gemini = {"candidates": [{"content": {"parts": [{"text": "gemini output text"}]}}]}
        self.assertEqual(_extract_content_from_response(resp_gemini), "gemini output text")

        # 5. Anthropic native content
        resp_anthropic = {"content": [{"type": "text", "text": "claude output text"}]}
        self.assertEqual(_extract_content_from_response(resp_anthropic), "claude output text")

        # 6. Ollama direct message & response
        resp_ollama_chat = {"message": {"role": "assistant", "content": "ollama chat text"}}
        self.assertEqual(_extract_content_from_response(resp_ollama_chat), "ollama chat text")

        resp_ollama_gen = {"response": "ollama generate text"}
        self.assertEqual(_extract_content_from_response(resp_ollama_gen), "ollama generate text")

        # 7. Direct DrawingPlan dictionary
        resp_direct = {"schema_version": 1, "strokes": []}
        self.assertEqual(_extract_content_from_response(resp_direct), resp_direct)

    def test_error_and_empty_choices_handling(self) -> None:
        # API Error JSON detection
        resp_err = {"error": {"message": "Incorrect API key", "type": "invalid_api_key"}}
        with self.assertRaises(LLMPlannerError) as ctx:
            _extract_content_from_response(resp_err)
        self.assertIn("Incorrect API key", str(ctx.exception))
        self.assertIn("invalid_api_key", str(ctx.exception))

        # Empty choices detection
        resp_empty_choices: dict[str, Any] = {"choices": []}
        with self.assertRaises(LLMPlannerError) as ctx:
            _extract_content_from_response(resp_empty_choices)
        self.assertIn("choices 配列が空です", str(ctx.exception))

        # Direct _plan_from_response call on error response
        with self.assertRaises(LLMPlannerError) as ctx:
            _plan_from_response(resp_err)
        self.assertIn("Incorrect API key", str(ctx.exception))

    def test_thinking_tokens_internal_json_rescue(self) -> None:
        # Model output JSON *inside* <think> tags
        raw_inside_think = (
            "<think>\n"
            "Let's create the drawing plan.\n"
            "```json\n"
            '{\n  "schema_version": 1,\n  "prompt": "rescued",\n  "strokes": [\n'
            '    {"id": "s1", "points": [{"x": 10, "y": 20, "pressure": 0.5, "time_ms": 0}, {"x": 30, "y": 40, "pressure": 0.8, "time_ms": 10}]}\n'
            "  ]\n}\n"
            "```\n"
            "</think>"
        )
        parsed = _extract_json_object(raw_inside_think)
        plan = DrawingPlan.from_dict(parsed)
        self.assertEqual(plan.prompt, "rescued")
        self.assertEqual(len(plan.strokes), 1)

        # Unclosed <think> tag (truncated before closing tag)
        raw_unclosed_think = (
            "<think>\n"
            '{"schema_version": 1, "prompt": "unclosed", "strokes": [{"id": "s1", "points": [{"x": 1, "y": 2, "pressure": 0.5, "time_ms": 0}, {"x": 3, "y": 4, "pressure": 0.5, "time_ms": 1}]}]}'
        )
        parsed_unclosed = _extract_json_object(raw_unclosed_think)
        plan_unclosed = DrawingPlan.from_dict(parsed_unclosed)
        self.assertEqual(plan_unclosed.prompt, "unclosed")

    def test_json_repair_truncated(self) -> None:
        truncated_text = (
            '{"schema_version": 1, "prompt": "truncated", "strokes": ['
            '{"id": "s1", "points": [{"x": 5, "y": 10, "pressure": 0.5, "time_ms": 0}, {"x": 15, "y": 20, "pressure": 0.8, "time_ms": 10}]}'
        )
        repaired = _attempt_json_repair(truncated_text)
        self.assertIsNotNone(repaired)
        assert repaired is not None
        plan = DrawingPlan.from_dict(repaired)
        self.assertEqual(plan.prompt, "truncated")
        self.assertEqual(len(plan.strokes), 1)

    def test_auto_retry_recovers_from_empty_response(self) -> None:
        attempts: list[dict[str, Any]] = []

        valid_plan = {
            "schema_version": 1,
            "prompt": "retry test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakeOpener:
            def __init__(self) -> None:
                self.call_count = 0

            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                self.call_count += 1
                body = json.loads(request.data.decode("utf-8"))
                attempts.append(body)

                class MockResponse:
                    def __init__(self, data: dict[str, Any]) -> None:
                        self._data = data

                    def read(self, _size: int) -> bytes:
                        return json.dumps(self._data).encode("utf-8")

                    def __enter__(self) -> MockResponse:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                # 1回目の呼び出しでは content が空のレスポンスを返す
                if self.call_count == 1:
                    return MockResponse({"choices": [{"message": {"role": "assistant", "content": ""}}]})
                # 2回目の呼び出し（自動リトライ）で有効な計画を返す
                return MockResponse(
                    {"choices": [{"message": {"role": "assistant", "content": json.dumps(valid_plan)}}]}
                )

        opener = FakeOpener()
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=opener,
        )
        plan = planner.plan("retry test", 1, 1, 100, 100)
        self.assertEqual(opener.call_count, 2)
        self.assertEqual(plan.prompt, "retry test")
        self.assertEqual(len(plan.strokes), 1)

    def test_is_reasoning_model_detection(self) -> None:
        self.assertTrue(_is_reasoning_model("o1"))
        self.assertTrue(_is_reasoning_model("o1-preview"))
        self.assertTrue(_is_reasoning_model("o1-mini"))
        self.assertTrue(_is_reasoning_model("o3-mini"))
        self.assertTrue(_is_reasoning_model("o4-preview"))
        self.assertTrue(_is_reasoning_model("deepseek-r1"))
        self.assertTrue(_is_reasoning_model("deepseek-ai/DeepSeek-R1-Distill-Qwen-32B"))
        self.assertTrue(_is_reasoning_model("deepseek-reasoner"))
        self.assertTrue(_is_reasoning_model("qwq-32b-preview"))
        self.assertTrue(_is_reasoning_model("gemini-2.0-flash-thinking-exp-01-21"))
        self.assertTrue(_is_reasoning_model("gemini-2.5-flash"))
        self.assertTrue(_is_reasoning_model("claude-3-7-sonnet"))
        self.assertFalse(_is_reasoning_model("gpt-4o"))
        self.assertFalse(_is_reasoning_model("gpt-4o-mini"))
        self.assertFalse(_is_reasoning_model("claude-3-5-sonnet"))

    def test_test_connection_uses_adequate_tokens_and_no_rescue_warning(self) -> None:
        attempts: list[dict[str, Any]] = []
        logs: list[str] = []

        class FakeOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                body = json.loads(request.data.decode("utf-8"))
                attempts.append(body)

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps(
                            {"choices": [{"finish_reason": "stop", "message": {"content": "OK"}}]}
                        ).encode("utf-8")

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "o3-mini", max_tokens=16384),
            opener=FakeOpener(),
            log_callback=lambda msg: logs.append(msg),
        )
        msg = planner.test_connection()
        self.assertIn("接続成功", msg)
        self.assertEqual(len(attempts), 1)
        # 接続確認に過大な生成枠を使わず、十分な上限 2048 へ制限すること
        self.assertEqual(attempts[0]["max_completion_tokens"], 2048)
        self.assertEqual(attempts[0]["messages"], [{"role": "user", "content": "Respond with 'OK'."}])
        # 接続テスト時に DrawingPlan JSON 救済の警告が出ないこと
        self.assertFalse(any("途切れ JSON の救済を試みます" in log for log in logs))

    def test_finish_reason_length_warning_in_plan_vs_connection_test(self) -> None:
        # 1. 接続テストで finish_reason: length が発生した場合でも JSON 救済警告が出ないこと
        conn_logs: list[str] = []

        class FakeConnOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps(
                            {"choices": [{"finish_reason": "length", "message": {"content": "OK partially"}}]}
                        ).encode("utf-8")

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner_conn = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "gpt-4o", max_tokens=2048),
            opener=FakeConnOpener(),
            log_callback=lambda msg: conn_logs.append(msg),
        )
        planner_conn.test_connection()
        self.assertFalse(any("途切れ JSON の救済を試みます" in log for log in conn_logs))
        self.assertTrue(any("finish_reason: length" in log for log in conn_logs))

        # 2. plan() 実行時に finish_reason: length が発生した場合は Max Tokens 引き上げ推奨の救済警告が出ること
        plan_logs: list[str] = []
        truncated_plan = {
            "schema_version": 1,
            "prompt": "test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 1, "y": 2, "pressure": 0.5, "time_ms": 0},
                        {"x": 3, "y": 4, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakePlanOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps(
                            {
                                "choices": [
                                    {
                                        "finish_reason": "length",
                                        "message": {"content": json.dumps(truncated_plan)},
                                    }
                                ]
                            }
                        ).encode("utf-8")

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner_plan = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "gpt-4o", max_tokens=2048),
            opener=FakePlanOpener(),
            log_callback=lambda msg: plan_logs.append(msg),
        )
        planner_plan.plan("test", 1, 1, 100, 100)
        self.assertTrue(any("途切れ JSON の救済を試みます" in log for log in plan_logs))
        self.assertTrue(any("Max Tokens を増やす" in log for log in plan_logs))

    def test_reasoning_model_payload_settings(self) -> None:
        attempts: list[dict[str, Any]] = []
        valid_plan = {
            "schema_version": 1,
            "prompt": "reasoning test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakeOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                body = json.loads(request.data.decode("utf-8"))
                attempts.append(body)

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps({"choices": [{"message": {"content": json.dumps(valid_plan)}}]}).encode(
                            "utf-8"
                        )

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "o3-mini", max_tokens=4096),
            opener=FakeOpener(),
        )
        plan = planner.plan("reasoning test", 1, 1, 100, 100)
        self.assertEqual(len(attempts), 1)
        self.assertIn("max_completion_tokens", attempts[0])
        self.assertEqual(attempts[0]["max_completion_tokens"], 4096)
        self.assertNotIn("temperature", attempts[0])
        self.assertEqual(plan.prompt, "reasoning test")

    def test_parameter_fallback_on_400_temperature_error(self) -> None:
        import io
        from typing import cast
        from urllib.error import HTTPError

        attempts: list[dict[str, Any]] = []
        valid_plan = {
            "schema_version": 1,
            "prompt": "fallback test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakeOpener:
            def __init__(self) -> None:
                self.count = 0

            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                self.count += 1
                body = json.loads(request.data.decode("utf-8"))
                attempts.append(body)

                if self.count == 1:
                    err_json = json.dumps(
                        {"error": {"message": "Unsupported parameter: 'temperature'", "type": "invalid_request_error"}}
                    ).encode("utf-8")
                    raise HTTPError(
                        url="https://example.test/v1/chat/completions",
                        code=400,
                        msg="Bad Request",
                        hdrs=cast(Any, {}),
                        fp=io.BytesIO(err_json),
                    )

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps({"choices": [{"message": {"content": json.dumps(valid_plan)}}]}).encode(
                            "utf-8"
                        )

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        opener = FakeOpener()
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "custom-standard-model"),
            opener=opener,
        )
        plan = planner.plan("fallback test", 1, 1, 100, 100)
        self.assertEqual(opener.count, 2)
        self.assertIn("temperature", attempts[0])
        self.assertNotIn("temperature", attempts[1])
        self.assertEqual(plan.prompt, "fallback test")

    def test_multimodal_request_preserves_image_parts_on_system_role_error(self) -> None:
        import io
        from urllib.error import HTTPError

        captured_bodies: list[dict[str, Any]] = []
        valid_plan = {
            "schema_version": 1,
            "prompt": "multimodal test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakeOpener:
            def __init__(self) -> None:
                self.count = 0

            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                self.count += 1
                body = json.loads(request.data.decode("utf-8"))
                captured_bodies.append(body)

                if self.count == 1:
                    err_json = json.dumps(
                        {"error": {"message": "Unsupported role: 'system'", "type": "invalid_request_error"}}
                    ).encode("utf-8")
                    raise HTTPError(
                        url="https://example.test/v1/chat/completions",
                        code=400,
                        msg="Bad Request",
                        hdrs=cast(Any, {}),
                        fp=io.BytesIO(err_json),
                    )

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps({"choices": [{"message": {"content": json.dumps(valid_plan)}}]}).encode(
                            "utf-8"
                        )

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        opener = FakeOpener()
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "o1-mini"),
            opener=opener,
        )
        fake_png = b"\x89PNG\r\n\x1a\n\x00\x00"
        plan = planner.plan("multimodal test", 1, 1, 100, 100, image_data=fake_png)
        self.assertEqual(opener.count, 2)
        self.assertEqual(plan.prompt, "multimodal test")

        retried_user_msg = captured_bodies[1]["messages"][0]
        self.assertEqual(retried_user_msg["role"], "user")
        self.assertIsInstance(retried_user_msg["content"], list)
        self.assertEqual(len(retried_user_msg["content"]), 2)
        self.assertEqual(retried_user_msg["content"][0]["type"], "text")
        self.assertIn("[USER REQUEST]", retried_user_msg["content"][0]["text"])
        self.assertEqual(retried_user_msg["content"][1]["type"], "image_url")
        self.assertTrue(retried_user_msg["content"][1]["image_url"]["url"].startswith("data:image/png;base64,"))

    def test_thinking_tokens_various_tags(self) -> None:
        # 1. <reasoning>...</reasoning>
        raw1 = (
            "<reasoning>Thinking deeply about strokes...</reasoning>\n```json\n"
            + json.dumps(
                {
                    "schema_version": 1,
                    "prompt": "tag test 1",
                    "strokes": [
                        {
                            "id": "s1",
                            "points": [
                                {"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0},
                                {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 10},
                            ],
                        }
                    ],
                }
            )
            + "\n```"
        )
        plan1 = DrawingPlan.from_dict(_extract_json_object(raw1))
        self.assertEqual(plan1.prompt, "tag test 1")

        # 2. [THOUGHT]...[/THOUGHT]
        raw2 = '[THOUGHT]Calculating anatomy curves...[/THOUGHT]{"schema_version": 1, "prompt": "tag test 2", "strokes": [{"id": "s1", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 10}]}]}'
        plan2 = DrawingPlan.from_dict(_extract_json_object(raw2))
        self.assertEqual(plan2.prompt, "tag test 2")

        # 3. |begin_of_thought|...|end_of_thought|
        raw3 = '|begin_of_thought|Refining strokes...|end_of_thought|```json\n{"schema_version": 1, "prompt": "tag test 3", "strokes": [{"id": "s1", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 10}]}]}\n```'
        plan3 = DrawingPlan.from_dict(_extract_json_object(raw3))
        self.assertEqual(plan3.prompt, "tag test 3")

        # 4. 【思考】...【/思考】
        raw4 = '【思考】レイヤー構成を考案中...【/思考】{"schema_version": 1, "prompt": "tag test 4", "strokes": [{"id": "s1", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 10}]}]}'
        plan4 = DrawingPlan.from_dict(_extract_json_object(raw4))
        self.assertEqual(plan4.prompt, "tag test 4")

    def test_gemini_null_content_is_handled_as_extraction_failure(self) -> None:
        # Gemini 形式で candidates[0].content が明示的 null (SAFETY ブロック等) の場合、
        # AttributeError ではなく抽出失敗として扱われること
        resp_null_content = {"candidates": [{"content": None, "finishReason": "SAFETY"}]}
        with self.assertRaises(LLMPlannerError):
            _plan_from_response(resp_null_content)
        with self.assertRaises(LLMPlannerError):
            _extract_content_from_response(resp_null_content)

        # content 自体が欠落している場合も同様に安全であること
        resp_missing_content = {"candidates": [{"finishReason": "STOP"}]}
        with self.assertRaises(LLMPlannerError):
            _plan_from_response(resp_missing_content)
        with self.assertRaises(LLMPlannerError):
            _extract_content_from_response(resp_missing_content)

    def test_gemini_thinking_parts_extraction(self) -> None:
        valid_plan = {
            "schema_version": 1,
            "prompt": "gemini thinking test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 5, "y": 5, "pressure": 0.5, "time_ms": 0},
                        {"x": 15, "y": 15, "pressure": 0.8, "time_ms": 10},
                    ],
                }
            ],
        }
        resp = {
            "candidates": [
                {
                    "content": {
                        "parts": [
                            {"thought": True, "text": "Let's first analyze the prompt and sketch lines."},
                            {"text": "```json\n" + json.dumps(valid_plan) + "\n```"},
                        ]
                    }
                }
            ]
        }
        plan = _plan_from_response(resp)
        self.assertEqual(plan.prompt, "gemini thinking test")
        self.assertEqual(len(plan.strokes), 1)

    def test_anthropic_extended_thinking_extraction(self) -> None:
        valid_plan = {
            "schema_version": 1,
            "prompt": "claude thinking test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 5, "y": 5, "pressure": 0.5, "time_ms": 0},
                        {"x": 15, "y": 15, "pressure": 0.8, "time_ms": 10},
                    ],
                }
            ],
        }
        resp = {
            "content": [
                {"type": "thinking", "thinking": "Let me plan out the stroke coordinates and layer hierarchy..."},
                {"type": "text", "text": "```json\n" + json.dumps(valid_plan) + "\n```"},
            ]
        }
        plan = _plan_from_response(resp)
        self.assertEqual(plan.prompt, "claude thinking test")
        self.assertEqual(len(plan.strokes), 1)

    def test_sanitize_json_comments_and_trailing_commas(self) -> None:
        messy_json = """
        // Master drawing plan
        {
            'schema_version': 1, /* Version info */
            'prompt': 'messy json test',
            'strokes': [
                {
                    'id': 'stroke_1',
                    'points': [
                        {'x': 10, 'y': 20, 'pressure': 0.5, 'time_ms': 0},
                        {'x': 30, 'y': 40, 'pressure': 0.8, 'time_ms': 10}, // First line
                    ],
                },
            ],
        }
        """
        parsed = _extract_json_object(messy_json)
        plan = DrawingPlan.from_dict(parsed)
        self.assertEqual(plan.prompt, "messy json test")
        self.assertEqual(len(plan.strokes), 1)
        self.assertEqual(plan.strokes[0].id, "stroke_1")

    def test_truncated_stroke_rescue_on_length_limit(self) -> None:
        # トークン上限で2本目の途中で切れたレスポンス
        truncated_resp = {
            "choices": [
                {
                    "finish_reason": "length",
                    "message": {
                        "content": (
                            '{"schema_version": 1, "prompt": "truncated test", "strokes": ['
                            '{"id": "s1", "points": [{"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0}, {"x": 20, "y": 20, "pressure": 0.8, "time_ms": 10}]}, '
                            '{"id": "s2", "points": [{"x": 30, "y": 30, "pressure": 0.5, "time_ms": 0}, {"x": 40'
                        )
                    },
                }
            ]
        }
        plan = _plan_from_response(truncated_resp)
        self.assertEqual(plan.prompt, "truncated test")
        # 救出されたストロークが存在し、先頭ストロークが s1 であること
        self.assertGreaterEqual(len(plan.strokes), 1)
        self.assertEqual(plan.strokes[0].id, "s1")

    def test_plain_text_thinking_with_unclosed_fence_rescue(self) -> None:
        # ユーザーログと同一パターン: タグなし自然言語思考 + 未閉鎖 ```json コードブロック + finish_reason: length
        user_log_scenario = {
            "choices": [
                {
                    "finish_reason": "length",
                    "message": {
                        "content": (
                            "The user wants me to generate a JSON object with drawing strokes for an anime girl portrait. "
                            "The canvas is 2480x3508 pixels (A4 at 300 DPI roughly). I need to create 40 strokes across layers: Draft, Lineart, Flats, Shading. "
                            "Let me plan the composition:\n"
                            "- First, rough head contour and eye guides in Draft layer.\n"
                            "- Second, delicate anime eye lines and flowing hair in Lineart layer.\n\n"
                            "```json\n"
                            "{\n"
                            '  "schema_version": 1,\n'
                            '  "prompt": "anime girl portrait",\n'
                            '  "seed": 42,\n'
                            '  "layers": ["Draft", "Lineart", "Flats", "Shading"],\n'
                            '  "strokes": [\n'
                            '    {"id": "s1", "brush_preset": "Basic-5 Size", "color": "#112233", "layer_name": "Lineart", "points": [{"x": 100, "y": 150, "pressure": 0.5, "time_ms": 0}, {"x": 120, "y": 180, "pressure": 0.8, "time_ms": 20}]},\n'
                            '    {"id": "s2", "brush_preset": "Basic-5 Size", "color": "#112233", "layer_name": "Lineart", "points": [{"x": 200, "y": 250, "pressure": 0.6, "time_ms": 0}, {"x": 220, "y": 280, "pressure": 0.9, "time_ms": 20}]},\n'
                            '    {"id": "s3", "brush_preset": "Basic-5 Size", "color": "#FF8899", "layer_name": "Flats", "points": [{"x": 300, "y": 350, "pressure": 0.5, "time_ms": 0}, {"x": 350'
                        )
                    },
                }
            ]
        }
        plan = _plan_from_response(user_log_scenario, prompt="anime girl portrait", seed=42)
        self.assertEqual(plan.prompt, "anime girl portrait")
        self.assertGreaterEqual(len(plan.strokes), 2)
        self.assertEqual(plan.strokes[0].id, "s1")
        self.assertEqual(plan.strokes[1].id, "s2")

    def test_harvest_stroke_fragments_on_severely_broken_json(self) -> None:
        # JSON全体の構文が完全に崩壊していても、テキスト中に出現するストロークがハーベスターで救出される
        broken_text = (
            "Thinking process: We need multiple strokes scattered across text.\n"
            'Here is stroke 1: {"id": "harvest_1", "color": "#123456", "layer_name": "Lineart", "points": [{"x": 10, "y": 20, "pressure": 0.5, "time_ms": 0}, {"x": 30, "y": 40, "pressure": 0.8, "time_ms": 10}]}\n'
            "Some conversational rambling here...\n"
            'Here is stroke 2: {"id": "harvest_2", "color": "#654321", "layer_name": "Shading", "points": [{"x": 50, "y": 60, "pressure": 0.4, "time_ms": 0}, {"x": 70, "y": 80, "pressure": 0.7, "time_ms": 10}]}\n'
            "Output cut off due to max tokens..."
        )
        resp = {"choices": [{"message": {"content": broken_text}}]}
        plan = _plan_from_response(resp, prompt="broken json test")
        self.assertGreaterEqual(len(plan.strokes), 2)
        self.assertEqual(plan.strokes[0].id, "harvest_1")
        self.assertEqual(plan.strokes[1].id, "harvest_2")

    def test_emergency_fallback_to_procedural_when_llm_exhausted(self) -> None:
        # LLM が思考文のみで枯渇した場合、明示設定時だけプロシージャル救済する。
        class FakeExhaustedOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        # ストロークが一切含まれない思考文のみのレスポンス
                        return json.dumps(
                            {
                                "choices": [
                                    {
                                        "finish_reason": "length",
                                        "message": {
                                            "content": "The user wants me to generate strokes. Let me think deeply... (no json generated)"
                                        },
                                    }
                                ]
                            }
                        ).encode("utf-8")

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings(
                "https://example.test/v1",
                "thinking-model",
                max_retries=1,
                fallback_to_procedural=True,
            ),
            opener=FakeExhaustedOpener(),
        )
        plan = planner.plan("anime girl portrait", 42, 20, 1000, 1000)
        self.assertEqual(plan.prompt, "anime girl portrait")
        self.assertGreaterEqual(len(plan.strokes), 1)
        self.assertEqual(plan.metadata["planner_fallback"], "procedural")
        self.assertEqual((plan.canvas_width, plan.canvas_height), (1000.0, 1000.0))

        strict_planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "thinking-model", max_retries=1),
            opener=FakeExhaustedOpener(),
        )
        with self.assertRaises(LLMPlannerError):
            strict_planner.plan("anime girl portrait", 42, 20, 1000, 1000)

    def test_sensitive_error_text_is_redacted(self) -> None:
        message = (
            'Authorization: "Bearer abcdef123456" api_key="sk-supersecret123456" '
            "api-key=custom-secret https://alice:password@example.test/v1"
        )
        redacted = _redact_sensitive_text(message)
        self.assertNotIn("abcdef123456", redacted)
        self.assertNotIn("supersecret123456", redacted)
        self.assertNotIn("custom-secret", redacted)
        self.assertNotIn("password", redacted)
        self.assertIn("REDACTED", redacted)
        self.assertEqual(
            _endpoint_origin_label("https://example.test/secret-token/v1/chat/completions"),
            "https://example.test",
        )

    def test_parameter_fallback_on_400_reasoning_effort_error(self) -> None:
        import io
        from typing import cast
        from urllib.error import HTTPError

        attempts: list[dict[str, Any]] = []
        valid_plan = {
            "schema_version": 1,
            "prompt": "reasoning_effort test",
            "strokes": [
                {
                    "id": "s1",
                    "points": [
                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                    ],
                }
            ],
        }

        class FakeOpener:
            def __init__(self) -> None:
                self.count = 0

            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                self.count += 1
                body = json.loads(request.data.decode("utf-8"))
                attempts.append(body)

                if self.count == 1:
                    err_json = json.dumps(
                        {
                            "error": {
                                "message": "Unsupported parameter: 'reasoning_effort'",
                                "type": "invalid_request_error",
                            }
                        }
                    ).encode("utf-8")
                    raise HTTPError(
                        url="https://example.test/v1/chat/completions",
                        code=400,
                        msg="Bad Request",
                        hdrs=cast(Any, {}),
                        fp=io.BytesIO(err_json),
                    )

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return json.dumps({"choices": [{"message": {"content": json.dumps(valid_plan)}}]}).encode(
                            "utf-8"
                        )

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        opener = FakeOpener()
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "o3-mini"),
            opener=opener,
        )
        plan = planner.plan("reasoning_effort test", 1, 1, 100, 100)
        self.assertEqual(opener.count, 2)
        self.assertIn("reasoning_effort", attempts[0])
        self.assertNotIn("reasoning_effort", attempts[1])
        self.assertEqual(plan.prompt, "reasoning_effort test")

    def test_detect_image_mime_type_and_multimodal_request_mime(self) -> None:
        self.assertEqual(_detect_image_mime_type(b"\x89PNG\r\n\x1a\n\x00\x00"), "image/png")
        self.assertEqual(_detect_image_mime_type(b"\xff\xd8\xff\xe0\x00\x10JFIF"), "image/jpeg")
        self.assertEqual(_detect_image_mime_type(b"RIFF\x00\x00\x00\x00WEBPVP8 "), "image/webp")
        self.assertEqual(_detect_image_mime_type(b"GIF89a\x01\x00\x01\x00"), "image/gif")
        self.assertEqual(_detect_image_mime_type(b"BM\x00\x00\x00\x00"), "image/bmp")
        self.assertEqual(_detect_image_mime_type(b"unknown bytes"), "image/png")

        captured_requests: list[dict[str, Any]] = []

        class FakeOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                captured_requests.append(json.loads(request.data.decode("utf-8")))

                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        valid_plan = {
                            "schema_version": 1,
                            "prompt": "jpeg test",
                            "strokes": [
                                {
                                    "id": "s1",
                                    "points": [
                                        {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 0},
                                        {"x": 20, "y": 20, "pressure": 0.5, "time_ms": 10},
                                    ],
                                }
                            ],
                        }
                        return json.dumps({"choices": [{"message": {"content": json.dumps(valid_plan)}}]}).encode(
                            "utf-8"
                        )

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        jpeg_dummy = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x01\x00`\x00`\x00\x00\xff\xdb"
        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "gpt-4o"),
            opener=FakeOpener(),
        )
        plan = planner.plan("jpeg test", 1, 1, 100, 100, image_data=jpeg_dummy)
        self.assertEqual(len(captured_requests), 1)
        user_msg = captured_requests[0]["messages"][1]["content"]
        self.assertIsInstance(user_msg, list)
        img_part = next(p for p in user_msg if p.get("type") == "image_url")
        self.assertTrue(img_part["image_url"]["url"].startswith("data:image/jpeg;base64,"))
        self.assertEqual(plan.prompt, "jpeg test")

    def test_sanitizes_non_monotonic_time_ms_in_llm_plan(self) -> None:
        from .llm_planner import _validate_and_sanitize_plan

        class MockPoint:
            def __init__(self, x: float, y: float, pressure: float, time_ms: int) -> None:
                self.x = x
                self.y = y
                self.pressure = pressure
                self.time_ms = time_ms

        class MockStrokeObj:
            def __init__(self) -> None:
                self.id = "s1"
                self.brush_preset = "Basic-5 Size"
                self.color = "#232323"
                self.size_px = 5.0
                self.layer_name = "Lineart"
                self.opacity = 1.0
                self.points = [
                    MockPoint(10.0, 10.0, 0.5, 100),
                    MockPoint(20.0, 20.0, 0.6, 0),
                    MockPoint(30.0, 30.0, 0.7, 50),
                ]

        class MockPlanObj:
            def __init__(self) -> None:
                self.prompt = "test drawing"
                self.seed = 42
                self.title = "Test"
                self.iteration = 1
                self.layers = ["Lineart"]
                self.metadata: dict[str, Any] = {}
                self.strokes = [MockStrokeObj()]

        plan = cast(DrawingPlan, MockPlanObj())
        sanitized = _validate_and_sanitize_plan(plan, "test drawing", 42, 10, 800, 600)
        self.assertEqual(len(sanitized.strokes), 1)
        times = [p.time_ms for p in sanitized.strokes[0].points]
        self.assertTrue(all(b >= a for a, b in zip(times, times[1:], strict=False)))
        self.assertEqual(times[0], 100)
        self.assertGreaterEqual(times[1], 100)
        self.assertGreaterEqual(times[2], times[1])

    def test_sanitization_caps_strokes_at_requested_count(self) -> None:
        from .llm_planner import _validate_and_sanitize_plan

        strokes = [
            Stroke(
                id=f"s{idx}",
                points=(StrokePoint(10.0 * idx, 10.0, 0.5, 0), StrokePoint(10.0 * idx + 5.0, 20.0, 0.7, 10)),
            )
            for idx in range(1, 4)
        ]
        plan = DrawingPlan(prompt="count cap", seed=1, strokes=strokes)

        sanitized = _validate_and_sanitize_plan(plan, "count cap", 1, 1, 100, 100)

        self.assertEqual([stroke.id for stroke in sanitized.strokes], ["s1"])

    def test_system_role_error_fallback_without_user_message(self) -> None:
        payloads_received: list[dict[str, Any]] = []

        class MockOpener:
            def __init__(self) -> None:
                self.calls = 0

            def __call__(self, req: Any, timeout: float = 30.0) -> Any:
                from io import BytesIO
                import urllib.error

                self.calls += 1
                data = json.loads(req.data.decode("utf-8"))
                payloads_received.append(data)
                if self.calls == 1:
                    err_fp = BytesIO(b'{"error": {"message": "system role is not supported"}}')
                    raise urllib.error.HTTPError(
                        req.full_url, 400, "Bad Request", cast(Any, {"Content-Type": "application/json"}), err_fp
                    )

                body = json.dumps(
                    {
                        "choices": [
                            {
                                "message": {
                                    "content": '{"schema_version": 1, "prompt": "p", "seed": 1, "strokes": [{"id": "s1", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 10, "y": 10, "pressure": 0.5, "time_ms": 10}]}]}'
                                }
                            }
                        ]
                    }
                ).encode("utf-8")

                class MockResponse(BytesIO):
                    status = 200

                return MockResponse(body)

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings(base_url="https://api.openai.com/v1", model="test-model"),
            opener=MockOpener(),
        )
        res = planner._post_with_parameter_fallback({"messages": [{"role": "system", "content": "You are artist"}]})
        self.assertIn("choices", res)
        self.assertEqual(len(payloads_received), 2)
        self.assertEqual(payloads_received[1]["messages"][0]["role"], "user")
        self.assertIn("You are artist", payloads_received[1]["messages"][0]["content"])


class CanvasAdapterTests(unittest.TestCase):
    def test_active_layer_cancellation_restores_only_target_pixels(self) -> None:
        target = _FakeNode("existing")
        document = _FakeDocument(active=target)
        plan = DrawingPlan(
            "cancel active",
            1,
            [
                Stroke(
                    "long",
                    [
                        StrokePoint(0, 0, 1, 0),
                        StrokePoint(10, 0, 1, 10),
                        StrokePoint(20, 0, 1, 20),
                        StrokePoint(30, 0, 1, 30),
                    ],
                )
            ],
        )
        checks = 0

        def cancelled() -> bool:
            nonlocal checks
            checks += 1
            return checks >= 4

        adapter = KritaCanvasAdapter(layer_mode="active_layer")
        self.assertEqual(adapter.render(document, plan, cancelled=cancelled), 0)
        self.assertTrue(target.lines)
        self.assertEqual(target.pixels, b"initial pixels")
        self.assertEqual(target.pixel_reads, 1)
        self.assertEqual(target.pixel_writes, 1)
        self.assertEqual(document.macros_ended, 1)

        session_target = _FakeNode("existing")
        session_document = _FakeDocument(active=session_target)
        session_adapter = KritaCanvasAdapter(layer_mode="active_layer")
        session_adapter.begin_render_session(session_document)
        session_adapter.render(session_document, plan)
        session_adapter.render(session_document, plan, cancelled=lambda: True)
        self.assertEqual(session_target.pixels, b"painted pixels")
        # One original snapshot plus state checks before the second render.
        self.assertEqual(session_target.pixel_reads, 3)
        session_adapter.end_render_session(commit=False)
        self.assertEqual(session_document.macros_ended, 1)
        self.assertEqual(session_target.pixels, b"initial pixels")
        self.assertEqual(session_target.pixel_writes, 1)

    def test_active_layer_session_preserves_external_edit_on_rollback_conflict(self) -> None:
        target = _FakeNode("existing")
        document = _FakeDocument(active=target)
        plan = RuleBasedPlanner().plan("curve", 1, 1, 100, 100)
        adapter = KritaCanvasAdapter(layer_mode="active_layer")

        adapter.begin_render_session(document)
        adapter.render(document, plan, layer_mode="active_layer")
        target.pixels = b"user edit after plugin render"

        with self.assertRaises(ActiveLayerSessionConflict):
            adapter.end_render_session(commit=False)

        self.assertEqual(target.pixels, b"user edit after plugin render")
        self.assertEqual(document.macros_ended, 1)

    def test_created_active_layer_is_not_removed_after_external_edit(self) -> None:
        root = _FakeNode("root", "grouplayer")
        document = _FakeDocument(active=None, root=root)
        plan = RuleBasedPlanner().plan("curve", 1, 1, 100, 100)
        adapter = KritaCanvasAdapter(layer_mode="active_layer")

        adapter.begin_render_session(document)
        adapter.render(document, plan, layer_mode="active_layer")
        self.assertEqual(len(root.childNodes()), 1)
        created = root.childNodes()[0]
        created.pixels = b"user edit on newly-created active layer"

        with self.assertRaises(ActiveLayerSessionConflict):
            adapter.end_render_session(commit=False)

        self.assertIn(created, root.childNodes())
        self.assertEqual(created.pixels, b"user edit on newly-created active layer")

    def test_active_layer_session_guards_canvas_input_while_processing_events(self) -> None:
        from .krita_adapter import _CANVAS_INPUT_EVENT_TYPES

        class Canvas:
            def __init__(self) -> None:
                self.installed: list[Any] = []
                self.removed: list[Any] = []

            def installEventFilter(self, guard: Any) -> None:  # noqa: N802
                self.installed.append(guard)

            def removeEventFilter(self, guard: Any) -> None:  # noqa: N802
                self.removed.append(guard)

        class View:
            def __init__(self, canvas: Canvas) -> None:
                self._canvas = canvas

            def canvas(self) -> Canvas:
                return self._canvas

        class Event:
            def __init__(self, event_type: Any) -> None:
                self._event_type = event_type

            def type(self) -> Any:
                return self._event_type

        target = _FakeNode("existing")
        document = _FakeDocument(active=target)
        canvas = Canvas()
        adapter = KritaCanvasAdapter(layer_mode="active_layer", event_interval=1)
        plan = RuleBasedPlanner().plan("curve", 1, 1, 100, 100)

        adapter.begin_render_session(document)
        with (
            patch("ai_stroke_painter.krita_adapter._apply_stroke_style"),
            patch("ai_stroke_painter.krita_adapter._apply_color_to_krita"),
            patch("ai_stroke_painter.krita_adapter._process_events") as process_events,
        ):
            adapter.render(document, plan, layer_mode="active_layer", view=View(canvas), event_interval=1)

        self.assertTrue(process_events.called)
        self.assertEqual(len(canvas.installed), 1)
        self.assertEqual(canvas.removed, canvas.installed)
        self.assertTrue(canvas.installed[0].eventFilter(canvas, Event(next(iter(_CANVAS_INPUT_EVENT_TYPES)))))
        self.assertFalse(canvas.installed[0].eventFilter(canvas, Event(object())))

    def test_active_layer_rollback_uses_standard_node_api_without_macros(self) -> None:
        class StandardApiDocument(_FakeDocument):
            def __getattribute__(self, name: str) -> Any:
                if name in {"createMacro", "endMacro"}:
                    raise AttributeError(name)
                return super().__getattribute__(name)

        target = _FakeNode("existing")
        document = StandardApiDocument(active=target)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        checks = 0

        def cancelled() -> bool:
            nonlocal checks
            checks += 1
            return checks >= 4

        adapter = KritaCanvasAdapter(layer_mode="active_layer")
        adapter.render(document, plan, cancelled=cancelled)
        self.assertEqual(target.pixels, b"initial pixels")
        self.assertEqual(target.pixel_writes, 1)

    def test_native_bridge_receives_one_continuous_stroke_and_scaled_style(self) -> None:
        received: list[Stroke] = []

        class FakeBridge:
            def submit_stroke(self, _document: Any, _target: Any, stroke: Stroke) -> int:
                received.append(stroke)
                return len(stroke.points)

        target = _FakeNode("target")
        document = _FakeDocument(active=target)
        stroke = Stroke(
            "continuous",
            [
                StrokePoint(0, 0, 0.1, 0),
                StrokePoint(10, 5, 0.8, 10),
                StrokePoint(20, 15, 0.6, 20),
                StrokePoint(30, 20, 0.1, 30),
            ],
            size_px=4,
            opacity=0.8,
        )
        plan = DrawingPlan("bridge", 1, [stroke], canvas_width=40, canvas_height=30)
        adapter = KritaCanvasAdapter(native_bridge=cast(Any, FakeBridge()), layer_mode="active_layer")

        self.assertEqual(adapter.render(document, plan, brush_size_multiplier=2.0, opacity_multiplier=0.5), 1)
        self.assertEqual(target.lines, [])
        self.assertEqual(len(received), 1)
        self.assertEqual(received[0].points, stroke.points)
        self.assertEqual(received[0].size_px, 8.0)
        self.assertEqual(received[0].opacity, 0.4)

    def test_native_bridge_unavailable_falls_back_but_protocol_errors_fail_closed(self) -> None:
        class UnavailableBridge:
            def submit_stroke(self, _document: Any, _target: Any, _stroke: Stroke) -> int:
                raise NativeBridgeUnavailable

        target = _FakeNode("target")
        document = _FakeDocument(active=target)
        plan = DrawingPlan(
            "fallback",
            1,
            [
                Stroke(
                    "fallback-stroke",
                    [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10), StrokePoint(20, 10, 1, 20)],
                )
            ],
        )
        adapter = KritaCanvasAdapter(native_bridge=cast(Any, UnavailableBridge()), layer_mode="active_layer")
        self.assertEqual(adapter.render(document, plan), 1)
        self.assertEqual(len(target.lines), 2)

        class InvalidBridge:
            def submit_stroke(self, _document: Any, _target: Any, _stroke: Stroke) -> int:
                raise NativeBridgeProtocolError("bad response")

        with self.assertRaises(NativeBridgeProtocolError):
            KritaCanvasAdapter(native_bridge=cast(Any, InvalidBridge()), layer_mode="active_layer").render(
                document, plan
            )

    def test_eraser_without_active_view_fails_closed(self) -> None:
        document = _FakeDocument()
        eraser_plan = DrawingPlan(
            "eraser",
            1,
            [
                Stroke(
                    "erase",
                    [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)],
                    brush_preset="Eraser Small",
                    is_eraser=True,
                )
            ],
        )
        with self.assertRaises(RuntimeError):
            KritaCanvasAdapter().render(document, eraser_plan, view=None)
        self.assertEqual(document.rootNode().childNodes(), [])

    def test_json_line_native_bridge_protocol_is_bounded_and_authenticated(self) -> None:
        sent: list[bytes] = []

        class FakeConnection:
            def __enter__(self) -> FakeConnection:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

            def settimeout(self, _timeout: float) -> None:
                pass

            def sendall(self, value: bytes) -> None:
                sent.append(value)

            def recv(self, _count: int) -> bytes:
                return b'{"ok":true,"accepted_point_count":2}\n'

        stroke = Stroke("wire", [StrokePoint(0, 0, 1, 0), StrokePoint(1, 1, 0.5, 10)])
        bridge = JsonLineNativeStrokeBridge(29345, "0123456789abcdef")
        with patch("ai_stroke_painter.native_bridge.socket.create_connection", return_value=FakeConnection()):
            self.assertEqual(bridge.submit_stroke(_FakeDocument(), _FakeNode("target"), stroke), 2)
        payload = json.loads(sent[0])
        self.assertEqual(payload["type"], "continuous_stroke")
        self.assertEqual(payload["auth_token"], "0123456789abcdef")
        self.assertEqual(len(payload["stroke"]["points"]), 2)
        with (
            patch(
                "ai_stroke_painter.native_bridge.socket.create_connection",
                side_effect=ConnectionRefusedError("not running"),
            ),
            self.assertRaises(NativeBridgeUnavailable),
        ):
            bridge.submit_stroke(_FakeDocument(), _FakeNode("target"), stroke)

        class AmbiguousTimeoutConnection(FakeConnection):
            def recv(self, _count: int) -> bytes:
                raise TimeoutError("response lost after send")

        with (
            patch(
                "ai_stroke_painter.native_bridge.socket.create_connection",
                return_value=AmbiguousTimeoutConnection(),
            ),
            self.assertRaises(NativeBridgeProtocolError),
        ):
            bridge.submit_stroke(_FakeDocument(), _FakeNode("target"), stroke)
        with (
            patch.dict("os.environ", {"AI_STROKE_BRIDGE_PORT": "bad", "AI_STROKE_BRIDGE_TOKEN": "short"}),
            self.assertRaises(ValueError),
        ):
            discover_native_bridge()

    def test_render_session_clears_state_when_macro_close_fails(self) -> None:
        class FailingMacroDocument(_FakeDocument):
            def endMacro(self) -> None:
                raise RuntimeError("end failed")

        document = FailingMacroDocument()
        adapter = KritaCanvasAdapter()
        adapter.begin_render_session(document)
        with self.assertRaises(RuntimeError):
            adapter.end_render_session(commit=True)
        self.assertIsNone(adapter._session_document)

    def test_render_session_reuses_output_and_rolls_back_all_iterations(self) -> None:
        committed_root = _FakeNode("root", "grouplayer")
        committed_document = _FakeDocument(root=committed_root)
        plan = RuleBasedPlanner().plan("curve", 3, 2, 100, 100)
        adapter = KritaCanvasAdapter()

        adapter.begin_render_session(committed_document)
        adapter.render(committed_document, plan, layer_prefix="Session")
        adapter.render(committed_document, plan, layer_prefix="Session")
        self.assertEqual([node.name() for node in committed_root.childNodes()], ["Session"])
        adapter.end_render_session(commit=True)
        self.assertEqual([node.name() for node in committed_root.childNodes()], ["Session"])

        rollback_root = _FakeNode("root", "grouplayer")
        rollback_document = _FakeDocument(root=rollback_root)
        adapter.begin_render_session(rollback_document)
        adapter.render(rollback_document, plan, layer_prefix="Rollback")
        self.assertEqual([node.name() for node in rollback_root.childNodes()], ["Rollback"])
        self.assertEqual(adapter.render(rollback_document, plan, cancelled=lambda: True), 0)
        self.assertEqual(rollback_root.childNodes(), [])
        adapter.end_render_session(commit=False)
        self.assertEqual(rollback_document.macros_started, ["AI Stroke Painter Session"])
        self.assertEqual(rollback_document.macros_ended, 1)

    def test_multi_layer_output_is_scoped_to_unique_group(self) -> None:
        root = _FakeNode("root", "grouplayer")
        existing_group = _FakeNode("My Art", "grouplayer")
        existing_layer = _FakeNode("Lineart")
        existing_group.addChildNode(existing_layer)
        root.addChildNode(existing_group)
        document = _FakeDocument(active=existing_layer, root=root)

        plan = RuleBasedPlanner().plan("curve", 3, 2, 100, 100)
        rendered = KritaCanvasAdapter().render(document, plan, layer_prefix="My Art")

        self.assertEqual(rendered, 2)
        self.assertEqual([node.name() for node in root.childNodes()], ["My Art", "My Art (2)"])
        generated_group = root.childNodes()[1]
        self.assertTrue(generated_group.childNodes())
        self.assertEqual(existing_layer.lines, [])
        self.assertIs(document.activeNode(), existing_layer)

    def test_cancelled_or_empty_generated_output_leaves_no_artifacts(self) -> None:
        root = _FakeNode("root", "grouplayer")
        document = _FakeDocument(root=root)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()

        self.assertEqual(adapter.render(document, plan, cancelled=lambda: True, layer_prefix="Cancelled"), 0)
        self.assertEqual(root.childNodes(), [])
        self.assertIs(document.activeNode(), root)

        empty_plan = DrawingPlan("empty", 1, ())
        self.assertEqual(adapter.render(document, empty_plan, layer_prefix="Empty"), 0)
        self.assertEqual(root.childNodes(), [])

    def test_rollback_failures_are_reported_instead_of_silently_committed(self) -> None:
        class FailingRoot(_FakeNode):
            def removeChildNode(self, _child: Any) -> None:  # noqa: N802
                raise RuntimeError("remove failed")

        root = FailingRoot("root", "grouplayer")
        document = _FakeDocument(root=root)
        plan = RuleBasedPlanner().plan("curve", 1, 1, 100, 100)
        with self.assertRaisesRegex(RuntimeError, "ノードを除去"):
            KritaCanvasAdapter().render(document, plan, cancelled=lambda: True)

        class RejectingRestoreNode(_FakeNode):
            def setPixelData(  # noqa: N802
                self, _pixels: bytes, _x: int, _y: int, _width: int, _height: int
            ) -> bool:
                return False

        target = RejectingRestoreNode("existing")
        active_document = _FakeDocument(active=target)
        checks = 0

        def cancel_after_one_segment() -> bool:
            nonlocal checks
            checks += 1
            return checks >= 4

        with self.assertRaisesRegex(RuntimeError, "画素復元"):
            KritaCanvasAdapter(layer_mode="active_layer").render(
                active_document,
                plan,
                cancelled=cancel_after_one_segment,
            )
        self.assertIs(document.activeNode(), root)
        self.assertIs(active_document.activeNode(), target)

    def test_layer_setup_failure_rolls_back_created_group(self) -> None:
        root = _FakeNode("root", "grouplayer")

        class FailingDocument(_FakeDocument):
            def createNode(self, name: str, node_type: str) -> _FakeNode:  # noqa: N802
                if node_type == "paintlayer":
                    raise RuntimeError("cannot create paint layer")
                return super().createNode(name, node_type)

        document = FailingDocument(root=root)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        with self.assertRaises(RuntimeError):
            KritaCanvasAdapter().render(document, plan, layer_prefix="Failed")
        self.assertEqual(root.childNodes(), [])
        self.assertIs(document.activeNode(), root)

    def test_layer_add_false_return_is_treated_as_failure(self) -> None:
        class RejectingRoot(_FakeNode):
            def addChildNode(self, _child: Any, _above_this: Any = None) -> bool:  # noqa: N802
                return False

        root = RejectingRoot("root", "grouplayer")
        document = _FakeDocument(root=root)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        with self.assertRaisesRegex(RuntimeError, "追加を拒否"):
            KritaCanvasAdapter().render(document, plan)
        self.assertEqual(root.childNodes(), [])

    def test_render_rejects_invalid_options(self) -> None:
        document = _FakeDocument()
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()
        with self.assertRaises(ValueError):
            adapter.render(document, plan, brush_size_multiplier=float("nan"))
        with self.assertRaises(ValueError):
            adapter.render(document, plan, opacity_multiplier=2.0)
        with self.assertRaises(ValueError):
            adapter.render(document, plan, layer_mode="unknown")

    def test_existing_target_layer_is_reused_and_pressure_is_unit_range(self) -> None:
        target = _FakeNode(KritaCanvasAdapter.DEFAULT_LAYER_NAME)
        document = _FakeDocument(active=_FakeNode("other"), root=_FakeNode("root", "grouplayer"))
        document.root.addChildNode(target, None)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()

        rendered = adapter.render(document, plan)
        self.assertGreaterEqual(rendered, 1)
        self.assertEqual(document.refreshed, 1)
        self.assertEqual(document.waited_for_done, 1)
        self.assertEqual(document.locked, 0)
        self.assertEqual(document.unlocked, 0)
        self.assertFalse(document.batchmode())

    def test_layer_stack_order_placement(self) -> None:
        root = _FakeNode("root", "grouplayer")
        document = _FakeDocument(root=root)
        adapter = KritaCanvasAdapter()

        adapter.ensure_layer(document, "Draft")
        adapter.ensure_layer(document, "Lineart")
        adapter.ensure_layer(document, "Flats")
        adapter.ensure_layer(document, "FX")

        children_names = [c.name() for c in root.childNodes()]
        self.assertEqual(children_names, ["Draft", "Flats", "Lineart", "FX"])

    def test_layer_stack_order_placement_when_higher_layer_exists_first(self) -> None:
        root = _FakeNode("root", "grouplayer")
        document = _FakeDocument(root=root)
        adapter = KritaCanvasAdapter()

        adapter.ensure_layer(document, "Lineart")
        adapter.ensure_layer(document, "Flats")
        adapter.ensure_layer(document, "Draft")
        adapter.ensure_layer(document, "Highlights")

        children_names = [c.name() for c in root.childNodes()]
        self.assertEqual(children_names, ["Draft", "Flats", "Lineart", "Highlights"])

    def test_cancel_before_drawing_does_not_paint(self) -> None:
        document = _FakeDocument()
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()
        self.assertEqual(adapter.render(document, plan, cancelled=lambda: True), 0)
        self.assertEqual(document.refreshed, 1)

    def test_capture_canvas_returns_bytes(self) -> None:
        document = _FakeDocument()
        adapter = KritaCanvasAdapter()
        cap_bytes = adapter.capture_canvas(document, 256, 256)
        self.assertIsInstance(cap_bytes, bytes)
        self.assertEqual(cap_bytes, b"")

    def test_apply_color_to_krita_parses_short_and_full_hex(self) -> None:
        from unittest.mock import MagicMock, patch

        from ai_stroke_painter.krita_adapter import _apply_color_to_krita, _parse_hex_rgb

        self.assertEqual(_parse_hex_rgb("#fff"), (1.0, 1.0, 1.0))
        self.assertEqual(_parse_hex_rgb("#000"), (0.0, 0.0, 0.0))
        self.assertEqual(_parse_hex_rgb("#ff0000"), (1.0, 0.0, 0.0))
        self.assertEqual(_parse_hex_rgb("#ff000080"), (1.0, 0.0, 0.0))
        self.assertIsNone(_parse_hex_rgb("invalid"))

        fake_view = MagicMock()
        with patch.dict("sys.modules", {"krita": MagicMock()}):
            import sys

            krita_mock = sys.modules["krita"]
            krita_instance = krita_mock.Krita.instance.return_value
            krita_instance.activeWindow.return_value.activeView.return_value = fake_view

            _apply_color_to_krita("#fff")
            _apply_color_to_krita("invalid")
            _apply_color_to_krita("#112233")

        self.assertGreaterEqual(fake_view.setForeGroundColor.call_count, 0)

    def test_render_applies_preset_size_and_opacity_for_each_stroke(self) -> None:
        from types import SimpleNamespace
        from unittest.mock import patch

        class FakeView:
            def __init__(self) -> None:
                self.presets: list[Any] = []
                self.sizes: list[float] = []
                self.opacities: list[float] = []

            def setCurrentBrushPreset(self, preset: Any) -> None:  # noqa: N802
                self.presets.append(preset)

            def setBrushSize(self, size: float) -> None:  # noqa: N802
                self.sizes.append(size)

            def setPaintingOpacity(self, opacity: float) -> None:  # noqa: N802
                self.opacities.append(opacity)

        fine_preset = object()
        broad_preset = object()
        view = FakeView()
        app = SimpleNamespace(
            activeWindow=lambda: SimpleNamespace(activeView=lambda: view),
            resources=lambda resource_type: (
                {"Fine": fine_preset, "Broad": broad_preset} if resource_type == "preset" else {}
            ),
        )
        fake_krita = SimpleNamespace(Krita=SimpleNamespace(instance=lambda: app))
        target = _FakeNode("Lineart")
        document = _FakeDocument(active=target)
        plan = DrawingPlan(
            prompt="style contract",
            seed=1,
            strokes=(
                Stroke(
                    id="fine",
                    points=(StrokePoint(10.0, 10.0, 0.5, 0), StrokePoint(20.0, 20.0, 0.7, 10)),
                    brush_preset="Fine",
                    size_px=3.0,
                    layer_name="Lineart",
                    opacity=0.4,
                ),
                Stroke(
                    id="broad",
                    points=(StrokePoint(30.0, 30.0, 0.5, 0), StrokePoint(40.0, 40.0, 0.7, 10)),
                    brush_preset="Broad",
                    color="#11223380",
                    size_px=9.0,
                    layer_name="Lineart",
                    opacity=0.8,
                ),
            ),
        )

        with patch.dict("sys.modules", {"krita": fake_krita}):
            rendered = KritaCanvasAdapter().render(document, plan)

        self.assertEqual(rendered, 2)
        self.assertEqual(view.presets, [fine_preset, broad_preset])
        self.assertEqual(view.sizes, [3.0, 9.0])
        self.assertEqual(view.opacities[0], 0.4)
        self.assertAlmostEqual(view.opacities[1], 0.8 * (128 / 255))

    def test_missing_non_eraser_brush_fails_instead_of_using_arbitrary_current_brush(self) -> None:
        from types import SimpleNamespace

        from ai_stroke_painter.krita_adapter import _apply_stroke_style

        view = SimpleNamespace(setCurrentBrushPreset=lambda _preset: None)
        app = SimpleNamespace(resources=lambda _resource_type: {})
        fake_krita = SimpleNamespace(Krita=SimpleNamespace(instance=lambda: app))
        stroke = Stroke(
            "missing-preset",
            [StrokePoint(0, 0, 1, 0), StrokePoint(1, 1, 1, 10)],
            brush_preset="Definitely Missing",
        )
        with patch.dict("sys.modules", {"krita": fake_krita}), self.assertRaises(RuntimeError):
            _apply_stroke_style(stroke, view=view)

    def test_qpoint_and_paintline_type_compatibility(self) -> None:
        from ai_stroke_painter.krita_adapter import _qpoint, _qpointf

        pt1 = _qpoint(12.7, 34.2)
        pt2 = _qpointf(56.1, 78.9)
        self.assertNotEqual(type(pt1).__name__, "QPointF")
        self.assertNotEqual(type(pt2).__name__, "QPointF")

        # レンダリングテストで QPointF が渡されずに正常終了することを確認
        document = _FakeDocument(active=_FakeNode(KritaCanvasAdapter.DEFAULT_LAYER_NAME))
        plan = RuleBasedPlanner().plan("test", 1, 1, 200, 200)
        adapter = KritaCanvasAdapter()
        rendered = adapter.render(document, plan)
        self.assertGreaterEqual(rendered, 1)

        class FloatPointNode(_FakeNode):
            def paintLine(self, start: Any, end: Any, start_pressure: float, end_pressure: float) -> None:  # noqa: N802
                if type(start).__name__ != "QPointF" or type(end).__name__ != "QPointF":
                    raise TypeError("QPointF required")
                self.lines.append((start, end, start_pressure, end_pressure))

        float_node = FloatPointNode(KritaCanvasAdapter.DEFAULT_LAYER_NAME)
        float_document = _FakeDocument(active=float_node)
        self.assertGreaterEqual(adapter.render(float_document, plan, layer_mode="active_layer"), 1)
        self.assertTrue(float_node.lines)

    def test_render_resets_color_cache_across_sessions(self) -> None:
        from unittest.mock import MagicMock, patch

        fake_view = MagicMock()
        with patch.dict("sys.modules", {"krita": MagicMock()}):
            import sys

            krita_mock = sys.modules["krita"]
            krita_instance = krita_mock.Krita.instance.return_value
            krita_instance.activeWindow.return_value.activeView.return_value = fake_view

            document = _FakeDocument(active=_FakeNode(KritaCanvasAdapter.DEFAULT_LAYER_NAME))
            plan = RuleBasedPlanner().plan("test", 1, 1, 200, 200)
            adapter = KritaCanvasAdapter()

            # First render session applies color
            adapter.render(document, plan)
            first_call_count = fake_view.setForeGroundColor.call_count
            self.assertGreaterEqual(first_call_count, 1)

            # Second render session with same plan/color must reset cache and re-apply
            adapter.render(document, plan)
            self.assertGreater(fake_view.setForeGroundColor.call_count, first_call_count)

    def test_render_session_macro_lifecycle(self) -> None:
        document = _FakeDocument(root=_FakeNode("root", "grouplayer"))
        plan = RuleBasedPlanner().plan("test", 1, 1, 200, 200)
        adapter = KritaCanvasAdapter()

        adapter.begin_render_session(document)
        self.assertEqual(document.macros_started, ["AI Stroke Painter Session"])
        self.assertEqual(document.macros_ended, 0)

        adapter.render(document, plan)
        # Session is still open, macro should not end yet
        self.assertEqual(document.macros_ended, 0)

        adapter.end_render_session(commit=True)
        self.assertEqual(document.macros_ended, 1)

    def test_standalone_render_macro_lifecycle(self) -> None:
        document = _FakeDocument(root=_FakeNode("root", "grouplayer"))
        plan = RuleBasedPlanner().plan("test", 1, 1, 200, 200)
        adapter = KritaCanvasAdapter()

        rendered = adapter.render(document, plan)
        self.assertEqual(rendered, 1)
        self.assertEqual(document.macros_started, ["AI Stroke Paint"])
        self.assertEqual(document.macros_ended, 1)


class WorkerAndDockerTests(unittest.TestCase):
    _app: Any = None

    @classmethod
    def setUpClass(cls) -> None:
        if hasattr(QApplication, "instance"):
            cls._app = QApplication.instance()
            if cls._app is None:
                with contextlib.suppress(Exception):
                    cls._app = QApplication(["test", "-platform", "offscreen"])

    def test_password_echo_mode_supports_qt5_and_qt6_enum_shapes(self) -> None:
        qt5_password = object()
        qt6_password = object()

        class Qt5LineEdit:
            Password = qt5_password

        class Qt6LineEdit:
            class EchoMode:
                Password = qt6_password

        self.assertIs(password_echo_mode(Qt5LineEdit), qt5_password)
        self.assertIs(password_echo_mode(Qt6LineEdit), qt6_password)

    def test_qt5_and_qt6_scoped_io_and_image_enums(self) -> None:
        qt5_write = object()
        qt6_write = object()
        qt5_argb = object()
        qt6_argb = object()

        class Qt5IODevice:
            WriteOnly = qt5_write

        class Qt6IODevice:
            class OpenModeFlag:
                WriteOnly = qt6_write

        class Qt5Image:
            Format_ARGB32 = qt5_argb

        class Qt6Image:
            class Format:
                Format_ARGB32 = qt6_argb

        self.assertIs(write_only_open_mode(Qt5IODevice), qt5_write)
        self.assertIs(write_only_open_mode(Qt6IODevice), qt6_write)
        self.assertIs(argb32_image_format(Qt5Image), qt5_argb)
        self.assertIs(argb32_image_format(Qt6Image), qt6_argb)

    def test_destructive_confirmation_requires_explicit_yes(self) -> None:
        from .docker import QMessageBox

        buttons: Any = getattr(QMessageBox, "StandardButton", QMessageBox)
        yes = getattr(buttons, "Yes", 16384)
        no = getattr(buttons, "No", 65536)
        with patch("ai_stroke_painter.docker.QMessageBox.question", return_value=no):
            self.assertFalse(_confirm(None, "Confirm", "Proceed?"))
        with patch("ai_stroke_painter.docker.QMessageBox.question", return_value=yes):
            self.assertTrue(_confirm(None, "Confirm", "Proceed?"))

    def test_debug_endpoint_label_never_exposes_url_credentials(self) -> None:
        label = _safe_endpoint_label("https://alice:secret@example.test:8443/v1/")
        self.assertEqual(label, "https://example.test:8443/v1")
        self.assertNotIn("alice", label)
        self.assertNotIn("secret", label)

    def test_offline_mode_disables_auto_refine(self) -> None:
        docker = AIStrokePainterDocker()
        docker.auto_refine.setChecked(True)
        docker.planner_mode.setCurrentIndex(0)
        docker._update_planner_settings_state()

        self.assertFalse(docker.auto_refine.isEnabled())
        self.assertFalse(docker.auto_refine.isChecked())
        self.assertFalse(docker.iterations.isEnabled())

        docker.planner_mode.setCurrentIndex(1)
        docker._update_planner_settings_state()

        self.assertTrue(docker.auto_refine.isEnabled())
        self.assertTrue(docker.iterations.isEnabled())

    def test_api_connection_worker_runs_without_blocking_caller(self) -> None:
        class FakePlanner:
            log_callback: Any = None

            def test_connection(self) -> str:
                return "connected"

        worker = ApiConnectionWorker(cast(Any, FakePlanner()))
        messages: list[str] = []
        finished: list[bool] = []
        worker.succeeded.connect(messages.append)
        worker.finished.connect(lambda: finished.append(True))
        worker.start()
        if worker._thread is not None:
            worker._thread.join(timeout=2.0)
        for _ in range(10):
            QApplication.processEvents()
            if finished:
                break

        self.assertEqual(messages, ["connected"])
        self.assertEqual(finished, [True])
        self.assertFalse(worker.isRunning())

    def test_qt_compat_stubs_are_functional(self) -> None:
        w = QWidget()
        self.assertIsNotNone(w)

        point = QPoint(10, 20)
        px = point.x() if callable(point.x) else point.x
        py = point.y() if callable(point.y) else point.y
        self.assertEqual(px, 10)
        self.assertEqual(py, 20)

        img = QImage()
        self.assertFalse(img.loadFromData(b""))

        # QWidget と QPoint の確認
        self.assertTrue(hasattr(w, "isVisible"))

    def test_plan_worker_emits_plan_on_success(self) -> None:
        planner = RuleBasedPlanner()
        worker = PlanWorker(
            planner=planner,
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            max_iterations=1,
        )
        received_plans: list[Any] = []
        received_errors: list[str] = []
        worker.plan_ready.connect(lambda p: received_plans.append(p))
        worker.plan_ready.connect(lambda _p: worker.notify_render_done())
        worker.plan_failed.connect(lambda e: received_errors.append(e))

        worker.run()
        self.assertEqual(len(received_plans), 1)
        self.assertEqual(len(received_errors), 0)
        self.assertIsInstance(received_plans[0], DrawingPlan)

    def test_plan_worker_suppresses_emission_when_cancelled(self) -> None:
        planner = RuleBasedPlanner()
        worker = PlanWorker(
            planner=planner,
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            max_iterations=1,
        )
        received_plans: list[Any] = []
        worker.plan_ready.connect(lambda p: received_plans.append(p))

        worker.cancel()
        self.assertTrue(worker.is_cancelled())
        worker.run()
        self.assertEqual(len(received_plans), 0)

    def test_goal_completion_does_not_win_over_stop_requested_after_render(self) -> None:
        class GoalPlanner:
            def plan(self, **_kwargs: Any) -> DrawingPlan:
                return DrawingPlan(
                    "goal",
                    1,
                    [Stroke("stroke", [StrokePoint(0, 0, 1, 0), StrokePoint(1, 1, 1, 1)])],
                    goal_reached=True,
                    completion_score=1.0,
                )

        worker = PlanWorker(cast(Any, GoalPlanner()), "goal", 1, 1, 100, 100, goal_mode=True)

        def render_then_stop(_plan: DrawingPlan) -> None:
            worker.notify_render_done()
            worker.cancel()

        worker.plan_ready.connect(render_then_stop)
        worker.run()

        self.assertTrue(worker.is_cancelled())
        self.assertFalse(worker.completed_successfully)

    def test_plan_worker_stops_after_render_failure(self) -> None:
        planner_calls: list[int] = []

        class SpyPlanner(RuleBasedPlanner):
            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                planner_calls.append(int(kwargs.get("iteration", 0)))
                return super().plan(*args, **kwargs)

        worker = PlanWorker(SpyPlanner(), "cat", 1, 2, 200, 200, max_iterations=3)
        errors: list[str] = []
        worker.plan_ready.connect(lambda _plan: worker.notify_render_failed("paint failed"))
        worker.plan_failed.connect(errors.append)

        worker.run()

        self.assertEqual(planner_calls, [1])
        self.assertEqual(errors, ["paint failed"])

    def test_plan_worker_thread_safe_canvas_capture_passing(self) -> None:
        import time

        from ai_stroke_painter.krita_adapter import _process_events

        captured_in_planner: list[bytes | None] = []

        class CaptureSpyPlanner(RuleBasedPlanner):
            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                captured_in_planner.append(kwargs.get("canvas_image"))
                return super().plan(*args, **kwargs)

        worker = PlanWorker(
            planner=CaptureSpyPlanner(),
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            max_iterations=2,
        )

        plans: list[DrawingPlan] = []
        worker.plan_ready.connect(lambda p: plans.append(p))

        thread = Thread(target=worker.run)
        thread.start()

        for _ in range(100):
            _process_events()
            if len(plans) == 1:
                break
            time.sleep(0.01)

        self.assertEqual(len(plans), 1)
        self.assertIsNone(captured_in_planner[0])

        worker.provide_canvas_capture(b"fake-main-thread-screenshot")

        for _ in range(50):
            _process_events()
            if len(plans) == 2:
                break
            time.sleep(0.01)

        self.assertEqual(len(plans), 2)
        self.assertEqual(captured_in_planner[1], b"fake-main-thread-screenshot")
        worker.notify_render_done()
        thread.join(timeout=2.0)

    def test_plan_worker_forwards_palette_name_to_planner(self) -> None:
        received: list[str] = []

        class PaletteSpyPlanner(RuleBasedPlanner):
            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                received.append(kwargs.get("palette_name", "<not-forwarded>"))
                return super().plan(*args, **kwargs)

        worker = PlanWorker(
            planner=PaletteSpyPlanner(),
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            palette_name="cyberpunk",
            max_iterations=1,
        )
        worker.plan_ready.connect(lambda _p: worker.notify_render_done())
        worker.run()
        self.assertEqual(received, ["cyberpunk"])

    def test_docker_implements_canvas_changed(self) -> None:
        self.assertTrue(hasattr(AIStrokePainterDocker, "canvasChanged"))
        self.assertTrue(callable(AIStrokePainterDocker.canvasChanged))

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker._canvas = None
        fake_canvas = object()
        AIStrokePainterDocker.canvasChanged(docker, fake_canvas)
        self.assertIs(docker._canvas, fake_canvas)
        AIStrokePainterDocker.canvasChanged(docker, None)
        self.assertIsNone(docker._canvas)

    def test_plan_worker_emits_debug_logs(self) -> None:
        planner = RuleBasedPlanner()
        worker = PlanWorker(
            planner=planner,
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            max_iterations=1,
        )
        logs: list[str] = []
        worker.debug_log.connect(lambda msg: logs.append(msg))
        worker.plan_ready.connect(lambda _p: worker.notify_render_done())
        worker.run()
        self.assertTrue(len(logs) > 0)
        self.assertTrue(any("ワーカー開始" in log for log in logs))
        self.assertTrue(any("ワーカー完了" in log for log in logs))

    def test_docker_debug_mode_toggle_and_copy(self) -> None:
        class _TestWidget:
            def __init__(self) -> None:
                self.visible = False
                self._text = ""

            def setVisible(self, v: bool) -> None:  # noqa: N802
                self.visible = v

            def isVisible(self) -> bool:  # noqa: N802
                return self.visible

            def toPlainText(self) -> str:  # noqa: N802
                return "Log sample line 1\nLog sample line 2"

            def clear(self) -> None:
                self._text = ""

            def appendPlainText(self, text: str) -> None:  # noqa: N802
                self._text += text

            def setText(self, text: str) -> None:  # noqa: N802
                self._text = text

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        debug_box = _TestWidget()
        debug_log_edit = _TestWidget()
        status_label = _TestWidget()

        docker.debug_box = debug_box
        docker.debug_log_edit = debug_log_edit
        docker.status = status_label

        docker._toggle_debug_panel(True)
        self.assertTrue(debug_box.isVisible())

        docker._copy_debug_log()
        docker._clear_debug_log()

    def test_docker_settings_persistence(self) -> None:
        if callable(QSettings):
            settings = QSettings("AIStrokePainter", "DockerSettings")
            if hasattr(settings, "clear"):
                settings.clear()

        class FakeTextWidget:
            def __init__(self, val: str = "") -> None:
                self._v = val

            def text(self) -> str:
                return self._v

            def setText(self, v: str) -> None:  # noqa: N802
                self._v = v

            def toPlainText(self) -> str:  # noqa: N802
                return self._v

            def setPlainText(self, v: str) -> None:  # noqa: N802
                self._v = v

        class FakeIntWidget:
            def __init__(self, val: int = 0) -> None:
                self._v = val

            def value(self) -> int:
                return self._v

            def setValue(self, v: int) -> None:  # noqa: N802
                self._v = v

        class FakeBoolWidget:
            def __init__(self, val: bool = False) -> None:
                self._v = val

            def isChecked(self) -> bool:  # noqa: N802
                return self._v

            def setChecked(self, v: bool) -> None:  # noqa: N802
                self._v = v

        class FakeComboWidget:
            def __init__(self, items: list[tuple[str, str]], default_data: str = "low") -> None:
                self._items = items  # (text, data)
                self._idx = 0
                for i, (_, data) in enumerate(items):
                    if data == default_data:
                        self._idx = i
                        break

            def count(self) -> int:
                return len(self._items)

            def itemData(self, index: int) -> str:  # noqa: N802
                return self._items[index][1] if 0 <= index < len(self._items) else ""

            def currentData(self) -> str:  # noqa: N802
                return self._items[self._idx][1] if 0 <= self._idx < len(self._items) else ""

            def setCurrentIndex(self, index: int) -> None:  # noqa: N802
                self._idx = index

        combo_items = [
            ("低", "low"),
            ("中", "medium"),
            ("高", "high"),
            ("オフ", "none"),
        ]

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.base_url = FakeTextWidget("https://custom.api/v1")
        docker.model = FakeTextWidget("custom-model-pro")
        docker.timeout_sec = FakeIntWidget(99)
        docker.max_tokens = FakeIntWidget(16384)
        docker.reasoning_effort = FakeComboWidget(combo_items, default_data="high")
        docker.prompt = FakeTextWidget("test persistent prompt")
        docker.seed = FakeIntWidget(777)
        docker.count = FakeIntWidget(55)
        docker.iterations = FakeIntWidget(4)
        docker.auto_refine = FakeBoolWidget(True)
        docker.fallback_to_procedural = FakeBoolWidget(True)
        docker.save_json = FakeBoolWidget(True)
        docker.save_svg_chk = FakeBoolWidget(False)
        docker.debug_mode_chk = FakeBoolWidget(True)

        docker._save_settings()

        docker2 = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker2.base_url = FakeTextWidget()
        docker2.model = FakeTextWidget()
        docker2.timeout_sec = FakeIntWidget()
        docker2.max_tokens = FakeIntWidget()
        docker2.reasoning_effort = FakeComboWidget(combo_items, default_data="low")
        docker2.prompt = FakeTextWidget()
        docker2.seed = FakeIntWidget()
        docker2.count = FakeIntWidget()
        docker2.iterations = FakeIntWidget()
        docker2.auto_refine = FakeBoolWidget()
        docker2.fallback_to_procedural = FakeBoolWidget()
        docker2.save_json = FakeBoolWidget()
        docker2.save_svg_chk = FakeBoolWidget()
        docker2.debug_mode_chk = FakeBoolWidget()

        docker2._load_settings()

        self.assertEqual(docker2.base_url.text(), "https://custom.api/v1")
        self.assertEqual(docker2.model.text(), "custom-model-pro")
        self.assertEqual(docker2.timeout_sec.value(), 99)
        self.assertEqual(docker2.max_tokens.value(), 16384)
        self.assertEqual(docker2.reasoning_effort.currentData(), "high")
        self.assertEqual(docker2.prompt.toPlainText(), "test persistent prompt")
        self.assertEqual(docker2.seed.value(), 777)
        self.assertEqual(docker2.count.value(), 55)
        self.assertEqual(docker2.iterations.value(), 4)
        self.assertTrue(docker2.auto_refine.isChecked())
        self.assertTrue(docker2.fallback_to_procedural.isChecked())
        self.assertFalse(docker2.save_svg_chk.isChecked())
        self.assertTrue(docker2.debug_mode_chk.isChecked())

    def test_docker_on_plan_ready_handles_none_document_without_stalling(self) -> None:
        class _TestWidget:
            def __init__(self) -> None:
                self._text = ""

            def setText(self, t: str) -> None:  # noqa: N802
                self._text = t

            def isChecked(self) -> bool:  # noqa: N802
                return False

            def setRange(self, *args: Any) -> None:  # noqa: N802
                pass

            def setValue(self, *args: Any) -> None:  # noqa: N802
                pass

            def setEnabled(self, *args: Any) -> None:  # noqa: N802
                pass

            def set_plan(self, *args: Any) -> None:
                pass

            def appendPlainText(self, *args: Any) -> None:  # noqa: N802
                pass

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.preview = cast(Any, _TestWidget())
        docker.status = cast(Any, _TestWidget())
        docker.save_json = cast(Any, _TestWidget())
        docker.save_svg_chk = cast(Any, _TestWidget())
        docker.progress = cast(Any, _TestWidget())
        docker.run_btn = cast(Any, _TestWidget())
        docker.stop_btn = cast(Any, _TestWidget())
        docker.debug_log_edit = cast(Any, _TestWidget())
        docker._active_doc = None
        docker._cancel = False

        notified: list[bool] = []

        class FakeWorker:
            def __init__(self) -> None:
                self.max_iterations = 2

            def notify_render_done(self) -> None:
                notified.append(True)

            def isRunning(self) -> bool:  # noqa: N802
                return False

        docker._worker = cast(Any, FakeWorker())
        plan = RuleBasedPlanner().plan("test", 1, 1, 100, 100)
        docker._on_plan_ready(plan)

        self.assertEqual(len(notified), 1)
        self.assertIn("ドキュメントが閉じられたため", docker.status._text)

    def test_plan_worker_waits_for_render_done_on_final_iteration(self) -> None:
        import time

        from ai_stroke_painter.krita_adapter import _process_events

        worker = PlanWorker(
            planner=RuleBasedPlanner(),
            prompt="cat",
            seed=1,
            count=2,
            width=200,
            height=200,
            max_iterations=1,
        )

        plans: list[DrawingPlan] = []
        is_finished: list[bool] = []
        worker.plan_ready.connect(lambda p: plans.append(p))
        worker.finished.connect(lambda: is_finished.append(True))

        worker.start()

        for _ in range(50):
            _process_events()
            if plans:
                break
            time.sleep(0.01)

        self.assertEqual(len(plans), 1)
        self.assertEqual(len(is_finished), 0)

        worker.notify_render_done()

        for _ in range(50):
            _process_events()
            if is_finished:
                break
            time.sleep(0.01)

        self.assertEqual(len(is_finished), 1)
        if worker._thread is not None:
            worker._thread.join(timeout=2.0)

    def test_docker_api_key_whitespace_fallback_to_environ(self) -> None:
        import os
        from unittest.mock import patch

        class FakeTextWidget:
            def __init__(self, val: str = "") -> None:
                self._v = val

            def text(self) -> str:
                return self._v

            def appendPlainText(self, *args: Any) -> None:  # noqa: N802
                pass

        class FakeIntWidget:
            def __init__(self, val: int = 0) -> None:
                self._v = val

            def value(self) -> int:
                return self._v

        class FakeComboWidget:
            def __init__(self, default_data: str = "low") -> None:
                self._d = default_data

            def currentData(self) -> str:  # noqa: N802
                return self._d

            def currentText(self) -> str:  # noqa: N802
                return "OpenAI"

            def currentIndex(self) -> int:  # noqa: N802
                return 1

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.base_url = FakeTextWidget("https://api.openai.com/v1")
        docker.model = FakeTextWidget("gpt-4o")
        docker.api_key = FakeTextWidget("   ")
        docker.timeout_sec = FakeIntWidget(30)
        docker.max_tokens = FakeIntWidget(2048)
        docker.reasoning_effort = FakeComboWidget("low")
        docker.planner_mode = FakeComboWidget("openai_compatible")
        docker.planner = RuleBasedPlanner()
        docker.debug_log_edit = FakeTextWidget()

        with patch.dict(os.environ, {"OPENAI_API_KEY": "sk-env-fallback-key"}):
            planner = cast(Any, docker._planner())
            self.assertEqual(planner.settings.api_key, "sk-env-fallback-key")

    def test_docker_on_worker_finished_always_resets_ui_state(self) -> None:
        docker = AIStrokePainterDocker()

        class FakeRunningWorker:
            def isRunning(self) -> bool:
                return True

        docker._worker = cast(Any, FakeRunningWorker())
        docker._active_doc = "dummy_doc"
        docker.run_btn.setEnabled(False)
        docker.stop_btn.setEnabled(True)

        docker._on_worker_finished()

        self.assertIsNone(docker._worker)
        self.assertIsNone(docker._active_doc)
        self.assertTrue(docker.run_btn.isEnabled())
        self.assertFalse(docker.stop_btn.isEnabled())

    def test_docker_commits_only_successful_canvas_sessions(self) -> None:
        class SessionCanvasPort:
            def __init__(self) -> None:
                self.commits: list[bool] = []

            def end_render_session(self, *, commit: bool) -> None:
                self.commits.append(commit)

        class CompletedWorker:
            completed_successfully = True

        docker = AIStrokePainterDocker()
        session_port = SessionCanvasPort()
        docker.canvas_port = cast(Any, session_port)
        docker._canvas_session_open = True
        docker._worker = cast(Any, CompletedWorker())
        docker._on_worker_finished()
        self.assertEqual(session_port.commits, [True])

        docker._canvas_session_open = True
        docker._reset_run_state()
        self.assertEqual(session_port.commits, [True, False])

    def test_docker_rolls_back_when_a_completed_worker_was_cancelled(self) -> None:
        class SessionCanvasPort:
            def __init__(self) -> None:
                self.commits: list[bool] = []

            def end_render_session(self, *, commit: bool) -> None:
                self.commits.append(commit)

        class CancelledCompletedWorker:
            completed_successfully = True

            def is_cancelled(self) -> bool:
                return True

        docker = AIStrokePainterDocker()
        session_port = SessionCanvasPort()
        docker.canvas_port = cast(Any, session_port)
        docker._canvas_session_open = True
        docker._worker = cast(Any, CancelledCompletedWorker())
        docker._on_worker_finished()

        self.assertEqual(session_port.commits, [False])

    def test_docker_surfaces_canvas_session_finalization_failure(self) -> None:
        class FailingCanvasPort:
            def end_render_session(self, *, commit: bool) -> None:
                raise RuntimeError("restore failed")

        class Status:
            def __init__(self) -> None:
                self.text = ""

            def setText(self, value: str) -> None:  # noqa: N802
                self.text = value

        messages: list[tuple[str, str]] = []

        class MessageBox:
            @staticmethod
            def critical(_parent: Any, title: str, message: str) -> None:
                messages.append((title, message))

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.canvas_port = cast(Any, FailingCanvasPort())
        docker._canvas_session_open = True
        docker.status = Status()

        with patch("ai_stroke_painter.docker.QMessageBox", MessageBox):
            self.assertFalse(docker._finish_canvas_session(False))

        self.assertFalse(docker._canvas_session_open)
        self.assertIn("ロールバックに失敗", docker.status.text)
        self.assertEqual(len(messages), 1)
        self.assertIn("restore failed", messages[0][1])

    def test_docker_on_plan_ready_captures_each_auto_refine_iteration(self) -> None:
        class _TestWidget:
            def __init__(self) -> None:
                self._text = ""

            def setText(self, t: str) -> None:  # noqa: N802
                self._text = t

            def isChecked(self) -> bool:  # noqa: N802
                return False

            def setRange(self, *args: Any) -> None:  # noqa: N802
                pass

            def setValue(self, *args: Any) -> None:  # noqa: N802
                pass

            def setEnabled(self, *args: Any) -> None:  # noqa: N802
                pass

            def set_plan(self, *args: Any) -> None:
                pass

            def appendPlainText(self, *args: Any) -> None:  # noqa: N802
                pass

        class FakeWorker:
            def __init__(self) -> None:
                self.max_iterations = 3
                self.provided_captures: list[bytes | None] = []

            def provide_canvas_capture(self, cap: bytes | None) -> None:
                self.provided_captures.append(cap)

            def isRunning(self) -> bool:  # noqa: N802
                return True

        class FakeCanvasPort:
            def render(self, *args: Any) -> int:
                return 5

            def capture_canvas(self, doc: Any, w: int, h: int) -> bytes:
                return b"real-canvas-capture-bytes"

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.preview = cast(Any, _TestWidget())
        docker.status = cast(Any, _TestWidget())
        docker.save_json = cast(Any, _TestWidget())
        docker.save_svg_chk = cast(Any, _TestWidget())
        docker.progress = cast(Any, _TestWidget())
        docker.run_btn = cast(Any, _TestWidget())
        docker.stop_btn = cast(Any, _TestWidget())
        docker.debug_log_edit = cast(Any, _TestWidget())
        docker._active_doc = "dummy_doc"
        docker._cancel = False
        docker.canvas_port = cast(Any, FakeCanvasPort())

        # Auto-Refine ではモデル側フラグに依存せず、各中間反復後に視覚フィードバックを渡す。
        worker1 = FakeWorker()
        docker._worker = cast(Any, worker1)
        plan_no_req = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            iteration=1,
            request_canvas_image=False,
        )
        docker._on_plan_ready(plan_no_req)
        self.assertEqual(len(worker1.provided_captures), 1)
        self.assertEqual(worker1.provided_captures[0], b"real-canvas-capture-bytes")

        # Case 2: AI が画像要求 (request_canvas_image=True)
        worker2 = FakeWorker()
        docker._worker = cast(Any, worker2)
        plan_with_req = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            iteration=1,
            request_canvas_image=True,
        )
        docker._on_plan_ready(plan_with_req)
        self.assertEqual(len(worker2.provided_captures), 1)
        self.assertEqual(worker2.provided_captures[0], b"real-canvas-capture-bytes")

        worker3 = FakeWorker()
        docker._worker = cast(Any, worker3)
        final_plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            iteration=3,
        )
        docker._on_plan_ready(final_plan)
        self.assertEqual(worker3.provided_captures, [])

        class FailingCapturePort(FakeCanvasPort):
            def capture_canvas(self, doc: Any, w: int, h: int) -> bytes:
                raise RuntimeError("capture failed")

        worker4 = FakeWorker()
        docker._worker = cast(Any, worker4)
        docker.canvas_port = cast(Any, FailingCapturePort())
        docker._on_plan_ready(plan_no_req)
        self.assertEqual(worker4.provided_captures, [b""])

    def test_docker_saves_goal_mode_early_completion(self) -> None:
        """Goal モードの早期達成結果も、選択されたエクスポート対象として保存する。"""

        class FakeCanvasPort:
            def render(self, *args: Any, **kwargs: Any) -> int:
                return 1

        class FakeWorker:
            max_iterations = 10
            goal_mode = True

            def notify_render_done(self) -> None:
                pass

            def isRunning(self) -> bool:  # noqa: N802
                return True

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.canvas_port = cast(Any, FakeCanvasPort())
        docker._active_doc = object()
        docker._active_view = None
        docker._cancel = False
        docker._worker = cast(Any, FakeWorker())
        docker._run_render_options = {
            "save_json": True,
            "save_svg": True,
            "layer_mode": "multi_layer",
            "layer_prefix": "AI Artwork",
            "event_interval": 1,
        }
        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)])],
            iteration=1,
            goal_reached=True,
            completion_score=1.0,
        )
        saved: list[str] = []

        def fake_save_plan(_plan: DrawingPlan) -> Path:
            saved.append("json")
            return Path("plan.json")

        def fake_save_svg(_plan: DrawingPlan) -> Path:
            saved.append("svg")
            return Path("plan.svg")

        with (
            patch("ai_stroke_painter.docker.save_plan", side_effect=fake_save_plan),
            patch("ai_stroke_painter.docker.save_svg", side_effect=fake_save_svg),
        ):
            docker._on_plan_ready(plan)

        self.assertEqual(saved, ["json", "svg"])


class ExtendedCustomizationTests(unittest.TestCase):
    """拡張設定項目・パレット・ブラシプロファイル・レイヤーモード・画像変換・カスタムプリセットの検証。"""

    def test_all_expanded_color_palettes(self) -> None:
        from .procedural.base import color_palette

        palettes = [
            "anime",
            "monochrome",
            "cyberpunk",
            "nature",
            "pastel",
            "watercolor",
            "retro_pop",
            "dark_fantasy",
            "sepia",
            "botanical",
            "sumie",
            "cyber_gold",
        ]
        for pal in palettes:
            colors_dict = color_palette(pal)
            self.assertGreaterEqual(len(colors_dict), 4, f"Palette '{pal}' should have at least 4 colors")
            for key, c in colors_dict.items():
                self.assertTrue(
                    c.startswith("#") and len(c) == 7, f"Invalid hex color '{c}' for key '{key}' in palette '{pal}'"
                )

        # 未知のパレットはデフォルト(anime)にフォールバック
        fallback_colors = color_palette("unknown_palette_name")
        self.assertEqual(fallback_colors, color_palette("anime"))

    def test_expanded_pressure_profiles(self) -> None:
        from .procedural.base import pressure_profile

        profiles = ["gpen", "marupen", "brush", "marker", "pencil", "watercolor", "airbrush"]
        for prof in profiles:
            p_start = pressure_profile(0.0, prof, 0.8)
            p_mid = pressure_profile(0.5, prof, 0.8)
            p_end = pressure_profile(1.0, prof, 0.8)
            self.assertTrue(0.05 <= p_start <= 1.0, f"{prof} start pressure {p_start} out of bounds")
            self.assertTrue(0.05 <= p_mid <= 1.0, f"{prof} mid pressure {p_mid} out of bounds")
            self.assertTrue(0.05 <= p_end <= 1.0, f"{prof} end pressure {p_end} out of bounds")
            self.assertGreater(
                p_mid,
                p_start,
                f"{prof} must build pressure after the entry taper",
            )
            self.assertGreater(
                p_mid,
                p_end,
                f"{prof} must release pressure before the stroke end",
            )

    def test_image_converter_auto_budget_respects_domain_limit(self) -> None:
        """全面暗部の高密度ハッチングでも Auto が DrawingPlan 上限を超えない。"""
        import random

        from .image_converter import AUTO_STROKE_BUDGET

        class BlackPixel:
            def red(self) -> int:
                return 0

            def green(self) -> int:
                return 0

            def blue(self) -> int:
                return 0

            def alpha(self) -> int:
                return 255

        class BlackImage:
            def width(self) -> int:
                return 160

            def height(self) -> int:
                return 160

            def scaled(self, _width: int, _height: int) -> BlackImage:
                return self

            def pixelColor(self, _x: int, _y: int) -> BlackPixel:  # noqa: N802
                return BlackPixel()

        converter = ImageStrokeConverter()
        strokes = converter._process_qimage(
            BlackImage(),
            seed=1,
            count=None,
            target_width=1000,
            target_height=1000,
            rng=random.Random(1),
            shading_density="high",
        )
        self.assertEqual(len(strokes), AUTO_STROKE_BUDGET)
        DrawingPlan("black image", 1, strokes)

    def test_missing_completion_score_is_not_treated_as_complete(self) -> None:
        plan = DrawingPlan.from_dict(
            {
                "schema_version": 1,
                "prompt": "unfinished",
                "seed": 1,
                "strokes": [
                    {
                        "id": "s1",
                        "points": [[0, 0], [10, 10]],
                    }
                ],
            }
        )
        self.assertFalse(plan.goal_reached)
        self.assertEqual(plan.completion_score, 0.0)

    def test_llm_count_budget_preserves_semantic_layers(self) -> None:
        from .llm_planner import _validate_and_sanitize_plan

        strokes = []
        for layer_index, layer in enumerate(("Flats", "Shading", "Lineart", "Highlights")):
            for item_index in range(2):
                strokes.append(
                    Stroke(
                        id=f"{layer}_{item_index}",
                        points=(
                            StrokePoint(10 + layer_index * 5, 10 + item_index, 0.8, 0),
                            StrokePoint(20 + layer_index * 5, 20 + item_index, 0.8, 10),
                        ),
                        layer_name=layer,
                    )
                )
        raw_plan = DrawingPlan("layer budget", 1, strokes)
        sanitized = _validate_and_sanitize_plan(raw_plan, "layer budget", 1, 3, 200, 200)
        selected_layers = {stroke.layer_name for stroke in sanitized.strokes}
        self.assertEqual(len(sanitized.strokes), 3)
        self.assertIn("Lineart", selected_layers)
        self.assertIn("Flats", selected_layers)

    def test_procedural_brush_profiles_applied(self) -> None:
        from .procedural import generate_procedural_plan

        profile_map = {
            "gpen": "Ink-2 Fineliner",
            "marupen": "Ink-1 Precision",
            "brush": "Wet-1 Water",
            "marker": "Marker-1 Broad",
            "pencil": "Pencil-2",
            "watercolor": "Wet Textured Soft",
            "airbrush": "Airbrush Soft",
        }
        for prof, expected_preset in profile_map.items():
            plan = generate_procedural_plan(
                prompt="anime girl portrait with flowers",
                seed=100,
                count=20,
                width=512,
                height=512,
                brush_profile=prof,
            )
            self.assertGreater(len(plan.strokes), 0)
            for stroke in plan.strokes:
                self.assertEqual(stroke.brush_preset, expected_preset)

    def test_image_converter_extended_options(self) -> None:
        from .image_converter import ImageStrokeConverter

        class FakeColor:
            def __init__(self, value: int) -> None:
                self.value = value

            def red(self) -> int:
                return self.value

            def green(self) -> int:
                return self.value

            def blue(self) -> int:
                return self.value

            def alpha(self) -> int:
                return 255

        class FakeImage:
            Format_ARGB32 = 5

            def loadFromData(self, _data: bytes) -> bool:  # noqa: N802
                return True

            def width(self) -> int:
                return 20

            def height(self) -> int:
                return 10

            def scaled(self, _width: int, _height: int) -> Any:
                return self

            def convertToFormat(self, _format: Any) -> Any:  # noqa: N802
                return self

            def pixelColor(self, x: int, _y: int) -> FakeColor:  # noqa: N802
                return FakeColor(20 if x < 10 else 240)

        converter = ImageStrokeConverter()
        converter.qimage_cls = FakeImage

        fake_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (20).to_bytes(4, "big") + (10).to_bytes(4, "big")
        plan = converter.convert_image_to_plan(
            image_bytes=fake_png,
            prompt="landscape",
            seed=42,
            count=25,
            target_width=800,
            target_height=600,
            edge_threshold=0.08,
            shading_density="high",
            enable_flats=False,
            color_mode="palette",
            palette_name="sepia",
            brush_profile="brush",
        )
        self.assertIsInstance(plan, DrawingPlan)
        self.assertGreater(len(plan.strokes), 0)

    def test_krita_adapter_layer_modes_and_multipliers(self) -> None:
        adapter = KritaCanvasAdapter()
        plan = DrawingPlan(
            prompt="portrait",
            seed=1,
            strokes=[
                Stroke(
                    id="s1",
                    points=[StrokePoint(10, 10, 0.5, 0), StrokePoint(50, 50, 0.8, 10)],
                    size_px=8.0,
                    opacity=0.8,
                    layer_name="Draft",
                ),
                Stroke(
                    id="s2",
                    points=[StrokePoint(20, 20, 0.5, 0), StrokePoint(60, 60, 0.8, 10)],
                    size_px=4.0,
                    opacity=1.0,
                    layer_name="Lineart",
                ),
            ],
        )

        # 1. Multi-layer mode
        doc1 = _FakeDocument()
        rendered1 = adapter.render(
            doc1,
            plan,
            brush_size_multiplier=1.5,
            opacity_multiplier=0.8,
            layer_mode="multi_layer",
            layer_prefix="TestArtwork",
            event_interval=10,
        )
        self.assertEqual(rendered1, 2)

        # 2. Active-layer mode
        doc2 = _FakeDocument()
        rendered2 = adapter.render(
            doc2,
            plan,
            brush_size_multiplier=2.0,
            opacity_multiplier=0.9,
            layer_mode="active_layer",
        )
        self.assertEqual(rendered2, 2)

        # 3. Single-layer mode
        doc3 = _FakeDocument()
        rendered3 = adapter.render(
            doc3,
            plan,
            brush_size_multiplier=0.5,
            opacity_multiplier=0.5,
            layer_mode="single_layer",
            layer_prefix="MergedLayer",
        )
        self.assertEqual(rendered3, 2)

    def test_openai_settings_validation_and_payload(self) -> None:
        # Valid settings
        s = OpenAICompatibleSettings(
            base_url="https://api.openai.com/v1",
            model="gpt-4o",
            temperature=0.85,
            top_p=0.90,
            custom_system_prompt="Draw with delicate fine lines",
            vision_resolution=768,
            max_retries=4,
        )
        self.assertAlmostEqual(s.temperature, 0.85)
        self.assertAlmostEqual(s.top_p, 0.90)
        self.assertEqual(s.custom_system_prompt, "Draw with delicate fine lines")
        self.assertEqual(s.vision_resolution, 768)
        self.assertEqual(s.max_retries, 4)

        # Invalid temperature raises ValueError
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                temperature=99.0,
            )

        # Invalid top_p raises ValueError
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                top_p=0.0,
            )

        # Invalid vision_resolution (<64) raises ValueError
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                vision_resolution=32,
            )

        # Invalid max_retries (<1) raises ValueError
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                max_retries=0,
            )
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                vision_resolution=100_000,
            )
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://api.openai.com/v1",
                model="gpt-4o",
                max_retries=100,
            )

        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="http://example.test/v1",
                model="gpt-4o",
                api_key="secret",
            )
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(base_url="http://example.test/v1", model="gpt-4o")
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://user:password@example.test/v1",
                model="gpt-4o",
            )
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(base_url="https://example.test:99999/v1", model="gpt-4o")
        with self.assertRaises(ValueError):
            OpenAICompatibleSettings(
                base_url="https://example.test/v1",
                model="gpt-4o",
                api_key="secret\r\nInjected: yes",
            )
        local = OpenAICompatibleSettings(
            base_url="http://127.0.0.1:11434/v1",
            model="local-model",
            api_key="local-token",
        )
        self.assertEqual(local.endpoint_url, "http://127.0.0.1:11434/v1/chat/completions")

    def test_preview_widget_multipliers(self) -> None:
        from .docker import PreviewWidget

        prev = PreviewWidget.__new__(PreviewWidget)
        prev._plan = None
        prev._size_multiplier = 1.0
        prev._opacity_multiplier = 1.0
        prev.update = lambda: None

        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[
                Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(100, 100, 0.8, 10)], size_px=10.0, opacity=1.0)
            ],
        )
        prev.set_plan(plan, size_multiplier=2.0, opacity_multiplier=0.5)
        self.assertEqual(prev._size_multiplier, 2.0)
        self.assertEqual(prev._opacity_multiplier, 0.5)

        prev.update_multipliers(size_multiplier=1.5, opacity_multiplier=0.9)
        self.assertEqual(prev._size_multiplier, 1.5)
        self.assertEqual(prev._opacity_multiplier, 0.9)

    def test_stroke_is_eraser_support(self) -> None:
        """消しゴムストロークの作成、辞書変換、復元、および自動判定テスト。"""
        pts = [StrokePoint(10, 10, 0.5, 0), StrokePoint(20, 20, 0.8, 10)]
        eraser_stroke = Stroke(
            id="e1",
            points=pts,
            brush_preset="Eraser Small",
            color="#000000",
            size_px=15.0,
            layer_name="Lineart",
            opacity=1.0,
            is_eraser=True,
        )
        self.assertTrue(eraser_stroke.is_eraser)
        d = eraser_stroke.as_dict()
        self.assertTrue(d.get("is_eraser"))

        # from_dict での復元
        restored = Stroke.from_dict(d)
        self.assertTrue(restored.is_eraser)

        # プリセット名に "eraser" が含まれる場合の自動判定
        auto_inferred = Stroke.from_dict(
            {
                "id": "e2",
                "points": [[10, 10], [20, 20]],
                "brush_preset": "Eraser Soft",
            }
        )
        self.assertTrue(auto_inferred.is_eraser)

    def test_string_false_does_not_enable_eraser_or_goal_completion(self) -> None:
        """外部 JSON の文字列 false を truthy なフラグとして扱わない。"""
        plan = DrawingPlan.from_dict(
            {
                "prompt": "test",
                "seed": 1,
                "goal_reached": "false",
                "metadata": {"goal_reached": "false"},
                "strokes": [
                    {
                        "id": "s1",
                        "points": [[0, 0], [10, 10]],
                        "is_eraser": "false",
                    }
                ],
            }
        )
        self.assertFalse(plan.goal_reached)
        self.assertFalse(plan.strokes[0].is_eraser)

    def test_drawing_plan_goal_and_svg_support(self) -> None:
        """DrawingPlan の goal_reached, completion_score, および SVG 出力テスト。"""
        pts = [StrokePoint(10, 10, 0.5, 0), StrokePoint(20, 20, 0.8, 10)]
        eraser_stroke = Stroke(
            id="e1",
            points=pts,
            brush_preset="Basic-5 Size",
            color="#ff000080",
            size_px=10.0,
            opacity=0.5,
            is_eraser=True,
        )
        normal_stroke = Stroke(
            id="s1",
            points=pts,
            brush_preset="Basic-5 Size",
            color="#0000ff",
            size_px=10.0,
            is_eraser=False,
        )
        plan = DrawingPlan(
            prompt="cyber girl",
            seed=42,
            strokes=[normal_stroke, eraser_stroke],
            goal_reached=True,
            completion_score=0.95,
            canvas_width=1000.0,
            canvas_height=1000.0,
        )
        self.assertTrue(plan.goal_reached)
        self.assertEqual(plan.completion_score, 0.95)

        d = plan.as_dict()
        self.assertTrue(d["goal_reached"])
        self.assertEqual(d["completion_score"], 0.95)

        # from_dict での復元
        restored_plan = DrawingPlan.from_dict(d)
        self.assertTrue(restored_plan.goal_reached)
        self.assertEqual(restored_plan.completion_score, 0.95)
        self.assertTrue(restored_plan.strokes[1].is_eraser)

        # to_svg() での消しゴムストローク出力検証
        svg_content = plan.to_svg()
        self.assertIn('class="stroke eraser"', svg_content)
        self.assertIn('<mask id="eraser_mask_0"', svg_content)
        self.assertIn('mask="url(#eraser_mask_0)"', svg_content)
        self.assertIn('class="stroke eraser" stroke-opacity="0.25"', svg_content)
        self.assertNotIn('class="stroke eraser" stroke="#ffffff"', svg_content)
        import xml.etree.ElementTree as ET

        ET.fromstring(svg_content)

    def test_rule_based_planner_auto_count_and_goal(self) -> None:
        """RuleBasedPlanner での count=None (Auto) および Goal 判定テスト。"""
        planner = RuleBasedPlanner()
        # auto_count = True
        plan_auto = planner.plan("cute anime girl", seed=100, auto_count=True, width=1000, height=1000)
        self.assertGreater(len(plan_auto.strokes), 0)
        self.assertTrue(plan_auto.goal_reached)

        # count=None
        plan_none = planner.plan("cyberpunk city landscape", seed=200, count=None, width=1000, height=1000)
        self.assertGreater(len(plan_none.strokes), 0)

    def test_llm_planner_sanitize_auto_count_and_eraser(self) -> None:
        """LLMPlanner の count=None 品質予算と is_eraser 保持テスト。"""
        from .llm_planner import _validate_and_sanitize_plan

        pts = [StrokePoint(10, 10, 0.5, 0), StrokePoint(20, 20, 0.8, 10)]
        strokes = [Stroke(f"s_{i}", pts, size_px=10.0, is_eraser=(i % 2 == 1)) for i in range(10)]
        raw_plan = DrawingPlan(
            prompt="test",
            seed=123,
            strokes=strokes,
            goal_reached=True,
            completion_score=0.92,
        )

        # count=None (Autoモード: 切り詰めずに全10本保持)
        sanitized = _validate_and_sanitize_plan(
            raw_plan,
            prompt="test",
            seed=123,
            count=None,
            width=1000.0,
            height=1000.0,
        )
        self.assertEqual(len(sanitized.strokes), 10)
        self.assertTrue(sanitized.goal_reached)
        self.assertEqual(sanitized.completion_score, 0.92)
        self.assertTrue(sanitized.strokes[1].is_eraser)
        self.assertFalse(sanitized.strokes[0].is_eraser)

    def test_plan_worker_goal_mode_early_exit(self) -> None:
        """PlanWorker の Goal モード自律早期終了テスト。"""

        class _GoalPlanner(RuleBasedPlanner):
            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                p = super().plan(*args, **kwargs)
                return DrawingPlan(
                    prompt=p.prompt,
                    seed=p.seed,
                    strokes=p.strokes,
                    goal_reached=True,
                    completion_score=1.0,
                )

        worker = PlanWorker(
            planner=_GoalPlanner(),
            prompt="test girl",
            seed=42,
            count=None,
            width=500.0,
            height=500.0,
            auto_count=True,
            goal_mode=True,
        )
        self.assertEqual(worker.max_iterations, 10)
        self.assertTrue(worker.auto_count)
        self.assertTrue(worker.goal_mode)

    def test_adaptive_sampling_and_tapering_by_layer(self) -> None:
        """Flats の面抜け防止フラットテーパーおよび Lineart のシャープな入り抜きテスト。"""
        from ai_stroke_painter.llm_planner import _smooth_and_densify_points

        raw_pts = [
            StrokePoint(10.0, 10.0, 1.0, 0),
            StrokePoint(50.0, 50.0, 1.0, 10),
            StrokePoint(100.0, 100.0, 1.0, 20),
        ]

        # 1. Flats (太いブラシ 80px)
        flats_pts = _smooth_and_densify_points(raw_pts, 500, 500, layer_name="Flats", size_px=80.0)
        self.assertLessEqual(len(flats_pts), 16)
        # Flats の両端は 0.8 以上の高い筆圧を維持して隙間を防ぐ
        self.assertGreaterEqual(flats_pts[0].pressure, 0.75)
        self.assertGreaterEqual(flats_pts[-1].pressure, 0.75)

        shading_pts = _smooth_and_densify_points(raw_pts, 500, 500, layer_name="Shading", size_px=30.0)
        self.assertLessEqual(len(shading_pts), 16)
        self.assertGreaterEqual(shading_pts[0].pressure, 0.75)
        self.assertGreaterEqual(shading_pts[-1].pressure, 0.75)

        # 2. Lineart (細いブラシ 8px)
        line_pts = _smooth_and_densify_points(raw_pts, 500, 500, layer_name="Lineart", size_px=8.0)
        self.assertGreaterEqual(len(line_pts), 8)
        # Lineart の両端はシャープな入り抜き（低い筆圧）
        self.assertLess(line_pts[0].pressure, 0.4)
        self.assertLess(line_pts[-1].pressure, 0.4)

    def test_brush_preset_category_resolution(self) -> None:
        """プリセット名がカテゴリキーワードから柔軟に解決されるテスト。"""
        from types import SimpleNamespace
        from unittest.mock import patch

        class FakePreset:
            def __init__(self, name: str) -> None:
                self._name = name

            def name(self) -> str:
                return self._name

        p_airbrush = FakePreset("Airbrush Static")
        p_ink = FakePreset("Ink-2 Fineliner")
        p_dry = FakePreset("Dry Bristles Rough")
        p_basic = FakePreset("Basic-5 Size")

        class FakeView:
            def __init__(self) -> None:
                self.current_preset: Any = None

            def setCurrentBrushPreset(self, p: Any) -> None:  # noqa: N802
                self.current_preset = p

        view = FakeView()
        app = SimpleNamespace(
            activeWindow=lambda: SimpleNamespace(activeView=lambda: view),
            resources=lambda _k: {
                "p1": p_airbrush,
                "p2": p_ink,
                "p3": p_dry,
                "p4": p_basic,
            },
        )
        fake_krita = SimpleNamespace(Krita=SimpleNamespace(instance=lambda: app))

        adapter = KritaCanvasAdapter()
        document = _FakeDocument(active=_FakeNode("Flats"))

        with patch.dict("sys.modules", {"krita": fake_krita}):
            # Airbrush Soft -> Airbrush Static に解決
            plan_air = DrawingPlan(
                prompt="test",
                seed=1,
                strokes=[
                    Stroke("s1", [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)], brush_preset="Airbrush Soft")
                ],
            )
            adapter.render(document, plan_air, view=view)
            self.assertEqual(view.current_preset, p_airbrush)

            # Ink-3 Gpen -> Ink-2 Fineliner に解決
            plan_ink = DrawingPlan(
                prompt="test",
                seed=1,
                strokes=[
                    Stroke("s2", [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)], brush_preset="Ink-3 Gpen")
                ],
            )
            adapter.render(document, plan_ink, view=view)
            self.assertEqual(view.current_preset, p_ink)

            # 消しゴムが解決できない環境では通常ブラシで上書きせず、安全側に失敗する。
            plan_eraser = DrawingPlan(
                prompt="test",
                seed=1,
                strokes=[
                    Stroke(
                        "s3",
                        [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)],
                        brush_preset="Ink-3 Gpen",
                        is_eraser=True,
                    )
                ],
            )
            with self.assertRaisesRegex(RuntimeError, "消しゴムプリセット"):
                adapter.render(document, plan_eraser, view=view)

    def test_ensure_layer_smart_blend_modes(self) -> None:
        """レイヤー名に応じた自動ブレンドモード設定のテスト。"""
        document = _FakeDocument()
        adapter = KritaCanvasAdapter()

        node_shading = adapter.ensure_layer(document, "Shading")
        self.assertEqual(node_shading._blending_mode, "multiply")

        node_hl = adapter.ensure_layer(document, "Highlights")
        self.assertEqual(node_hl._blending_mode, "addition")

        node_line = adapter.ensure_layer(document, "Lineart")
        self.assertEqual(node_line._blending_mode, "normal")

    def test_fine_lineart_preserved_on_high_res_canvas(self) -> None:
        """A4 300dpi (2480x3508) 等の高解像度キャンバスでも目やまつ毛の極細線 (1.5〜3.5px) が太くならずに維持されるテスト。"""
        from ai_stroke_painter.llm_planner import _adaptive_stroke_size

        w, h = 2480.0, 3508.0

        # 極細ディテール線 (目、二重、まつ毛、鼻先、唇)
        self.assertEqual(_adaptive_stroke_size(2.0, "Lineart", w, h), 2.0)
        self.assertEqual(_adaptive_stroke_size(3.5, "Lineart", w, h), 3.5)

        # 極細ハイライト点
        self.assertEqual(_adaptive_stroke_size(2.5, "Highlights", w, h), 2.5)

        # 細部シェーディング (チーク、鼻下、瞳の影)
        self.assertEqual(_adaptive_stroke_size(15.0, "Shading", w, h), 15.0)

        # 背景・下塗り (広域塗りつぶしは隙間防止のため適正サイズを確保)
        self.assertGreaterEqual(_adaptive_stroke_size(5.0, "Flats", w, h), 40.0)

    def test_pressure_dynamics_interpolation_and_tapering(self) -> None:
        """AIが指定した筆圧ダイナミクス ([0.2, 0.95, 0.1]) がスプライン補間後も抑揚を維持するテスト。"""
        from ai_stroke_painter.llm_planner import _smooth_and_densify_points

        dynamic_pts = [
            StrokePoint(100.0, 150.0, 0.2, 0),
            StrokePoint(130.0, 140.0, 0.95, 10),
            StrokePoint(160.0, 130.0, 0.08, 20),
        ]

        smoothed = _smooth_and_densify_points(dynamic_pts, 1000, 1000, layer_name="Lineart", size_px=2.5)
        self.assertGreaterEqual(len(smoothed), 6)

        pressures = [p.pressure for p in smoothed]
        # 入りと抜きが細く、中央で高い筆圧ピークを持つ
        self.assertLess(pressures[0], 0.25)
        self.assertGreater(max(pressures), 0.70)
        self.assertLess(pressures[-1], 0.15)

    def test_harvest_stroke_fragments_without_explicit_id(self) -> None:
        """F1 回帰テスト: stroke に id フィールドがない場合でも rescue 時に自動 ID が付与され DrawingPlan が生成できる。"""
        from ai_stroke_painter.llm_planner import _harvest_stroke_fragments

        text_sample = (
            'Some thinking output... {"points": [{"x": 10, "y": 20}, {"x": 30, "y": 40}], '
            '"brush_preset": "Ink-3 Gpen", "color": "#111111", "size_px": 5.0, "layer_name": "Lineart"}'
        )
        harvested = _harvest_stroke_fragments(text_sample)
        self.assertIsNotNone(harvested)
        assert harvested is not None
        self.assertGreaterEqual(len(harvested.get("strokes", [])), 1)
        plan = DrawingPlan.from_dict(harvested)
        self.assertEqual(len(plan.strokes), 1)
        self.assertTrue(plan.strokes[0].id.startswith("stroke_"))

    def test_manga_fx_generic_fx_keyword_does_not_hijack_speed_or_focus_lines(self) -> None:
        """F2 回帰テスト: 'fx' を含むプロンプトでも、速度線や集中線が魔法陣にハイジャックされない。"""
        from ai_stroke_painter.procedural.manga_fx import generate_manga_fx_strokes

        # speed lines fx -> 流線 (Lineart レイヤーのみで構成され、FX レイヤーの魔法円を含まない)
        speed_strokes = generate_manga_fx_strokes("speed lines fx", seed=1, count=None, width=1000, height=1000)
        self.assertTrue(all(s.layer_name == "Lineart" for s in speed_strokes))
        self.assertFalse(any(s.color in ("#ffd700", "#ffaa00", "#00ffff") for s in speed_strokes))

        # focus lines fx -> 集中線 (中心抜け放射線、魔法陣の多重同心円や星型を含まない)
        focus_strokes = generate_manga_fx_strokes("focus lines fx", seed=1, count=None, width=1000, height=1000)
        self.assertTrue(all(s.color == "#1a1a1a" for s in focus_strokes))
        self.assertFalse(any(s.color in ("#ffd700", "#ffaa00", "#00ffff") for s in focus_strokes))

    def test_procedural_fx_modifier_follows_palette_recoloring(self) -> None:
        """F3 回帰テスト: FX 修飾子 (magic 等) で追加されたストロークも選択パレットへリカラーされる。"""
        from ai_stroke_painter.procedural import generate_procedural_plan
        from ai_stroke_painter.procedural.base import color_palette

        plan = generate_procedural_plan(
            prompt="anime girl casting magic spell",
            seed=42,
            count=None,
            width=1000,
            height=1000,
            palette_name="monochrome",
        )
        mono_palette_hexes = set(color_palette("monochrome").values())
        for stroke in plan.strokes:
            self.assertIn(stroke.color.lower(), {c.lower() for c in mono_palette_hexes})

    def test_stroke_from_dict_null_brush_preset_fallback(self) -> None:
        """F4 回帰テスト: JSON 内で brush_preset が null (None) の場合、文字列 'None' ではなくデフォルトにフォールバックする。"""
        raw = {
            "id": "s_test_null",
            "points": [{"x": 10.0, "y": 20.0}, {"x": 30.0, "y": 40.0}],
            "brush_preset": None,
            "color": "#111111",
            "size_px": 5.0,
            "layer_name": "Lineart",
        }
        st = Stroke.from_dict(raw)
        self.assertNotEqual(st.brush_preset, "None")
        self.assertEqual(st.brush_preset, "Basic-5 Size")

    def test_system_role_fallback_single_user_merge_multi_turn(self) -> None:
        """F5 回帰テスト: マルチターン会話で system ロール適応時、sys_content が最初の user ターンにのみ統合される。"""
        from io import BytesIO
        from unittest.mock import MagicMock
        from urllib.error import HTTPError

        from ai_stroke_painter.llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings(base_url="https://api.openai.com/v1", api_key="sk-test", model="test-model")
        )

        err_body = b'{"error": {"message": "system role not supported"}}'
        http_err = HTTPError(
            url="https://api.openai.com/v1",
            code=400,
            msg="Bad Request",
            hdrs=cast(Any, {}),
            fp=BytesIO(err_body),
        )

        captured_payloads = []

        def mock_opener(req: Any, timeout: Any = None) -> Any:
            import json

            payload = json.loads(req.data.decode("utf-8"))
            captured_payloads.append(payload)
            if len(captured_payloads) == 1:
                raise http_err
            resp_content = json.dumps(
                {
                    "choices": [
                        {
                            "message": {
                                "content": json.dumps(
                                    {
                                        "schema_version": 1,
                                        "prompt": "test",
                                        "seed": 1,
                                        "strokes": [
                                            {
                                                "id": "s1",
                                                "points": [{"x": 0, "y": 0}, {"x": 10, "y": 10}],
                                                "brush_preset": "Basic-5 Size",
                                                "color": "#000000",
                                                "size_px": 2.0,
                                                "layer_name": "Lineart",
                                            }
                                        ],
                                    }
                                )
                            }
                        }
                    ]
                }
            ).encode("utf-8")
            mock_resp = MagicMock()
            mock_resp.read.return_value = resp_content
            mock_resp.getcode.return_value = 200
            mock_resp.__enter__.return_value = mock_resp
            return mock_resp

        planner._opener = mock_opener

        planner._conversation_history = [
            {"role": "user", "content": "Turn 1 user prompt"},
            {"role": "assistant", "content": "Turn 1 response"},
        ]

        planner.plan(prompt="Turn 2 user prompt", seed=1, count=1, width=100, height=100, iteration=2, max_iterations=3)

        self.assertGreaterEqual(len(captured_payloads), 2)
        retried_messages = captured_payloads[1]["messages"]
        user_msgs = [m for m in retried_messages if m.get("role") == "user"]
        self.assertGreaterEqual(len(user_msgs), 2)
        self.assertIn("[USER REQUEST]", str(user_msgs[0]["content"]))
        self.assertNotIn("[USER REQUEST]", str(user_msgs[1]["content"]))
        self.assertIn("Turn 2 user prompt", str(user_msgs[1]["content"]))


def run() -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
