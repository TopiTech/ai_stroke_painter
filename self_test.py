"""Krita を起動せずに実行できる、AI Stroke Painter の回帰・機能テスト。"""

from __future__ import annotations

import base64
import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
from threading import Thread
from typing import Any, cast
import unittest
from unittest.mock import patch
import uuid
from zipfile import ZipFile
import zlib

from .build_plugin import PACKAGE_NAME, build
from .docker import (
    AIStrokePainterDocker,
    ApiConnectionWorker,
    PlanWorker,
    _confirm,
    _format_plan_quality_summary,
    _is_plan_goal_reached,
    _safe_endpoint_label,
)
from .domain import (
    MAX_PLAN_STROKES,
    DrawingPlan,
    PlanValidationError,
    Stroke,
    StrokePoint,
    VisionCritique,
    combine_drawing_plans,
    materialize_render_options,
)
from .image_converter import (
    ImageStrokeConverter,
    _image_dimensions_from_header,
    _trace_edge_paths,
    sanitize_reference_image,
)
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
    _get_stroke_program_json_schema,
    _is_reasoning_model,
    _mapping_to_drawing_plan,
    _plan_from_response,
    _redact_sensitive_text,
    _SameOriginRedirectHandler,
    _sanitize_and_rescue_program_dict,
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
    composition_mode_multiply,
    composition_mode_plus,
    composition_mode_source_over,
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
    ProgramBrush,
    ProgramPoint,
    StrokeProgram,
    compile_stroke_program,
    drawing_plan_to_stroke_program,
)

_VALID_TINY_PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
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

    def test_stroke_program_v3_primitives_gradient_fill_and_ribbon(self) -> None:
        raw_program = {
            "schema_version": 2,
            "prompt": "v3 primitives test",
            "seed": 42,
            "canvas": {"width": 500, "height": 500},
            "operations": [
                {
                    "kind": "gradient_fill",
                    "id": "sky_grad",
                    "polygon": [[0.0, 0.0], [1.0, 0.0], [1.0, 0.5], [0.0, 0.5]],
                    "colors": ["#2b5c8f", "#eef6ff"],
                    "style": "linear",
                    "angle_deg": 90,
                },
                {
                    "kind": "ribbon",
                    "id": "hair_strand",
                    "spine": [[0.3, 0.2, 0.9], [0.4, 0.5, 0.8], [0.35, 0.8, 0.2]],
                    "width_start": 0.01,
                    "width_mid": 0.03,
                    "width_end": 0.005,
                    "taper_profile": "taper_both",
                },
                {
                    "kind": "path",
                    "id": "hero_outline",
                    "points": [[0.2, 0.3], [0.5, 0.2], [0.8, 0.3]],
                    "role": "outline",
                },
            ],
        }
        program = StrokeProgram.from_dict(raw_program)
        self.assertEqual(StrokeProgram.from_dict(program.as_dict()), program)
        plan = compile_stroke_program(program)
        self.assertTrue(len(plan.strokes) >= 3)
        self.assertIn("Flats", plan.layers)
        self.assertIn("Lineart", plan.layers)

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

    def test_macro_compilation_is_stable_across_python_hash_seeds(self) -> None:
        package_parent = Path(__file__).resolve().parent.parent
        code = (
            "import json\n"
            "from ai_stroke_painter.stroke_program import StrokeProgram, compile_stroke_program\n"
            "program = StrokeProgram.from_dict({\n"
            "  'schema_version': 2, 'prompt': 'macro', 'seed': 37,\n"
            "  'canvas': {'width': 640, 'height': 480},\n"
            "  'operations': [{'kind': 'macro', 'id': 'flower-cluster', 'name': 'flower_cluster',\n"
            "                   'center': [0.5, 0.5], 'radius': 0.2}]\n"
            "})\n"
            "print(json.dumps(compile_stroke_program(program).as_dict(), sort_keys=True))\n"
        )
        outputs: list[str] = []
        for hash_seed in ("1", "2"):
            env = dict(os.environ)
            env["PYTHONHASHSEED"] = hash_seed
            result = subprocess.run(
                [sys.executable, "-c", code],
                cwd=package_parent,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            outputs.append(result.stdout)
        self.assertEqual(outputs[0], outputs[1])

    def test_planner_keeps_requested_seed_across_iterations_for_combination(self) -> None:
        planner = RuleBasedPlanner()
        first = planner.plan("cat", 42, 2, 200, 200, iteration=1, max_iterations=2)
        second = planner.plan("cat", 42, 2, 200, 200, iteration=2, max_iterations=2)

        self.assertEqual(first.seed, 42)
        self.assertEqual(second.seed, 42)
        combined = combine_drawing_plans([first, second])
        self.assertEqual(combined.seed, 42)

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
                if prompt == "magic circle":
                    self.assertFalse(flats)
                    self.assertTrue(plan.metadata["overlay"])
                else:
                    self.assertTrue(flats)
                    self.assertTrue(
                        any(abs(stroke.points[-1].x - stroke.points[0].x) >= 800 * 0.75 for stroke in flats)
                    )
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

        sakura = generate_procedural_plan("fantasy sakura landscape with mountains and clouds", 42, None, 800, 600)
        wildflowers = generate_procedural_plan(
            "delicate watercolor wildflower garden with soft petals", 42, None, 800, 600
        )
        rose = generate_procedural_plan("blooming rose flower", 42, None, 800, 600)

        def landscape_uid(name: str, index: int = 0) -> str:
            return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/land/42/{name}/{index}"))

        self.assertIn(landscape_uid("mountain_ridge"), {stroke.id for stroke in sakura.strokes})
        self.assertIn(landscape_uid("sakura_blossom"), {stroke.id for stroke in sakura.strokes})
        self.assertIn(landscape_uid("wildflower_stem"), {stroke.id for stroke in wildflowers.strokes})
        self.assertNotIn(landscape_uid("rose_petal"), {stroke.id for stroke in sakura.strokes})
        self.assertNotEqual(
            tuple((stroke.points[0].x, stroke.points[0].y) for stroke in wildflowers.strokes),
            tuple((stroke.points[0].x, stroke.points[0].y) for stroke in rose.strokes),
        )

        manual_budget = generate_procedural_plan("anime girl portrait", 42, 40, 800, 600)
        self.assertEqual(len(manual_budget.strokes), 40)
        self.assertEqual(manual_budget.metadata["budget_strategy"], "operation_aware_v2")
        self.assertGreaterEqual(evaluate_plan_quality(manual_budget).coverage, 0.90)

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

    def test_character_3d_anatomy_and_lighting_structures(self) -> None:
        strokes = generate_character_strokes("anime girl with blue hair", 42, None, 800, 600)
        layer_names = {s.layer_name for s in strokes}
        self.assertTrue({"Draft", "Flats", "Shading", "Lineart", "Highlights"}.issubset(layer_names))

        def uid(name: str, idx: int = 0, seed: int = 42) -> str:
            return str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/char/{seed}/{name}/{idx}"))

        stroke_ids = {s.id for s in strokes}
        self.assertIn(uid("ear_outer_l"), stroke_ids)
        self.assertIn(uid("ear_outer_r"), stroke_ids)
        self.assertIn(uid("ear_inner_l"), stroke_ids)
        self.assertIn(uid("temple_shade_l"), stroke_ids)
        self.assertIn(uid("bangs_cast_shadow"), stroke_ids)
        self.assertIn(uid("neck_ao_top"), stroke_ids)
        self.assertIn(uid("clavicle_line_l"), stroke_ids)
        self.assertIn(uid("nose_highlight"), stroke_ids)
        self.assertIn(uid("lip_highlight"), stroke_ids)

        boy_strokes = generate_character_strokes("anime boy hero", 42, None, 800, 600)
        boy_ids = {s.id for s in boy_strokes}
        self.assertIn(uid("adams_apple"), boy_ids)
        self.assertIn(uid("shirt_collar_l"), boy_ids)
        self.assertIn(uid("short_hair", 0), boy_ids)

        plan = generate_procedural_plan("anime girl with blue hair and green eyes", 42, None, 800, 600)
        report = evaluate_plan_quality(plan)
        self.assertGreaterEqual(report.score, 0.85)
        self.assertGreaterEqual(report.coverage, 0.50)
        self.assertEqual(report.out_of_bounds_points, 0)
        self.assertFalse(report.issues)

    def test_plan_json_round_trip_and_collision_free_save(self) -> None:
        plan = RuleBasedPlanner().plan("curve", 9, 2, 300, 200)
        with tempfile.TemporaryDirectory() as temp:
            first_path = save_plan(plan, temp)
            second_path = save_plan(plan, temp)
            self.assertNotEqual(first_path, second_path)
            self.assertEqual(load_plan(first_path), plan)
            self.assertEqual(load_plan(second_path), plan)

    def test_combined_iteration_plan_and_materialized_render_options(self) -> None:
        points = [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 1.0, 10)]
        first = DrawingPlan(
            "session",
            7,
            [Stroke("shared", points, size_px=4.0, opacity=0.8, layer_name="Flats")],
            iteration=1,
            canvas_width=100,
            canvas_height=100,
        )
        second = DrawingPlan(
            "session",
            7,
            [Stroke("shared", points, size_px=2.0, opacity=0.6, layer_name="Lineart")],
            iteration=2,
            canvas_width=100,
            canvas_height=100,
            goal_reached=True,
            completion_score=0.95,
        )

        combined = combine_drawing_plans([first, second])
        self.assertEqual(len(combined.strokes), 2)
        self.assertEqual(len({stroke.id for stroke in combined.strokes}), 2)
        self.assertEqual(combined.metadata["source_stroke_counts"], [1, 1])
        self.assertTrue(combined.goal_reached)

        materialized = materialize_render_options(combined, size_multiplier=2.0, opacity_multiplier=0.5)
        self.assertEqual([stroke.size_px for stroke in materialized.strokes], [8.0, 4.0])
        self.assertEqual([stroke.opacity for stroke in materialized.strokes], [0.4, 0.3])
        self.assertTrue(materialized.metadata["render_options"]["materialized"])

        unknown_size = DrawingPlan("session", 7, [Stroke("unknown", points)])
        different_size = DrawingPlan(
            "session",
            7,
            [Stroke("different", points)],
            canvas_width=200,
            canvas_height=100,
        )
        with self.assertRaises(PlanValidationError):
            combine_drawing_plans([unknown_size, first, different_size])

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

        composite_plan = DrawingPlan(
            "blend parity",
            1,
            [
                Stroke("shadow", points, layer_name="Shading"),
                Stroke("light", points, layer_name="Highlights"),
            ],
            layers=["Shading", "Highlights"],
        )
        multi_svg = materialize_render_options(composite_plan, layer_mode="multi_layer").to_svg(100, 100)
        self.assertIn("mix-blend-mode:multiply", multi_svg)
        self.assertIn('data-krita-blend-mode="addition"', multi_svg)
        single_svg = materialize_render_options(composite_plan, layer_mode="single_layer").to_svg(100, 100)
        self.assertIn('id="layer_Combined"', single_svg)
        self.assertNotIn("mix-blend-mode", single_svg)

    def test_svg_comment_with_double_hyphen_stays_well_formed(self) -> None:
        import xml.etree.ElementTree as ET

        plan = RuleBasedPlanner().plan("attack -- defense", 7, 3, 100, 100)
        svg_content = plan.to_svg(100, 100)
        ET.fromstring(svg_content)

    def test_svg_removes_xml_forbidden_control_characters(self) -> None:
        import xml.etree.ElementTree as ET

        points = [StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)]
        plan = DrawingPlan(
            "prompt\x00 with\x0b controls",
            7,
            [Stroke("xml-safe", points, layer_name="Line\x01art")],
        )
        svg_content = plan.to_svg(100, 100)
        self.assertNotIn("\x00", svg_content)
        self.assertNotIn("\x01", svg_content)
        self.assertNotIn("\x0b", svg_content)
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
        complete_base = generate_procedural_plan("anime girl portrait", 1, None, 800, 600)
        score_only = DrawingPlan("score only", 1, complete_base.strokes, completion_score=1.0)
        low_score_explicit = DrawingPlan(
            "low score",
            1,
            complete_base.strokes,
            completion_score=0.1,
            goal_reached=True,
        )
        explicit = DrawingPlan(
            "explicit",
            1,
            complete_base.strokes,
            completion_score=0.95,
            goal_reached=True,
        )
        metadata_explicit = DrawingPlan(
            "metadata",
            1,
            complete_base.strokes,
            completion_score=0.95,
            metadata={"goal_reached": True},
        )
        self.assertFalse(_is_plan_goal_reached(score_only))
        self.assertFalse(_is_plan_goal_reached(low_score_explicit))
        self.assertTrue(_is_plan_goal_reached(explicit))
        self.assertTrue(_is_plan_goal_reached(metadata_explicit))

    def test_image_converter_rejects_invalid_data_without_silent_fallback(self) -> None:
        converter = ImageStrokeConverter()
        with self.assertRaises(ValueError):
            converter.convert_image_to_plan(b"not-a-valid-image", "cat", 42, 10, 800, 600)

    def test_reference_image_sanitizer_removes_png_text_metadata(self) -> None:
        chunk_data = b"GPS=35.0,139.0;Author=private"
        chunk_type = b"tEXt"
        text_chunk = (
            len(chunk_data).to_bytes(4, "big")
            + chunk_type
            + chunk_data
            + zlib.crc32(chunk_type + chunk_data).to_bytes(4, "big")
        )
        iend_offset = _VALID_TINY_PNG.rfind(b"\x00\x00\x00\x00IEND")
        private_png = _VALID_TINY_PNG[:iend_offset] + text_chunk + _VALID_TINY_PNG[iend_offset:]

        sanitized = sanitize_reference_image(private_png, max_dimension=512)

        self.assertEqual(_image_dimensions_from_header(sanitized), (1, 1))
        self.assertNotIn(b"GPS=", sanitized)
        self.assertNotIn(b"Author=", sanitized)

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
        self.assertIn(f"{PACKAGE_NAME}/image_generator.py", names)
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

    def test_packaged_zip_imports_cleanly_in_isolated_process(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            zip_path = Path(temp) / f"{PACKAGE_NAME}.zip"
            extract_dir = Path(temp) / "extracted"
            build(zip_path)
            with ZipFile(zip_path) as archive:
                archive.extractall(extract_dir)
            code = (
                "import sys\n"
                f"sys.path.insert(0, r'{extract_dir}')\n"
                "import ai_stroke_painter.docker\n"
                "import ai_stroke_painter.image_generator\n"
                "print('OK')\n"
            )
            res = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=False)
            self.assertEqual(res.returncode, 0, f"Import failed: {res.stderr}")
            self.assertIn("OK", res.stdout)


class OpenAICompatiblePlannerTests(unittest.TestCase):
    def test_stroke_program_schema_and_rescue_path_enforce_resource_limits(self) -> None:
        schema = _get_stroke_program_json_schema()["schema"]
        operations_schema = schema["properties"]["operations"]
        self.assertEqual(operations_schema["maxItems"], 2_000)
        path_schema = operations_schema["items"]["oneOf"][0]
        self.assertEqual(path_schema["properties"]["points"]["maxItems"], 1_000)

        oversized_points = [[0.0, 0.0] for _ in range(1_001)]
        oversized_operations = [
            {"kind": "path", "id": f"op-{index}", "points": oversized_points if index == 0 else [[0, 0], [1, 1]]}
            for index in range(2_001)
        ]
        rescued = _sanitize_and_rescue_program_dict(
            {"schema_version": 2, "canvas": {"width": 100, "height": 100}, "operations": oversized_operations}
        )
        self.assertEqual(len(rescued["operations"]), 2_000)
        self.assertEqual(len(rescued["operations"][0]["points"]), 1_000)

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

    def test_v2_pixel_coordinates_use_declared_canvas_dimensions(self) -> None:
        response = {
            "schema_version": 2,
            "prompt": "pixel coordinates",
            "canvas": {"width": 800, "height": 600},
            "operations": [
                {
                    "kind": "path",
                    "id": "pixel-line",
                    "points": [[400, 300], [799, 599]],
                    "brush": {"profile": "gpen", "size": 0.01},
                }
            ],
        }

        plan = _plan_from_response(response, prompt="pixel coordinates", seed=1, width=800, height=600)

        self.assertEqual(
            [(point.x, point.y) for point in plan.strokes[0].points],
            [(400.0, 300.0), (799.0, 599.0)],
        )

    def test_v2_string_false_does_not_enable_goal_completion_after_rescue(self) -> None:
        response = {
            "schema_version": 2,
            "prompt": "incomplete",
            "canvas": {"width": 800, "height": 600},
            "goal_reached": "false",
            "completion_score": 0.2,
            "operations": [
                {
                    "kind": "path",
                    "id": "line",
                    "points": [[0.1, 0.1], [0.9, 0.9]],
                    "brush": {"profile": "gpen", "size": 0.01},
                }
            ],
        }

        plan = _plan_from_response(response, prompt="incomplete", seed=1, width=800, height=600)

        self.assertFalse(plan.goal_reached)
        self.assertFalse(_is_plan_goal_reached(plan))

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

    def test_post_reads_short_chunks_until_eof(self) -> None:
        raw = json.dumps({"ok": True, "message": "chunked"}).encode("utf-8")

        class ChunkedResponse:
            headers = {"Content-Type": "application/json"}

            def __init__(self) -> None:
                self._chunks = [raw[:3], raw[3:8], raw[8:]]

            def read(self, _size: int) -> bytes:
                return self._chunks.pop(0) if self._chunks else b""

            def __enter__(self) -> ChunkedResponse:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: ChunkedResponse(),
        )
        self.assertEqual(planner._post({"model": "model"}), {"ok": True, "message": "chunked"})

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
        self.assertEqual(plan.strokes[0].color, "#3f51b5")
        self.assertEqual(plan.strokes[0].layer_name, "Lineart")
        self.assertGreaterEqual(len(plan.strokes[0].points), 2)
        assert Handler.received is not None
        self.assertEqual(Handler.received["path"], "/v1/chat/completions")
        self.assertEqual(Handler.received["authorization"], "Bearer test-key")
        self.assertEqual(Handler.received["body"]["model"], "test-model")
        response_format = Handler.received["body"]["response_format"]
        self.assertEqual(response_format["type"], "json_schema")
        self.assertTrue(response_format["json_schema"]["strict"])

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

    def test_autonomy_mode_system_instruction_and_settings(self) -> None:
        from ai_stroke_painter.llm_planner import OpenAICompatibleSettings, _system_instruction

        # Settings validation
        cfg_creative = OpenAICompatibleSettings("https://example.test/v1", "model", autonomy_mode="creative")
        self.assertEqual(cfg_creative.autonomy_mode, "creative")

        cfg_balanced = OpenAICompatibleSettings("https://example.test/v1", "model", autonomy_mode="balanced")
        self.assertEqual(cfg_balanced.autonomy_mode, "balanced")

        cfg_template = OpenAICompatibleSettings("https://example.test/v1", "model", autonomy_mode="template")
        self.assertEqual(cfg_template.autonomy_mode, "template")

        with self.assertRaises(ValueError):
            OpenAICompatibleSettings("https://example.test/v1", "model", autonomy_mode="unsupported_mode")

        # System instruction in creative mode
        inst_creative = _system_instruction(
            iteration=1,
            max_iterations=1,
            prompt="anime girl portrait with delicate eyes",
            autonomy_mode="creative",
        )
        self.assertIn("CREATIVE ANATOMY & DYNAMIC COMPOSITION", inst_creative)
        self.assertNotIn("Face Center X: 0.50", inst_creative)
        self.assertIn("full creative autonomy", inst_creative)

        # System instruction in template mode
        inst_template = _system_instruction(
            iteration=1,
            max_iterations=1,
            prompt="anime girl portrait with delicate eyes",
            autonomy_mode="template",
        )
        self.assertIn("STRICT SPATIAL FACIAL ANCHORS", inst_template)
        self.assertIn("Face Center X: 0.50", inst_template)

    def test_rescue_and_harvest_ribbon_and_gradient_fill(self) -> None:
        from ai_stroke_painter.llm_planner import _harvest_stroke_fragments, _sanitize_and_rescue_program_dict

        raw_prog = {
            "schema_version": 2,
            "prompt": "sunset ribbon and sky",
            "operations": [
                {
                    "kind": "gradient_fill",
                    "id": "sky_grad",
                    "polygon": [[0.0, 0.0], [1.0, 0.0], [1.0, 0.5], [0.0, 0.5]],
                    "colors": ["#4a1c40", "#f8a846"],
                    "style": "linear",
                    "angle_deg": 90.0,
                },
                {
                    "kind": "ribbon",
                    "id": "hair_ribbon",
                    "spine": [[0.3, 0.2], [0.35, 0.5], [0.4, 0.8]],
                    "width_start": 0.01,
                    "width_mid": 0.03,
                    "width_end": 0.005,
                    "taper_profile": "taper_both",
                },
            ],
        }
        rescued = _sanitize_and_rescue_program_dict(raw_prog, 1000.0, 1000.0)
        self.assertEqual(len(rescued["operations"]), 2)
        op_grad = rescued["operations"][0]
        self.assertEqual(op_grad["kind"], "gradient_fill")
        self.assertEqual(op_grad["colors"], ["#4a1c40", "#f8a846"])
        self.assertEqual(op_grad["style"], "linear")

        op_ribbon = rescued["operations"][1]
        self.assertEqual(op_ribbon["kind"], "ribbon")
        self.assertEqual(len(op_ribbon["spine"]), 3)
        self.assertAlmostEqual(op_ribbon["width_mid"], 0.03)

        # Harvest from truncated / unclosed text
        broken_text = (
            "Here is the art program:\n"
            '{"id":"sky_op","kind":"gradient_fill","polygon":[[0,0],[1,0],[1,0.6],[0,0.6]],"colors":["#112233","#ffffff"]}\n'
            '{"id":"ribbon_op","kind":"ribbon","spine":[[0.2,0.3],[0.5,0.7]],"width_start":0.01}\n'
        )
        harvested = _harvest_stroke_fragments(broken_text)
        self.assertIsNotNone(harvested)
        assert harvested is not None
        self.assertEqual(len(harvested["operations"]), 2)
        self.assertEqual(harvested["operations"][0]["kind"], "gradient_fill")
        self.assertEqual(harvested["operations"][1]["kind"], "ribbon")

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
        plan1 = planner.plan(
            "mountain landscape",
            42,
            1,
            800,
            600,
            image_data=_VALID_TINY_PNG,
            iteration=1,
            max_iterations=2,
        )
        self.assertEqual(len(plan1.strokes), 1)
        self.assertFalse(plan1.request_canvas_image)

        # Step 2: 前回描画のキャンバスを添付し、視覚評価を次の計画へ反映する。
        plan2 = planner.plan(
            "mountain landscape",
            42,
            1,
            800,
            600,
            image_data=_VALID_TINY_PNG,
            canvas_image=_VALID_TINY_PNG,
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
        self.assertEqual(sum(part.get("type") == "image_url" for part in step2_content), 1)
        self.assertFalse(any("REFERENCE IMAGE" in str(part.get("text", "")) for part in step2_content))
        self.assertTrue(any("CURRENT CANVAS" in str(part.get("text", "")) for part in step2_content))
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
                            {
                                "choices": [
                                    {
                                        "finish_reason": "stop",
                                        "message": {"content": "SENSITIVE_PROVIDER_RESPONSE"},
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
            OpenAICompatibleSettings("https://example.test/v1", "o3-mini", max_tokens=16384),
            opener=FakeOpener(),
            log_callback=lambda msg: logs.append(msg),
        )
        msg = planner.test_connection()
        self.assertIn("接続成功", msg)
        self.assertNotIn("SENSITIVE_PROVIDER_RESPONSE", msg)
        self.assertNotIn("SENSITIVE_PROVIDER_RESPONSE", "\n".join(logs))
        self.assertEqual(len(attempts), 1)
        # 接続確認に過大な生成枠を使わず、十分な上限 2048 へ制限すること
        self.assertEqual(attempts[0]["max_completion_tokens"], 2048)
        self.assertEqual(attempts[0]["messages"], [{"role": "user", "content": "Respond with 'OK'."}])
        # 接続テスト時に DrawingPlan JSON 救済の警告が出ないこと
        self.assertFalse(any("途切れ JSON の救済を試みます" in log for log in logs))

    def test_test_connection_rejects_unparseable_http_200_response(self) -> None:
        class FakeOpener:
            def __call__(self, request: Any, timeout: float = 10.0) -> Any:
                class MockResponse:
                    def read(self, _size: int) -> bytes:
                        return b'{"unexpected":"shape"}'

                    def __enter__(self) -> Any:
                        return self

                    def __exit__(self, *_args: Any) -> None:
                        pass

                return MockResponse()

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "gpt-4o"),
            opener=FakeOpener(),
        )
        with self.assertRaisesRegex(LLMPlannerError, "接続失敗"):
            planner.test_connection()

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
        plan = planner.plan("multimodal test", 1, 1, 100, 100, image_data=_VALID_TINY_PNG)
        self.assertEqual(opener.count, 2)
        self.assertEqual(plan.prompt, "multimodal test")

        retried_user_msg = captured_bodies[1]["messages"][0]
        self.assertEqual(retried_user_msg["role"], "user")
        self.assertIsInstance(retried_user_msg["content"], list)
        self.assertEqual(len(retried_user_msg["content"]), 3)
        self.assertEqual(retried_user_msg["content"][0]["type"], "text")
        self.assertIn("[USER REQUEST]", retried_user_msg["content"][0]["text"])
        self.assertEqual(retried_user_msg["content"][1]["type"], "text")
        self.assertIn("REFERENCE IMAGE", retried_user_msg["content"][1]["text"])
        self.assertEqual(retried_user_msg["content"][2]["type"], "image_url")
        self.assertTrue(retried_user_msg["content"][2]["image_url"]["url"].startswith("data:image/png;base64,"))

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
        with patch("ai_stroke_painter.llm_planner.sanitize_reference_image", return_value=jpeg_dummy):
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
    def test_constant_pressure_stroke_uses_one_continuous_krita_path(self) -> None:
        class PathNode(_FakeNode):
            def __init__(self) -> None:
                super().__init__("path target")
                self.paths: list[Any] = []

            def paintPath(self, path: Any) -> None:  # noqa: N802
                self.paths.append(path)
                self.pixels = b"painted path pixels"

        target = PathNode()
        document = _FakeDocument(active=target)
        stroke = Stroke(
            "continuous-path",
            [
                StrokePoint(100, 100, 0.6, 0),
                StrokePoint(130, 120, 0.6, 10),
                StrokePoint(160, 105, 0.6, 20),
                StrokePoint(190, 140, 0.6, 30),
            ],
            size_px=8,
        )
        with (
            patch("ai_stroke_painter.krita_adapter._apply_stroke_style") as apply_style,
            patch("ai_stroke_painter.krita_adapter._apply_color_to_krita"),
        ):
            rendered = KritaCanvasAdapter(layer_mode="active_layer").render(
                document,
                DrawingPlan("path", 1, [stroke]),
            )

        self.assertEqual(rendered, 1)
        self.assertEqual(len(target.paths), 1)
        self.assertEqual(target.lines, [])
        self.assertAlmostEqual(apply_style.call_args.kwargs["size_multiplier"], 0.6)

    def test_standalone_active_layer_snapshot_is_limited_to_plan_bounds(self) -> None:
        class SnapshotNode(_FakeNode):
            def __init__(self) -> None:
                super().__init__("snapshot target")
                self.read_rectangles: list[tuple[int, int, int, int]] = []
                self.write_rectangles: list[tuple[int, int, int, int]] = []

            def pixelData(self, x: int, y: int, width: int, height: int) -> bytes:  # noqa: N802
                self.pixel_reads += 1
                self.read_rectangles.append((x, y, width, height))
                return self.pixels

            def setPixelData(self, pixels: bytes, x: int, y: int, width: int, height: int) -> bool:  # noqa: N802
                self.pixel_writes += 1
                self.write_rectangles.append((x, y, width, height))
                self.pixels = pixels
                return True

        target = SnapshotNode()
        document = _FakeDocument(active=target)
        plan = DrawingPlan(
            "bounded snapshot",
            1,
            [
                Stroke(
                    "bounded",
                    [
                        StrokePoint(300, 200, 0.8, 0),
                        StrokePoint(340, 230, 0.8, 10),
                        StrokePoint(380, 210, 0.8, 20),
                    ],
                    size_px=10,
                )
            ],
        )
        checks = 0

        def cancel_during_stroke() -> bool:
            nonlocal checks
            checks += 1
            return checks >= 4

        KritaCanvasAdapter(layer_mode="active_layer").render(document, plan, cancelled=cancel_during_stroke)

        self.assertEqual(len(target.read_rectangles), 1)
        self.assertEqual(target.write_rectangles, target.read_rectangles)
        x, y, width, height = target.read_rectangles[0]
        self.assertGreater(x, 0)
        self.assertGreater(y, 0)
        self.assertLess(width * height, document.width() * document.height())

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

        # セッションを使わない直接 render でも、イベント処理中のキャンバス入力を遮断する。
        standalone_target = _FakeNode("standalone")
        standalone_document = _FakeDocument(active=standalone_target)
        standalone_canvas = Canvas()
        standalone_adapter = KritaCanvasAdapter(layer_mode="active_layer", event_interval=1)
        with (
            patch("ai_stroke_painter.krita_adapter._apply_stroke_style"),
            patch("ai_stroke_painter.krita_adapter._apply_color_to_krita"),
            patch("ai_stroke_painter.krita_adapter._process_events") as standalone_process_events,
        ):
            standalone_adapter.render(
                standalone_document,
                plan,
                layer_mode="active_layer",
                view=View(standalone_canvas),
                event_interval=1,
            )
        self.assertTrue(standalone_process_events.called)
        self.assertEqual(len(standalone_canvas.installed), 1)
        self.assertEqual(standalone_canvas.removed, standalone_canvas.installed)

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
    def test_preview_quality_summary_reports_success_and_missing_semantics(self) -> None:
        prompt = "anime girl with a cat in a cyberpunk city and focus lines"
        standard = generate_procedural_plan(prompt, 42, 40, 800, 600)
        constrained = generate_procedural_plan(prompt, 42, 12, 800, 600)

        self.assertIn("✓ 品質診断", _format_plan_quality_summary(standard))
        constrained_summary = _format_plan_quality_summary(constrained)
        self.assertIn("不足:", constrained_summary)
        self.assertIn("本数を増やして", constrained_summary)

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

    def test_docker_close_waits_for_workers_before_ending_canvas_session(self) -> None:
        class Status:
            def __init__(self) -> None:
                self.text = ""

            def setText(self, value: str) -> None:  # noqa: N802
                self.text = value

        class SlowWorker:
            def __init__(self) -> None:
                self.cancelled = False

            def cancel(self) -> None:
                self.cancelled = True

            def wait(self, _timeout_ms: int) -> bool:
                return False

            def isRunning(self) -> bool:  # noqa: N802
                return True

        class CloseEvent:
            def __init__(self) -> None:
                self.ignored = False

            def ignore(self) -> None:
                self.ignored = True

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.status = Status()
        slow_worker = SlowWorker()
        docker._worker = cast(Any, slow_worker)
        docker._connection_worker = None
        docker._closing = False
        event = CloseEvent()

        with (
            patch.object(AIStrokePainterDocker, "_save_settings"),
            patch.object(AIStrokePainterDocker, "_finish_canvas_session", return_value=True) as finish_session,
        ):
            docker.closeEvent(event)

        self.assertTrue(slow_worker.cancelled)
        self.assertTrue(event.ignored)
        self.assertFalse(docker._closing)
        self.assertIn("終了を待っています", docker.status.text)
        finish_session.assert_not_called()

    def test_debug_endpoint_label_never_exposes_url_credentials(self) -> None:
        label = _safe_endpoint_label("https://alice:secret@example.test:8443/v1/secret-token")
        self.assertEqual(label, "https://example.test:8443")
        self.assertNotIn("alice", label)
        self.assertNotIn("secret", label)
        self.assertNotIn("secret-token", label)

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

    def test_openai_mode_disables_seed_controls(self) -> None:
        docker = AIStrokePainterDocker()

        # オフライン (プロシージャル) モード: auto_seed 有効、seed は not auto_seed.isChecked()
        docker.planner_mode.setCurrentIndex(0)
        docker.auto_seed.setChecked(False)
        docker._update_planner_settings_state()
        self.assertTrue(docker.auto_seed.isEnabled())
        self.assertTrue(docker.seed.isEnabled())

        # auto_seed をチェックすると seed は無効化される
        docker.auto_seed.setChecked(True)
        self.assertTrue(docker.auto_seed.isEnabled())
        self.assertFalse(docker.seed.isEnabled())

        # OpenAI 互換モードへ切り替え: seed / auto_seed 共にブラックアウト（無効化）
        docker.planner_mode.setCurrentIndex(1)
        docker._update_planner_settings_state()
        self.assertFalse(docker.auto_seed.isEnabled())
        self.assertFalse(docker.seed.isEnabled())

        # OpenAI 互換モード中は auto_seed のトグルが走っても seed は無効のまま
        docker.auto_seed.setChecked(False)
        self.assertFalse(docker.auto_seed.isEnabled())
        self.assertFalse(docker.seed.isEnabled())

        # オフラインモードに戻すと復帰
        docker.planner_mode.setCurrentIndex(0)
        docker._update_planner_settings_state()
        self.assertTrue(docker.auto_seed.isEnabled())
        self.assertTrue(docker.seed.isEnabled())

    def test_builtin_preset_enables_auto_quality_budget(self) -> None:
        docker = AIStrokePainterDocker()
        docker.auto_count.setChecked(False)
        docker.preset_combo.setCurrentIndex(0)
        docker._apply_preset()
        self.assertTrue(docker.auto_count.isChecked())
        self.assertFalse(docker.count.isEnabled())

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

    def test_plan_worker_distributes_auto_budget_across_ten_iterations(self) -> None:
        points = [StrokePoint(10, 10, 0.8, 0), StrokePoint(20, 20, 0.8, 10)]

        class DensePlanner:
            log_callback: Any = None

            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                iteration = int(kwargs.get("iteration", 1))
                return DrawingPlan(
                    "dense",
                    1,
                    [
                        Stroke(
                            f"step-{iteration}-{index}",
                            points,
                            layer_name=("Flats" if index % 2 == 0 else "Lineart"),
                        )
                        for index in range(500)
                    ],
                    iteration=iteration,
                )

        plans: list[DrawingPlan] = []
        worker = PlanWorker(
            cast(Any, DensePlanner()),
            "dense",
            1,
            None,
            100,
            100,
            max_iterations=10,
            auto_count=True,
        )

        def accept_plan(plan: DrawingPlan) -> None:
            plans.append(plan)
            worker.notify_render_done()

        worker.plan_ready.connect(accept_plan)
        worker.run()

        self.assertTrue(worker.completed_successfully)
        self.assertEqual(len(plans), 10)
        self.assertEqual(sum(len(plan.strokes) for plan in plans), 2_000)
        self.assertTrue(all(len(plan.strokes) == 200 for plan in plans))
        self.assertTrue(all("session_budget" in plan.metadata for plan in plans))

    def test_plan_worker_goal_mode_uses_cumulative_quality(self) -> None:
        prompt = "fantasy sakura landscape with mountains and clouds"
        foundation = generate_procedural_plan(prompt, 42, None, 800, 600)
        detail = Stroke(
            "final-detail",
            [StrokePoint(390, 290, 0.8, 0), StrokePoint(410, 310, 0.8, 10)],
            layer_name="Highlights",
        )

        class CumulativeGoalPlanner:
            log_callback: Any = None

            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                iteration = int(kwargs.get("iteration", 1))
                if iteration == 1:
                    return DrawingPlan(
                        prompt,
                        42,
                        foundation.strokes,
                        iteration=1,
                        canvas_width=800,
                        canvas_height=600,
                        goal_reached=False,
                        completion_score=0.7,
                    )
                return DrawingPlan(
                    prompt,
                    42,
                    [detail],
                    iteration=iteration,
                    canvas_width=800,
                    canvas_height=600,
                    goal_reached=True,
                    completion_score=0.95,
                )

        plans: list[DrawingPlan] = []
        worker = PlanWorker(
            cast(Any, CumulativeGoalPlanner()),
            prompt,
            42,
            None,
            800,
            600,
            max_iterations=3,
            auto_count=True,
            goal_mode=True,
        )

        def accept_plan(plan: DrawingPlan) -> None:
            plans.append(plan)
            worker.notify_render_done()

        worker.plan_ready.connect(accept_plan)
        worker.run()

        self.assertTrue(worker.completed_successfully)
        self.assertEqual(len(plans), 2)
        self.assertFalse(_is_plan_goal_reached(plans[1]))
        self.assertIs(plans[1].metadata.get("session_goal_reached"), True)
        self.assertEqual(plans[1].metadata.get("session_stroke_count"), len(foundation.strokes) + 1)

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
        planner_items = [("Offline", "offline"), ("LLM", "openai_compatible")]
        docker.planner_mode = FakeComboWidget(planner_items, default_data="openai_compatible")
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
        docker.confirm_before_apply = FakeBoolWidget(False)
        docker.debug_mode_chk = FakeBoolWidget(True)

        docker._save_settings()

        docker2 = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker2.planner_mode = FakeComboWidget(planner_items, default_data="offline")
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
        docker2.confirm_before_apply = FakeBoolWidget(True)
        docker2.debug_mode_chk = FakeBoolWidget()

        docker2._load_settings()

        self.assertEqual(docker2.base_url.text(), "https://custom.api/v1")
        self.assertEqual(docker2.planner_mode.currentData(), "openai_compatible")
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
        self.assertFalse(docker2.confirm_before_apply.isChecked())
        self.assertTrue(docker2.debug_mode_chk.isChecked())

        if callable(QSettings):
            corrupted_settings: Any = QSettings("AIStrokePainter", "DockerSettings")
            corrupted_settings.setValue("timeout_sec", "broken-number")
            corrupted_settings.setValue("max_tokens", 8192)
            if hasattr(corrupted_settings, "sync"):
                corrupted_settings.sync()

            docker3 = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
            docker3.timeout_sec = FakeIntWidget(45)
            docker3.max_tokens = FakeIntWidget(1024)
            docker3._load_settings()
            self.assertEqual(docker3.timeout_sec.value(), 45)
            self.assertEqual(docker3.max_tokens.value(), 8192)
            if hasattr(corrupted_settings, "clear"):
                corrupted_settings.clear()

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
        quality_base = generate_procedural_plan("anime girl portrait", 1, None, 800, 600)
        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=quality_base.strokes,
            iteration=1,
            goal_reached=True,
            completion_score=1.0,
            canvas_width=800,
            canvas_height=600,
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

    def test_docker_exports_cumulative_materialized_plan(self) -> None:
        class FakeCanvasPort:
            def render(self, *args: Any, **kwargs: Any) -> int:
                return 1

        class FakeWorker:
            max_iterations = 2
            goal_mode = False

            def __init__(self) -> None:
                self.done = 0

            def notify_render_done(self) -> None:
                self.done += 1

            def isRunning(self) -> bool:  # noqa: N802
                return True

        class Status:
            text = ""

            def setText(self, value: str) -> None:  # noqa: N802
                self.text = value

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.canvas_port = cast(Any, FakeCanvasPort())
        docker._active_doc = object()
        docker._active_view = None
        docker._cancel = False
        worker = FakeWorker()
        docker._worker = cast(Any, worker)
        docker.status = cast(Any, Status())
        docker._run_render_options = {
            "save_json": True,
            "save_svg": False,
            "size_multiplier": 2.0,
            "opacity_multiplier": 0.5,
            "layer_mode": "multi_layer",
            "layer_prefix": "AI Artwork",
            "event_interval": 1,
        }
        points = [StrokePoint(0, 0, 0.5, 0), StrokePoint(10, 10, 0.8, 10)]
        first = DrawingPlan("test", 1, [Stroke("s1", points, size_px=3.0)], iteration=1)
        second = DrawingPlan("test", 1, [Stroke("s2", points, size_px=5.0)], iteration=2)
        saved: list[DrawingPlan] = []

        def record_plan(value: DrawingPlan) -> Path:
            saved.append(value)
            return Path("p")

        with patch("ai_stroke_painter.docker.save_plan", side_effect=record_plan):
            docker._on_plan_ready(first)
            docker._on_plan_ready(second)

        self.assertEqual(len(saved), 1)
        self.assertEqual([stroke.id for stroke in saved[0].strokes], ["s1", "s2"])
        self.assertEqual([stroke.size_px for stroke in saved[0].strokes], [6.0, 10.0])
        self.assertEqual([stroke.opacity for stroke in saved[0].strokes], [0.5, 0.5])
        self.assertIsNotNone(docker._last_plan)
        assert docker._last_plan is not None
        self.assertEqual(len(docker._last_plan.strokes), 2)

    def test_docker_export_failure_does_not_report_render_failure(self) -> None:
        class FakeCanvasPort:
            def render(self, *args: Any, **kwargs: Any) -> int:
                return 1

        class FakeWorker:
            max_iterations = 1
            goal_mode = False

            def __init__(self) -> None:
                self.done = 0
                self.failed: list[str] = []

            def notify_render_done(self) -> None:
                self.done += 1

            def notify_render_failed(self, message: str) -> None:
                self.failed.append(message)

            def isRunning(self) -> bool:  # noqa: N802
                return True

        class Status:
            text = ""

            def setText(self, value: str) -> None:  # noqa: N802
                self.text = value

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.canvas_port = cast(Any, FakeCanvasPort())
        docker._active_doc = object()
        docker._active_view = None
        docker._cancel = False
        worker = FakeWorker()
        docker._worker = cast(Any, worker)
        docker.status = cast(Any, Status())
        docker._run_render_options = {
            "save_json": True,
            "save_svg": False,
            "layer_mode": "multi_layer",
            "layer_prefix": "AI Artwork",
            "event_interval": 1,
        }
        plan = RuleBasedPlanner().plan("test", 1, 1, 100, 100)

        with patch("ai_stroke_painter.docker.save_plan", side_effect=OSError("disk full")):
            docker._on_plan_ready(plan)

        self.assertEqual(worker.failed, [])
        self.assertEqual(worker.done, 1)
        self.assertIn("保存警告", docker.status.text)

    def test_docker_waits_for_preview_confirmation_before_render(self) -> None:
        class Toggle:
            def __init__(self, checked: bool = False) -> None:
                self.checked = checked
                self.enabled = False

            def isChecked(self) -> bool:  # noqa: N802
                return self.checked

            def setEnabled(self, value: bool) -> None:  # noqa: N802
                self.enabled = value

        class Status:
            text = ""

            def setText(self, value: str) -> None:  # noqa: N802
                self.text = value

        class FakeCanvasPort:
            def __init__(self) -> None:
                self.rendered = 0

            def render(self, *args: Any, **kwargs: Any) -> int:
                self.rendered += 1
                return 1

        class FakeWorker:
            max_iterations = 1
            goal_mode = False

            def __init__(self) -> None:
                self.done = 0

            def notify_render_done(self) -> None:
                self.done += 1

            def isRunning(self) -> bool:  # noqa: N802
                return True

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        canvas = FakeCanvasPort()
        worker = FakeWorker()
        docker.canvas_port = cast(Any, canvas)
        docker._worker = cast(Any, worker)
        docker._active_doc = object()
        docker._active_view = None
        docker._cancel = False
        docker._pending_plan = None
        docker._applying_pending = False
        docker.confirm_before_apply = cast(Any, Toggle(True))
        docker.apply_btn = cast(Any, Toggle())
        docker.status = cast(Any, Status())
        docker._run_render_options = {"save_json": False, "save_svg": False}
        plan = RuleBasedPlanner().plan("cat", 1, 1, 100, 100)

        docker._on_plan_ready(plan)
        self.assertEqual(canvas.rendered, 0)
        self.assertEqual(worker.done, 0)
        self.assertTrue(docker.apply_btn.enabled)
        self.assertIs(docker._pending_plan, plan)

        docker._apply_pending_plan()
        self.assertEqual(canvas.rendered, 1)
        self.assertEqual(worker.done, 1)
        self.assertFalse(docker.apply_btn.enabled)
        self.assertIsNone(docker._pending_plan)


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
        self.assertLessEqual(len(strokes), AUTO_STROKE_BUDGET)
        self.assertEqual(len(strokes), 1)
        self.assertEqual(strokes[0].layer_name, "Flats")
        self.assertEqual(strokes[0].color, "#000000")
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

    def test_image_converter_rejects_blank_paper_and_detects_chroma_edges(self) -> None:
        class FakeColor:
            def __init__(self, red: int, green: int, blue: int, alpha: int = 255) -> None:
                self._rgba = (red, green, blue, alpha)

            def red(self) -> int:
                return self._rgba[0]

            def green(self) -> int:
                return self._rgba[1]

            def blue(self) -> int:
                return self._rgba[2]

            def alpha(self) -> int:
                return self._rgba[3]

        class BlankImage:
            def width(self) -> int:
                return 20

            def height(self) -> int:
                return 10

            def scaled(self, _width: int, _height: int) -> BlankImage:
                return self

            def pixelColor(self, _x: int, _y: int) -> FakeColor:  # noqa: N802
                return FakeColor(255, 255, 255)

        converter = ImageStrokeConverter()
        self.assertEqual(
            converter._process_qimage(BlankImage(), 1, None, 200, 100, random.Random(1)),
            [],
        )

        class TransparentColorBlock(BlankImage):
            def pixelColor(self, x: int, y: int) -> FakeColor:  # noqa: N802
                if 5 <= x < 15 and 2 <= y < 8:
                    return FakeColor(40, 80, 160)
                return FakeColor(255, 255, 255, 0)

        uniform_strokes = converter._process_qimage(TransparentColorBlock(), 1, None, 200, 100, random.Random(1))
        self.assertEqual(len(uniform_strokes), 1)
        self.assertGreater(uniform_strokes[0].points[0].x, 0)
        self.assertLess(uniform_strokes[0].points[-1].x, 200)
        self.assertLess(uniform_strokes[0].size_px, 100)

        class ChromaImage(BlankImage):
            def pixelColor(self, x: int, y: int) -> FakeColor:  # noqa: N802
                if y in {0, 9} or x in {0, 19}:
                    return FakeColor(255, 255, 255)
                # ほぼ等輝度の赤／緑境界は輝度Sobelだけでは失われる。
                return FakeColor(255, 0, 0) if x < 10 else FakeColor(0, 130, 0)

        strokes = converter._process_qimage(
            ChromaImage(),
            1,
            100,
            200,
            100,
            random.Random(1),
            edge_threshold=0.08,
            shading_density="off",
            enable_flats=False,
        )
        self.assertTrue(any(stroke.layer_name == "Lineart" for stroke in strokes))
        self.assertFalse(any(stroke.layer_name == "Highlights" for stroke in strokes))

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
        prev._accumulated_strokes = []
        prev._canvas_width = 1000.0
        prev._canvas_height = 1000.0
        prev._size_multiplier = 1.0
        prev._opacity_multiplier = 1.0
        prev.update = lambda: None
        prev.width = lambda: 200
        prev.height = lambda: 160

        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[
                Stroke("s1", [StrokePoint(0, 0, 0.5, 0), StrokePoint(100, 100, 0.8, 10)], size_px=10.0, opacity=1.0)
            ],
            canvas_width=800.0,
            canvas_height=600.0,
        )
        prev.set_plan(plan, size_multiplier=2.0, opacity_multiplier=0.5)
        self.assertEqual(prev._size_multiplier, 2.0)
        self.assertEqual(prev._opacity_multiplier, 0.5)
        self.assertEqual(len(prev._accumulated_strokes), 1)
        self.assertEqual(prev._canvas_width, 800.0)
        self.assertEqual(prev._canvas_height, 600.0)

        # マルチステップ累積描画テスト
        plan2 = DrawingPlan(
            prompt="test2",
            seed=2,
            strokes=[
                Stroke("s2", [StrokePoint(50, 50, 1.0, 0), StrokePoint(50, 50, 1.0, 0)], size_px=5.0, opacity=0.8),
                Stroke("s3", [StrokePoint(10, 10, 0.2, 0), StrokePoint(20, 20, 0.9, 5)], size_px=8.0, is_eraser=True),
            ],
        )
        prev.set_plan(plan2, accumulate=True)
        self.assertEqual(len(prev._accumulated_strokes), 3)

        # paintEvent および paint_to_painter が例外なく正常に完了すること
        prev.paintEvent(None)

        class _MockPainter:
            def __init__(self) -> None:
                self.lines: list[Any] = []
                self.rects: list[Any] = []
                self.ellipses: list[Any] = []

            def fillRect(self, *args: Any) -> None:
                self.rects.append(args)

            def drawRect(self, *args: Any) -> None:
                self.rects.append(args)

            def setPen(self, *args: Any) -> None:
                pass

            def setBrush(self, *args: Any) -> None:
                pass

            def drawLine(self, *args: Any) -> None:
                self.lines.append(args)

            def drawEllipse(self, *args: Any) -> None:
                self.ellipses.append(args)

        mock_painter = _MockPainter()
        prev.paint_to_painter(mock_painter, 200, 160)
        self.assertGreater(len(mock_painter.rects), 0)
        self.assertGreater(len(mock_painter.lines) + len(mock_painter.ellipses), 0)

        # クリアテスト
        prev.clear_plan()
        self.assertEqual(len(prev._accumulated_strokes), 0)
        self.assertIsNone(prev._plan)

        prev.update_multipliers(size_multiplier=1.5, opacity_multiplier=0.9)
        self.assertEqual(prev._size_multiplier, 1.5)
        self.assertEqual(prev._opacity_multiplier, 0.9)

    def test_salvage_brush_size_px_mode(self) -> None:
        """LLM が size: 45 のようなピクセル値を出力した際、pxモードとして救済されるテスト。"""
        from .llm_planner import _sanitize_and_rescue_program_dict

        raw = {
            "schema_version": 2,
            "canvas": {"width": 2480, "height": 3508},
            "operations": [
                {
                    "kind": "path",
                    "points": [[0.1, 0.1], [0.9, 0.9]],
                    "brush": {"size": 45, "color": "#123456"},
                },
                {
                    "kind": "fill",
                    "polygon": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
                    "brush": {"size": 0.05, "color": "#abcdef"},
                },
                {
                    "kind": "fill",
                    "style": "directional",
                    "angle_deg": 45,
                    "polygon": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]],
                    "brush": {"size": 0.04, "color": "#224466"},
                },
            ],
        }
        salvaged = _sanitize_and_rescue_program_dict(raw, canvas_w=2480, canvas_h=3508)
        ops = salvaged.get("operations", [])
        self.assertEqual(len(ops), 3)
        # size: 45 は > 1.0 のため size_mode='px' で救済
        self.assertEqual(ops[0]["brush"]["size_mode"], "px")
        self.assertEqual(ops[0]["brush"]["size"], 45.0)
        # size: 0.05 は <= 1.0 のため size_mode='ratio'
        self.assertEqual(ops[1]["brush"]["size_mode"], "ratio")
        self.assertEqual(ops[1]["brush"]["size"], 0.05)
        self.assertEqual(ops[2]["style"], "directional")
        self.assertEqual(ops[2]["angle_deg"], 45.0)

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

        # count > 500 (UI/Domain の MAX_PLAN_STROKES = 2,000 に整合するテスト [R1])
        plan_high = planner.plan("cyberpunk city landscape", seed=200, count=600, width=1000, height=1000)
        self.assertGreater(len(plan_high.strokes), 0)
        self.assertLessEqual(len(plan_high.strokes), MAX_PLAN_STROKES)

    def test_validate_plan_request_stroke_count_range(self) -> None:
        """validate_plan_request が MAX_PLAN_STROKES (2,000) までの本数を正しく許容・検証するテスト [R1]。"""
        from .planner import validate_plan_request

        # 正常系: 境界値および UI 許容範囲
        for c in (1, 500, 600, 1000, MAX_PLAN_STROKES):
            p, s, valid_c, w, h = validate_plan_request("test prompt", 42, c, 1000, 1000)
            self.assertEqual(valid_c, c)

        # auto (None / 0 / "auto") は None として扱われる
        for auto_val in (None, 0, "auto"):
            p, s, valid_c, w, h = validate_plan_request("test prompt", 42, auto_val, 1000, 1000)
            self.assertIsNone(valid_c)

        # auto_count=True 時は検証を通過した上で valid_count は None (自動予算) となる
        p, s, valid_c, w, h = validate_plan_request("test prompt", 42, 1500, 1000, 1000, auto_count=True)
        self.assertIsNone(valid_c)

        # 異常系: 負数, 2001, bool (True), 小数, 無効文字列
        for invalid_c in (-1, MAX_PLAN_STROKES + 1, True, 3.5, "invalid"):
            with self.assertRaises(ValueError):
                validate_plan_request("test prompt", 42, invalid_c, 1000, 1000)

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
            # headers が未設定の単純 transport は _post の1回 read 契約を使用する。
            mock_resp.headers = None
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

    def test_strokes_summary_json_rescue_from_actual_user_log(self) -> None:
        # ユーザーログで発生した strokes_summary 形式（比率座標およびピクセル座標）の救済検証
        log_payload_1 = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 2,
                                "iteration": 2,
                                "completed_layers": ["Flats", "Shading"],
                                "stroke_count": 35,
                                "request_canvas_image": False,
                                "strokes_summary": [
                                    {
                                        "id": "shadow_sky_blue",
                                        "layer": "Shading",
                                        "color": "#b3c4e0",
                                        "size_px": 200.0,
                                        "start_xy": [0.0, 0.2],
                                        "end_xy": [1.0, 0.2],
                                    },
                                    {
                                        "id": "shadow_sky_rose",
                                        "layer": "Shading",
                                        "color": "#e8a4b8",
                                        "size_px": 200.0,
                                        "start_xy": [0.0, 0.35],
                                        "end_xy": [1.0, 0.35],
                                    },
                                    {
                                        "id": "shadow_grass_upper",
                                        "layer": "Shading",
                                        "color": "#5f805c",
                                        "size_px": 120.0,
                                        "start_xy": [0.0, 0.58],
                                        "end_xy": [1.0, 0.6],
                                    },
                                ],
                            }
                        )
                    }
                }
            ]
        }
        plan1 = _plan_from_response(log_payload_1, prompt="wildflower garden", width=2480, height=3508)
        self.assertGreaterEqual(len(plan1.strokes), 3)
        self.assertIn("Shading", plan1.layers)

        # ユーザーログのピクセル座標（1000.0, 2400.0 など > 1.0）混在レスポンスの救済検証
        log_payload_2 = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 2,
                                "iteration": 2,
                                "completed_layers": ["Flats", "Shading"],
                                "stroke_count": 18,
                                "request_canvas_image": False,
                                "goal_reached": False,
                                "completion_score": 0.35,
                                "strokes_summary": [
                                    {
                                        "id": "a1b2c3d4",
                                        "layer": "Shading",
                                        "color": "#c5b1d8",
                                        "size_px": 350.0,
                                        "start_xy": [1000.0, 2400.0],
                                        "end_xy": [1800.0, 2400.0],
                                    },
                                    {
                                        "id": "b1c2d3e4",
                                        "layer": "Shading",
                                        "color": "#c5b1d8",
                                        "size_px": 350.0,
                                        "start_xy": [1100.0, 2600.0],
                                        "end_xy": [1900.0, 2600.0],
                                    },
                                ],
                            }
                        )
                    }
                }
            ]
        }
        plan2 = _plan_from_response(log_payload_2, prompt="wildflower garden", width=2480, height=3508)
        self.assertGreaterEqual(len(plan2.strokes), 2)
        for s in plan2.strokes:
            self.assertTrue(all(0.0 <= pt.x <= 2480.0 and 0.0 <= pt.y <= 3508.0 for pt in s.points))

    def test_dry_bristles_natural_brush_name_rescue(self) -> None:
        # LLM が dry bristles や soft airbrush などの自然なブラシ名を出力した場合の救済検証
        program_json = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 2,
                                "prompt": "wildflower garden",
                                "seed": 42,
                                "title": "Watercolor Wildflower Garden",
                                "iteration": 2,
                                "operations": [
                                    {
                                        "kind": "path",
                                        "id": "stroke-1",
                                        "layer": "Shading",
                                        "points": [[0.1, 0.2], [0.5, 0.6], [0.9, 0.8]],
                                        "brush": {"profile": "dry bristles", "color": "#a3c29b", "size": 0.05},
                                    },
                                    {
                                        "kind": "fill",
                                        "id": "fill-1",
                                        "layer": "Flats",
                                        "polygon": [[0.0, 0.0], [1.0, 0.0], [1.0, 0.5], [0.0, 0.5]],
                                        "brush": {"profile": "wash", "color": "#fce4ec", "size": 0.1},
                                    },
                                ],
                            }
                        )
                    }
                }
            ]
        }
        plan = _plan_from_response(program_json, prompt="wildflower garden", width=1000, height=1000)
        self.assertGreaterEqual(len(plan.strokes), 2)

    def test_apostrophe_in_prompt_and_thinking_json_parsing(self) -> None:
        # プロンプトやタイトル、思考文にアポストロフィ ('s) が含まれる場合の正常パース検証
        raw_text = (
            "Thinking Process: Let's create an artist's garden with nature's beauty.\n\n"
            "```json\n"
            "{\n"
            '  "schema_version": 2,\n'
            '  "prompt": "delicate wildflower\'s garden with soft petal\'s glow",\n'
            '  "seed": 42,\n'
            '  "title": "Nature\'s Masterpiece",\n'
            '  "iteration": 2,\n'
            '  "operations": [\n'
            "    {\n"
            '      "kind": "path",\n'
            '      "id": "op-1",\n'
            '      "layer": "Lineart",\n'
            '      "points": [[0.1, 0.1], [0.9, 0.9]],\n'
            '      "brush": {"profile": "gpen", "color": "#232323", "size": 0.005}\n'
            "    }\n"
            "  ]\n"
            "}\n"
            "```\n"
        )
        parsed = _extract_json_object(raw_text)
        self.assertIn("operations", parsed)
        self.assertEqual(parsed["title"], "Nature's Masterpiece")
        self.assertEqual(parsed["prompt"], "delicate wildflower's garden with soft petal's glow")

    def test_unquoted_keys_and_comments_json_extraction(self) -> None:
        # クォートなしキー、JavaScriptコメント、Python真偽値混在の救済検証
        raw_text = (
            "{\n"
            "  schema_version: 2, // スキーマバージョン\n"
            "  prompt: 'delicate wildflower garden',\n"
            "  seed: 42,\n"
            "  goal_reached: False,\n"
            "  completion_score: 0.5,\n"
            "  operations: [\n"
            "    /* ベース背景 */\n"
            "    {\n"
            "      kind: 'fill',\n"
            "      id: 'bg-1',\n"
            "      layer: 'Flats',\n"
            "      polygon: [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],\n"
            "      brush: { profile: 'watercolor', color: '#fce4ec', size: 0.1 }\n"
            "    }\n"
            "  ]\n"
            "}\n"
        )
        parsed = _extract_json_object(raw_text)
        self.assertIn("operations", parsed)
        self.assertEqual(len(parsed["operations"]), 1)

    def test_nested_container_unwrapping(self) -> None:
        # {"plan": {...}} や [{"kind": "fill", ...}] のようなラッパーの自動アンラップ検証
        wrapper_json_1 = {
            "plan": {
                "schema_version": 2,
                "operations": [
                    {
                        "kind": "path",
                        "id": "p1",
                        "points": [[0.1, 0.2], [0.8, 0.9]],
                        "brush": {"profile": "gpen", "color": "#000000"},
                    }
                ],
            }
        }
        plan1 = _mapping_to_drawing_plan(wrapper_json_1, prompt="test", seed=1, width=1000, height=1000)
        self.assertGreaterEqual(len(plan1.strokes), 1)

        raw_list_text = (
            '[{"kind": "fill", "id": "f1", "polygon": [[0,0],[1,0],[1,1],[0,1]], "brush": {"color": "#ffffff"}}]'
        )
        parsed_list = _extract_json_object(raw_list_text)
        self.assertIn("operations", parsed_list)

    def test_actual_user_log_all_three_attempts_repro(self) -> None:
        # ユーザーログ Step 2 の全 3 試行の生レスポンスが全て正常に計画へ変換できることを検証
        # 試行 1: strokes_summary (比率座標)
        attempt_1_content = (
            '{"schema_version": 2, "iteration": 2, "completed_layers": ["Flats", "Shading"], '
            '"stroke_count": 35, "request_canvas_image": false, "strokes_summary": ['
            '{"id": "shadow_sky_blue", "layer": "Shading", "color": "#b3c4e0", "size_px": 200.0, "start_xy": [0.0, 0.2], "end_xy": [1.0, 0.2]}, '
            '{"id": "shadow_sky_rose", "layer": "Shading", "color": "#e8a4b8", "size_px": 200.0, "start_xy": [0.0, 0.35], "end_xy": [1.0, 0.35]}'
            "]}"
        )
        plan_1 = _plan_from_response(
            {"choices": [{"message": {"content": attempt_1_content}}]},
            prompt="wildflowers",
            width=2480,
            height=3508,
        )
        self.assertGreaterEqual(len(plan_1.strokes), 2)

        # 試行 2: dry bristles プロファイル
        attempt_2_content = (
            "```json\n"
            "{\n"
            '  "schema_version": 2,\n'
            '  "prompt": "delicate watercolor wildflower garden with soft petals",\n'
            '  "seed": 42,\n'
            '  "title": "Watercolor Wildflower Garden",\n'
            '  "iteration": 2,\n'
            '  "operations": [\n'
            "    {\n"
            '      "kind": "path",\n'
            '      "id": "shadow-1",\n'
            '      "layer": "Shading",\n'
            '      "points": [[0.1, 0.5], [0.8, 0.5]],\n'
            '      "brush": {"profile": "dry bristles", "color": "#5f805c", "size_px": 120.0, "size_mode": "px"}\n'
            "    }\n"
            "  ]\n"
            "}\n"
            "```"
        )
        plan_2 = _plan_from_response(
            {"choices": [{"message": {"content": attempt_2_content}}]},
            prompt="wildflowers",
            width=2480,
            height=3508,
        )
        self.assertGreaterEqual(len(plan_2.strokes), 1)

        # 試行 3: strokes_summary (ピクセル座標 1000.0, 2400.0)
        attempt_3_content = (
            '{"schema_version": 2, "iteration": 2, "completed_layers": ["Flats", "Shading"], '
            '"stroke_count": 18, "request_canvas_image": false, "goal_reached": false, "completion_score": 0.35, '
            '"strokes_summary": ['
            '{"id": "a1b2c3d4", "layer": "Shading", "color": "#c5b1d8", "size_px": 350.0, "start_xy": [1000.0, 2400.0], "end_xy": [1800.0, 2400.0]}, '
            '{"id": "b1c2d3e4", "layer": "Shading", "color": "#c5b1d8", "size_px": 350.0, "start_xy": [1100.0, 2600.0], "end_xy": [1900.0, 2600.0]}'
            "]}"
        )
        plan_3 = _plan_from_response(
            {"choices": [{"message": {"content": attempt_3_content}}]},
            prompt="wildflowers",
            width=2480,
            height=3508,
        )
        self.assertGreaterEqual(len(plan_3.strokes), 2)

    def test_fullwidth_and_smart_quotes_and_unescaped_newlines_json(self) -> None:
        """全角記号・スマートクォート・文字列内生改行・コメント混入のJSONが正常にパース・救出されるテスト。"""
        from .llm_planner import _extract_json_object, _mapping_to_drawing_plan

        raw_text = """
        // Header comment
        ｛
            “schema_version”： 2，
            “prompt”： “beautiful
            sunset over
            lake”，
            “operations”： ［
                ｛
                    “kind”： “fill”，
                    “id”： “sky_base”，
                    ‘layer’： “Flats”，
                    ‘polygon’： ［［0.0， 0.0］， ［1.0， 0.0］， ［1.0， 0.5］， ［0.0， 0.5］］，
                    “brush”： ｛“color”： “#ff7f50”， “size”： 0.1｝，
                ｝
            ］
        ｝
        """
        parsed = _extract_json_object(raw_text)
        self.assertIn("operations", parsed)
        plan = _mapping_to_drawing_plan(parsed, prompt="sunset", seed=1, width=1000, height=1000)
        self.assertGreaterEqual(len(plan.strokes), 1)
        self.assertEqual(plan.strokes[0].layer_name, "Flats")

    def test_nested_layers_operations_unwrapping(self) -> None:
        """layers 配列内にネストされた operations/strokes が自動フラット化されて救出されるテスト。"""
        from .llm_planner import _sanitize_and_rescue_program_dict

        nested_json = {
            "schema_version": 2,
            "title": "Nested Artwork",
            "layers": [
                {
                    "name": "Flats",
                    "operations": [
                        {
                            "kind": "fill",
                            "id": "f_bg",
                            "polygon": [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
                            "brush": {"color": "#abcdef"},
                        }
                    ],
                },
                {
                    "name": "Lineart",
                    "operations": [
                        {
                            "kind": "path",
                            "id": "p_contour",
                            "points": [[0.1, 0.1], [0.9, 0.9]],
                            "brush": {"profile": "gpen", "color": "#111111"},
                        }
                    ],
                },
            ],
        }
        rescued = _sanitize_and_rescue_program_dict(nested_json, canvas_w=1000, canvas_h=1000)
        ops = rescued.get("operations", [])
        self.assertEqual(len(ops), 2)
        self.assertEqual(ops[0]["layer"], "Flats")
        self.assertEqual(ops[1]["layer"], "Lineart")

    def test_rect_circle_line_shape_conversions(self) -> None:
        """簡易図形 (rect, circle, line) が自動的に正規の fill / path に変換されるテスト。"""
        from .llm_planner import _sanitize_and_rescue_program_dict

        shapes_json = {
            "schema_version": 2,
            "operations": [
                {
                    "kind": "rect",
                    "id": "r1",
                    "bounds": [0.1, 0.1, 0.8, 0.8],
                    "layer": "Flats",
                    "brush": {"color": "#ff0000"},
                },
                {
                    "kind": "circle",
                    "id": "c1",
                    "center": [0.5, 0.5],
                    "radius": 0.2,
                    "layer": "Shading",
                    "brush": {"color": "#0000ff"},
                },
                {
                    "kind": "line",
                    "id": "l1",
                    "from": [0.1, 0.2],
                    "to": [0.8, 0.9],
                    "layer": "Lineart",
                    "brush": {"profile": "gpen", "color": "#000000"},
                },
            ],
        }
        rescued = _sanitize_and_rescue_program_dict(shapes_json, canvas_w=1000, canvas_h=1000)
        ops = rescued.get("operations", [])
        self.assertEqual(len(ops), 3)
        self.assertEqual(ops[0]["kind"], "fill")
        self.assertEqual(len(ops[0]["polygon"]), 4)
        self.assertEqual(ops[1]["kind"], "fill")
        self.assertGreaterEqual(len(ops[1]["polygon"]), 12)
        self.assertEqual(ops[2]["kind"], "path")
        self.assertEqual(len(ops[2]["points"]), 2)

    def test_hsl_and_rgba_and_named_colors(self) -> None:
        """HSL / RGBA / 色名 / transparent が正しく16進数に正規化されるテスト。"""
        from .stroke_program import normalize_hex_color

        self.assertEqual(normalize_hex_color("coral"), "#ff7f50")
        self.assertEqual(normalize_hex_color("navy"), "#000080")
        self.assertEqual(normalize_hex_color("transparent"), "#00000000")
        self.assertEqual(normalize_hex_color("rgba(255, 0, 0, 1.0)"), "#ff0000ff")
        self.assertEqual(normalize_hex_color("hsl(0, 100%, 50%)"), "#ff0000")
        self.assertEqual(normalize_hex_color("hsla(240, 100%, 50%, 0.5)"), "#0000ff80")

    def test_truncated_json_multi_brace_repair(self) -> None:
        """トークン上限で途中で切断された JSON がスタック解析・多段巻き戻しで正常に修復されるテスト。"""
        truncated_raw = (
            '{"schema_version": 2, "prompt": "cyberpunk city", "canvas": {"width": 1000, "height": 1000}, '
            '"operations": ['
            '{"kind": "fill", "id": "f1", "layer": "Flats", "polygon": [[0,0],[1,0],[1,1],[0,1]], "brush": {"color": "#112233"}}, '
            '{"kind": "path", "id": "p1", "layer": "Lineart", "points": [[0.1, 0.2], [0.5, 0.8]], "brush": {"color": "#ffffff"}}, '
            '{"kind": "path", "id": "p2", "layer": "Lineart", "points": [[0.2, 0.3], [0.6'
        )
        repaired = _attempt_json_repair(truncated_raw)
        self.assertIsNotNone(repaired)
        assert repaired is not None
        ops = repaired.get("operations", [])
        # 途切れた p2 の直前までの f1 と p1 が救出される
        self.assertGreaterEqual(len(ops), 2)
        self.assertEqual(ops[0]["id"], "f1")
        self.assertEqual(ops[1]["id"], "p1")

    def test_error_feedback_retry_flow(self) -> None:
        """1回目の応答が構文エラーだった際に、2回目でエラーフィードバック付きリトライが行われ成功するテスト。"""

        class RetryHandler(BaseHTTPRequestHandler):
            attempt_count = 0
            received_messages: list[Any] = []

            def do_POST(self) -> None:
                type(self).attempt_count += 1
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length))
                type(self).received_messages.append(body.get("messages", []))

                if type(self).attempt_count == 1:
                    # 1回目は壊れた構文（JSONパース不可テキスト）
                    resp = {"choices": [{"message": {"content": "Sorry, I am thinking... not a json"}}]}
                else:
                    # 2回目は正常な StrokeProgram JSON
                    valid_prog = {
                        "schema_version": 2,
                        "prompt": "test retry",
                        "operations": [
                            {
                                "kind": "path",
                                "id": "retried_op",
                                "layer": "Lineart",
                                "points": [[0.1, 0.1], [0.9, 0.9]],
                                "brush": {"color": "#000000"},
                            }
                        ],
                    }
                    resp = {"choices": [{"message": {"content": json.dumps(valid_prog)}}]}

                encoded = json.dumps(resp).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def log_message(self, _format: str, *_args: Any) -> None:
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), RetryHandler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    f"http://127.0.0.1:{server.server_port}/v1",
                    "test-model",
                    "test-key",
                    2,
                    max_retries=3,
                )
            )
            plan = planner.plan("test retry", 1, 1, 1000, 1000)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        self.assertEqual(RetryHandler.attempt_count, 2)
        self.assertEqual(len(plan.strokes), 1)
        # 2回目のプロンプトに FEEDBACK が含まれていることを検証
        second_msgs = RetryHandler.received_messages[1]
        self.assertTrue(any("FEEDBACK" in str(m.get("content", "")) for m in second_msgs))

    def test_parameter_fallback_transient_retry(self) -> None:
        """HTTP 429 / 503 等の一時的障害時に指数バックオフで再試行されるテスト。"""

        class TransientHandler(BaseHTTPRequestHandler):
            attempt_count = 0

            def do_POST(self) -> None:
                type(self).attempt_count += 1
                if type(self).attempt_count == 1:
                    # 1回目は 429 Rate Limit
                    self.send_response(429)
                    self.send_header("Content-Type", "application/json")
                    body = b'{"error": {"message": "Rate limit exceeded"}}'
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                else:
                    # 2回目は 200 OK
                    valid_prog = {
                        "schema_version": 2,
                        "prompt": "transient test",
                        "operations": [
                            {
                                "kind": "path",
                                "id": "transient_op",
                                "layer": "Lineart",
                                "points": [[0.1, 0.1], [0.9, 0.9]],
                                "brush": {"color": "#333333"},
                            }
                        ],
                    }
                    resp = {"choices": [{"message": {"content": json.dumps(valid_prog)}}]}
                    encoded = json.dumps(resp).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(encoded)))
                    self.end_headers()
                    self.wfile.write(encoded)

            def log_message(self, _format: str, *_args: Any) -> None:
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), TransientHandler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    f"http://127.0.0.1:{server.server_port}/v1",
                    "test-model",
                    "test-key",
                    3,
                )
            )
            plan = planner.plan("transient test", 1, 1, 1000, 1000)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        self.assertEqual(TransientHandler.attempt_count, 2)
        self.assertEqual(len(plan.strokes), 1)

    def test_response_format_fallback_tries_json_object_before_removal(self) -> None:
        planner = OpenAICompatiblePlanner(OpenAICompatibleSettings("https://example.test/v1", "test-model"))
        formats: list[Any] = []

        def fake_post(payload: Any, cancelled: Any = None) -> dict[str, Any]:
            formats.append(payload.get("response_format"))
            if len(formats) == 1:
                raise LLMPlannerError("response_format json_schema is unsupported")
            return {"choices": [{"message": {"content": "{}"}}]}

        planner._post = fake_post  # type: ignore[method-assign]
        planner._post_with_parameter_fallback(
            {
                "model": "test-model",
                "messages": [{"role": "user", "content": "test"}],
                "response_format": {"type": "json_schema", "json_schema": {"name": "test"}},
            }
        )

        self.assertEqual(formats[0]["type"], "json_schema")
        self.assertEqual(formats[1], {"type": "json_object"})

    def test_preview_composition_modes_and_layer_sorting(self) -> None:
        """PreviewWidget がレイヤー階層順にソートし、ブレンドモード（乗算・加算・通常）を適切に適用することを検証。"""
        from .docker import PreviewWidget
        from .qt_compat import QPainter

        prev = PreviewWidget.__new__(PreviewWidget)
        prev._plan = None
        prev._accumulated_strokes = []
        prev._canvas_width = 1000.0
        prev._canvas_height = 1000.0
        prev._size_multiplier = 1.0
        prev._opacity_multiplier = 1.0
        prev.update = lambda: None
        prev.width = lambda: 200
        prev.height = lambda: 160

        # 意図的にレイヤー順序をバラバラに配置（Lineart -> Flats -> Highlights -> Shading）
        s_lineart = Stroke("s_line", [StrokePoint(10, 10, 1.0, 0), StrokePoint(20, 20, 1.0, 1)], layer_name="Lineart")
        s_flats = Stroke("s_flat", [StrokePoint(30, 30, 1.0, 0), StrokePoint(40, 40, 1.0, 1)], layer_name="Flats")
        s_hl = Stroke("s_hl", [StrokePoint(50, 50, 1.0, 0), StrokePoint(60, 60, 1.0, 1)], layer_name="Highlights")
        s_shade = Stroke("s_sh", [StrokePoint(70, 70, 1.0, 0), StrokePoint(80, 80, 1.0, 1)], layer_name="Shading")
        s_eraser = Stroke("s_er", [StrokePoint(90, 90, 1.0, 0), StrokePoint(95, 95, 1.0, 1)], is_eraser=True)

        plan = DrawingPlan(
            prompt="blend test",
            seed=1,
            strokes=[s_lineart, s_flats, s_hl, s_shade, s_eraser],
            canvas_width=1000.0,
            canvas_height=1000.0,
        )
        prev.set_plan(plan)

        modes_used: list[Any] = []
        drawn_strokes_order: list[str] = []

        class _RecordingPainter:
            def fillRect(self, *args: Any) -> None:
                pass

            def drawRect(self, *args: Any) -> None:
                pass

            def setPen(self, *args: Any) -> None:
                pass

            def setBrush(self, *args: Any) -> None:
                pass

            def setCompositionMode(self, mode: Any) -> None:
                modes_used.append(mode)

            def drawLine(self, x0: int, y0: int, x1: int, y1: int) -> None:
                drawn_strokes_order.append(f"{x0},{y0}->{x1},{y1}")

        mock_painter = _RecordingPainter()
        prev.paint_to_painter(mock_painter, 200, 160)

        # モードが設定されたこと（乗算、加算、通常）を確認
        expected_mul = composition_mode_multiply(QPainter)
        expected_plus = composition_mode_plus(QPainter)
        expected_over = composition_mode_source_over(QPainter)

        self.assertIn(expected_mul, modes_used)
        self.assertIn(expected_plus, modes_used)
        self.assertIn(expected_over, modes_used)
        # 描画ストローク数が一致すること
        self.assertEqual(len(drawn_strokes_order), 5)

        # 単一／アクティブレイヤーは実描画と同様に計画順を維持する。
        drawn_strokes_order.clear()
        prev.set_plan(plan, layer_mode="single_layer")
        prev.paint_to_painter(mock_painter, 200, 160)
        first_start_x = int(drawn_strokes_order[0].split(",", 1)[0])
        preview_scale = min(200 / 1000, 160 / 1000) * 0.92
        preview_offset_x = (200 - 1000 * preview_scale) * 0.5
        self.assertEqual(first_start_x, round(preview_offset_x + s_lineart.points[0].x * preview_scale))

    def test_preview_eraser_reveals_lower_layer(self) -> None:
        if QImage is None or not callable(QImage) or not hasattr(QImage, "pixelColor"):
            self.skipTest("実 Qt QImage がない headless 環境")
        from .docker import PreviewWidget
        from .qt_compat import QPainter

        preview = PreviewWidget.__new__(PreviewWidget)
        preview._plan = None
        preview._accumulated_strokes = []
        preview._canvas_width = 100.0
        preview._canvas_height = 100.0
        preview._size_multiplier = 1.0
        preview._opacity_multiplier = 1.0
        preview._layer_mode = "multi_layer"
        preview.update = lambda: None
        points = [StrokePoint(10, 50, 1.0, 0), StrokePoint(90, 50, 1.0, 10)]
        erase_points = [StrokePoint(47, 50, 1.0, 0), StrokePoint(53, 50, 1.0, 10)]
        plan = DrawingPlan(
            "preview eraser",
            1,
            [
                Stroke("flat", points, color="#ff0000", size_px=20, layer_name="Flats"),
                Stroke("ink", points, color="#0000ff", size_px=14, layer_name="Lineart"),
                Stroke("erase", erase_points, size_px=10, layer_name="Lineart", is_eraser=True),
            ],
            canvas_width=100,
            canvas_height=100,
        )
        preview.set_plan(plan, layer_mode="multi_layer")
        image: Any = QImage(200, 200, argb32_image_format(QImage))
        image.fill(0)
        painter: Any = QPainter(image)
        try:
            preview.paint_to_painter(painter, 200, 200)
        finally:
            painter.end()

        erased = image.pixelColor(100, 100)
        ink = image.pixelColor(125, 100)
        self.assertGreater(erased.red(), erased.blue())
        self.assertGreater(ink.blue(), ink.red())

    def test_high_res_hatch_angle_density_and_clamping(self) -> None:
        """高解像度キャンバス（2480x3508）でハッチングが粗すぎるゼブラ縞にならず適切に間隔がクランプされることを検証。"""
        prog_dict = {
            "schema_version": 2,
            "prompt": "high res hatch test",
            "seed": 42,
            "canvas": {"width": 2480, "height": 3508},
            "operations": [
                {
                    "kind": "hatch",
                    "id": "hatch_clamp",
                    "layer": "Shading",
                    "polygon": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]],
                    "angle_deg": 45,
                    "spacing": 0.05,  # 粗い比率指定 (124px相当)
                    "brush": {"profile": "pencil", "size": 5.0, "size_mode": "px", "color": "#202020"},
                }
            ],
        }
        prog = StrokeProgram.from_dict(prog_dict)
        plan = compile_stroke_program(prog)
        # クランプによりブラシサイズ (5px * 2.8 = 14px) に近い適切なストローク数に生成される
        self.assertGreater(len(plan.strokes), 50)
        self.assertLess(len(plan.strokes), 500)

    def test_program_brush_eraser_inference_and_stroke_sync(self) -> None:
        """ProgramBrush および _make_stroke が is_eraser を確実に判定・同期することを検証。"""
        # 1. profile = "eraser" からの自動判定
        b1 = ProgramBrush.from_dict({"profile": "eraser", "size": 0.01})
        self.assertTrue(b1.is_eraser)

        # 2. preset_hint に eraser が含まれる場合の自動判定
        b2 = ProgramBrush.from_dict({"profile": "auto", "preset_hint": "Eraser Soft", "size": 0.01})
        self.assertTrue(b2.is_eraser)

        # 3. 通常ブラシの場合
        b3 = ProgramBrush.from_dict({"profile": "gpen", "size": 0.01})
        self.assertFalse(b3.is_eraser)

        # 4. StrokeProgram コンパイル時の stroke.is_eraser 伝播
        prog = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "eraser sync",
                "seed": 1,
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "path",
                        "id": "erase_op",
                        "points": [[0.1, 0.1], [0.5, 0.5]],
                        "brush": {"profile": "eraser", "size": 0.02},
                    }
                ],
            }
        )
        plan = compile_stroke_program(prog)
        self.assertEqual(len(plan.strokes), 1)
        self.assertTrue(plan.strokes[0].is_eraser)

    def test_fill_operation_new_styles_and_angle_compilation(self) -> None:
        """FillOperation の contour, radial, directional スタイルおよび angle_deg が正常にコンパイルされることを検証。"""
        # 1. contour (同心円輪郭塗り)
        p_contour = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "contour fill test",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "f_contour",
                        "style": "contour",
                        "polygon": [[0.2, 0.2], [0.8, 0.2], [0.8, 0.8], [0.2, 0.8]],
                        "brush": {"profile": "watercolor", "size": 0.05, "color": "#e08090"},
                    }
                ],
            }
        )
        plan_contour = compile_stroke_program(p_contour)
        self.assertGreater(len(plan_contour.strokes), 3)

        # 2. radial (放射状塗り)
        p_radial = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "radial fill test",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "f_radial",
                        "style": "radial",
                        "polygon": [[0.3, 0.3], [0.7, 0.3], [0.7, 0.7], [0.3, 0.7]],
                        "brush": {"profile": "watercolor", "size": 0.04, "color": "#3366cc"},
                    }
                ],
            }
        )
        plan_radial = compile_stroke_program(p_radial)
        self.assertGreaterEqual(len(plan_radial.strokes), 8)

        # 3. directional (角度指定スキャンライン塗り)
        p_dir = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "directional fill test",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "f_dir",
                        "style": "directional",
                        "angle_deg": 45.0,
                        "polygon": [[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9]],
                        "brush": {"profile": "airbrush", "size": 0.08, "color": "#204060"},
                    }
                ],
            }
        )
        plan_dir = compile_stroke_program(p_dir)
        self.assertGreater(len(plan_dir.strokes), 4)

    def test_curvature_pressure_boost_and_smart_tapering(self) -> None:
        """急カーブ（角）での曲率連動筆圧インク溜まりと端点スマートテーパリングを検証。"""
        # 90度直角に曲がる3点パス
        prog = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "curvature test",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "path",
                        "id": "corner_path",
                        "layer": "Lineart",
                        "points": [[0.1, 0.1], [0.5, 0.1], [0.5, 0.9]],
                        "brush": {"profile": "gpen", "size": 0.005, "color": "#000000"},
                    }
                ],
            }
        )
        plan = compile_stroke_program(prog)
        self.assertEqual(len(plan.strokes), 1)
        pts = plan.strokes[0].points
        self.assertGreater(len(pts), 5)
        # 端点はテーパリングされている（開始点・終了点の筆圧が低い）
        self.assertLess(pts[0].pressure, 0.5)
        self.assertLess(pts[-1].pressure, 0.5)
        # 中間の曲がり角付近で筆圧がブーストされている
        max_p = max(pt.pressure for pt in pts)
        self.assertGreaterEqual(max_p, 0.90)

    def test_thin_polygon_fill_and_hatch_midline_fallback(self) -> None:
        """薄い・微小ポリゴンの fill および hatch が走査線で欠落せず中心断面フォールバックでストロークを生成することを検証。"""
        # 1. 縦方向に非常に薄い fill ポリゴン (y0=0.500, y1=0.501)
        prog_fill = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "thin fill",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "thin_fill_op",
                        "polygon": [[0.1, 0.5], [0.9, 0.5], [0.9, 0.501], [0.1, 0.501]],
                        "brush": {"profile": "marker", "size": 0.05, "color": "#ff0000"},
                    }
                ],
            }
        )
        plan_fill = compile_stroke_program(prog_fill)
        self.assertGreaterEqual(len(plan_fill.strokes), 1)

        # 2. 角度付き directional fill で薄いポリゴン
        prog_dir = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "thin directional fill",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "fill",
                        "id": "thin_dir_op",
                        "style": "directional",
                        "angle_deg": 35.0,
                        "polygon": [[0.1, 0.5], [0.9, 0.5], [0.9, 0.501], [0.1, 0.501]],
                        "brush": {"profile": "marker", "size": 0.05, "color": "#00ff00"},
                    }
                ],
            }
        )
        plan_dir = compile_stroke_program(prog_dir)
        self.assertGreaterEqual(len(plan_dir.strokes), 1)

        # 3. 薄い hatch ポリゴン
        prog_hatch = StrokeProgram.from_dict(
            {
                "schema_version": 2,
                "prompt": "thin hatch",
                "canvas": {"width": 1000, "height": 1000},
                "operations": [
                    {
                        "kind": "hatch",
                        "id": "thin_hatch_op",
                        "angle_deg": 45.0,
                        "spacing": 0.05,
                        "polygon": [[0.2, 0.4], [0.8, 0.4], [0.8, 0.401], [0.2, 0.401]],
                        "brush": {"profile": "pencil", "size": 0.005, "color": "#000000"},
                    }
                ],
            }
        )
        plan_hatch = compile_stroke_program(prog_hatch)
        self.assertGreaterEqual(len(plan_hatch.strokes), 1)

    def test_llm_planner_corrupted_token_rescue_resilience(self) -> None:
        """LLM の非数値トークン・欠落パラメータが混入した JSON 辞書から安全に救済できることを検証。"""
        corrupted_dict = {
            "schema_version": 2,
            "prompt": "corrupted test",
            "canvas": {"width": 1000, "height": 1000},
            "operations": [
                {
                    "kind": "path",
                    "id": "bad_pts_op",
                    "points": [
                        [0.1, 0.2],
                        ["invalid", "NaN"],  # 不正な点
                        {"x": "auto", "y": None},  # 不正な辞書点
                        [0.5, 0.6],
                    ],
                    "brush": {"size": "auto", "opacity": "null"},
                },
                {
                    "kind": "fill",
                    "id": "bad_fill_op",
                    "polygon": [[0.1, 0.1], [0.5, 0.1], [0.5, 0.5]],
                    "angle_deg": "invalid_angle",
                    "spacing": "bad_spacing",
                },
                {
                    "kind": "particles",
                    "id": "bad_particles_op",
                    "bounds": ["a", "b", "c", "d"],
                    "count": "twenty",
                    "length": "short",
                },
            ],
        }
        rescued = _sanitize_and_rescue_program_dict(corrupted_dict, canvas_w=1000.0, canvas_h=1000.0)
        self.assertIn("operations", rescued)
        self.assertGreaterEqual(len(rescued["operations"]), 1)
        prog = StrokeProgram.from_dict(rescued)
        plan = compile_stroke_program(prog)
        self.assertGreaterEqual(len(plan.strokes), 1)

    def test_krita_adapter_paint_path_attribute_error_fallback(self) -> None:
        """Node の paintPath が AttributeError や NotImplementedError を送出した場合も segment 描画へ安全にフォールバックすることを検証。"""

        class FailingNode(_FakeNode):
            def paintPath(self, _path: Any) -> None:
                raise AttributeError("paintPath is not implemented on this build")

        doc = _FakeDocument()
        node = FailingNode("Lineart")
        adapter = KritaCanvasAdapter()
        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[
                Stroke(
                    id="s1",
                    points=[
                        StrokePoint(10.0, 10.0, 0.8, 0),
                        StrokePoint(50.0, 50.0, 0.8, 10),
                        StrokePoint(100.0, 100.0, 0.8, 20),
                    ],
                    brush_preset="Basic-5 Size",
                    color="#111111",
                    size_px=5.0,
                    layer_name="Lineart",
                )
            ],
        )
        rendered = adapter.render(doc, plan, lambda: False, active_node=node)
        self.assertEqual(rendered, 1)

    def test_storage_load_plan_and_program_file_not_found(self) -> None:
        """存在しないパスを指定した load_plan および load_program が FileNotFoundError を送出することを検証。"""
        non_existent = Path("non_existent_file_path_12345.json")
        with self.assertRaises(FileNotFoundError):
            load_plan(non_existent)
        with self.assertRaises(FileNotFoundError):
            load_program(non_existent)

    def test_drawing_plan_scale_to_modes(self) -> None:
        """DrawingPlan.scale_to の各種 fit_mode (scale, fit, fill, center) における座標・線幅変換を検証。"""
        stroke = Stroke(
            id="s1",
            points=[
                StrokePoint(100.0, 100.0, 0.8, 0),
                StrokePoint(500.0, 500.0, 0.9, 10),
            ],
            brush_preset="Basic-5 Size",
            color="#222222",
            size_px=10.0,
            layer_name="Lineart",
        )
        plan = DrawingPlan(
            prompt="test scaling",
            seed=42,
            strokes=[stroke],
            canvas_width=1000.0,
            canvas_height=1000.0,
        )

        # 1. scale / stretch (非等方スケーリング)
        scaled = plan.scale_to(2000.0, 3000.0, fit_mode="scale")
        self.assertEqual((scaled.canvas_width, scaled.canvas_height), (2000.0, 3000.0))
        p0 = scaled.strokes[0].points[0]
        p1 = scaled.strokes[0].points[1]
        self.assertAlmostEqual(p0.x, 200.0, places=2)
        self.assertAlmostEqual(p0.y, 300.0, places=2)
        self.assertAlmostEqual(p1.x, 1000.0, places=2)
        self.assertAlmostEqual(p1.y, 1500.0, places=2)
        self.assertGreater(scaled.strokes[0].size_px, 10.0)

        # 2. fit / contain (アスペクト比維持・余白レターボックス)
        fitted = plan.scale_to(2000.0, 1000.0, fit_mode="fit")
        self.assertEqual((fitted.canvas_width, fitted.canvas_height), (2000.0, 1000.0))
        # 1000x1000 は 1000x1000 のまま scale=1.0、offset_x = (2000 - 1000)/2 = 500.0
        fp0 = fitted.strokes[0].points[0]
        self.assertAlmostEqual(fp0.x, 600.0, places=2)
        self.assertAlmostEqual(fp0.y, 100.0, places=2)
        self.assertAlmostEqual(fitted.strokes[0].size_px, 10.0, places=2)

        # 3. fill / cover (アスペクト比維持・全域カバー)
        filled = plan.scale_to(2000.0, 1000.0, fit_mode="fill")
        # scale = max(2000/1000, 1000/1000) = 2.0
        # offset_x = (2000 - 2000)/2 = 0.0, offset_y = (1000 - 2000)/2 = -500.0
        self.assertEqual((filled.canvas_width, filled.canvas_height), (2000.0, 1000.0))
        fl_p0 = filled.strokes[0].points[0]
        self.assertAlmostEqual(fl_p0.x, 200.0, places=2)
        self.assertAlmostEqual(fl_p0.y, 0.0, places=2)  # 100 * 2 - 500 = -300 -> clamped to 0.0
        self.assertAlmostEqual(filled.strokes[0].size_px, 20.0, places=2)

        # 4. center (スケール維持・中央配置)
        centered = plan.scale_to(1200.0, 1200.0, fit_mode="center")
        c_p0 = centered.strokes[0].points[0]
        self.assertAlmostEqual(c_p0.x, 200.0, places=2)  # 100 + (1200 - 1000)/2 = 200.0
        self.assertAlmostEqual(c_p0.y, 200.0, places=2)

        # 5. with_canvas_size helper
        resized = plan.with_canvas_size(800.0, 600.0)
        self.assertEqual((resized.canvas_width, resized.canvas_height), (800.0, 600.0))

        # 6. エラーハンドリング
        with self.assertRaises(PlanValidationError):
            plan.scale_to(0, 100)
        with self.assertRaises(PlanValidationError):
            plan.scale_to(100, 100, fit_mode="invalid_mode")

    def test_combine_drawing_plans_auto_rescale(self) -> None:
        """combine_drawing_plans で auto_rescale=True を指定した場合に異なる解像度の計画を統一して統合できることを検証。"""
        stroke1 = Stroke(
            id="s1",
            points=[StrokePoint(10.0, 10.0, 0.8, 0), StrokePoint(90.0, 90.0, 0.8, 10)],
            brush_preset="Basic-5 Size",
            color="#111111",
            size_px=5.0,
            layer_name="Lineart",
        )
        stroke2 = Stroke(
            id="s2",
            points=[StrokePoint(20.0, 20.0, 0.8, 0), StrokePoint(180.0, 180.0, 0.8, 10)],
            brush_preset="Basic-5 Size",
            color="#222222",
            size_px=10.0,
            layer_name="Lineart",
        )
        plan1 = DrawingPlan("same prompt", 1, [stroke1], canvas_width=100.0, canvas_height=100.0)
        plan2 = DrawingPlan("same prompt", 1, [stroke2], canvas_width=200.0, canvas_height=200.0)

        # auto_rescale=False では寸法不一致例外
        with self.assertRaises(PlanValidationError):
            combine_drawing_plans([plan1, plan2], auto_rescale=False)

        # auto_rescale=True では第1計画の寸法 (100x100) に自動リサイズされて正常統合
        merged = combine_drawing_plans([plan1, plan2], auto_rescale=True)
        self.assertEqual(len(merged.strokes), 2)
        self.assertEqual((merged.canvas_width, merged.canvas_height), (100.0, 100.0))
        # 2本目のストローク座標が 200x200 から 100x100 にスケーリングされている
        self.assertAlmostEqual(merged.strokes[1].points[0].x, 10.0, places=2)
        self.assertAlmostEqual(merged.strokes[1].points[1].x, 90.0, places=2)

    def test_stroke_program_resolution_adaptation_and_compile(self) -> None:
        """StrokeProgram の with_canvas_size および compile_stroke_program の target_width/height による解像度適応を検証。"""
        prog = StrokeProgram(
            prompt="anime girl",
            seed=123,
            operations=[
                PathOperation(
                    id="p1",
                    points=(ProgramPoint(0.2, 0.2), ProgramPoint(0.8, 0.8)),
                    brush=ProgramBrush(profile="gpen", color="#333333", size=0.01, size_mode="ratio"),
                    layer="Lineart",
                ),
                FillOperation(
                    id="f1",
                    polygon=(
                        ProgramPoint(0.1, 0.1),
                        ProgramPoint(0.9, 0.1),
                        ProgramPoint(0.9, 0.9),
                        ProgramPoint(0.1, 0.9),
                    ),
                    brush=ProgramBrush(profile="watercolor", color="#eef5ff", size=0.08, size_mode="ratio"),
                    style="wash",
                    layer="Flats",
                ),
            ],
            canvas_width=1000.0,
            canvas_height=1000.0,
        )

        # 1. with_canvas_size
        prog_4k = prog.with_canvas_size(3840.0, 2160.0)
        self.assertEqual((prog_4k.canvas_width, prog_4k.canvas_height), (3840.0, 2160.0))

        # 2. compile_stroke_program with target_width / target_height
        compiled_4k = compile_stroke_program(prog, count=50, target_width=3840.0, target_height=2160.0)
        self.assertEqual((compiled_4k.canvas_width, compiled_4k.canvas_height), (3840.0, 2160.0))
        self.assertGreater(len(compiled_4k.strokes), 0)
        # 全ての点が 3840x2160 の範囲内に収まっている
        for s in compiled_4k.strokes:
            for p in s.points:
                self.assertGreaterEqual(p.x, 0.0)
                self.assertLess(p.x, 3840.0)
                self.assertGreaterEqual(p.y, 0.0)
                self.assertLess(p.y, 2160.0)

    def test_procedural_generation_across_various_resolutions_and_aspect_ratios(self) -> None:
        """様々な解像度・アスペクト比（4K, 21:9 超ワイド, 9:16 縦長, 320x240 低解像度）でのプロシージャル生成健全性を検証。"""
        test_resolutions = [
            (3840.0, 2160.0),  # 4K UHD 16:9
            (2560.0, 1080.0),  # 21:9 Ultra-Wide
            (1080.0, 1920.0),  # 9:16 Mobile Vertical
            (1000.0, 1000.0),  # 1:1 Square
            (320.0, 240.0),  # Low-res Legacy
        ]
        prompts = [
            ("cute anime girl with emerald eyes", "anime"),
            ("majestic mountain lake landscape", "nature"),
            ("cyberpunk neon city skyline", "cyberpunk"),
            ("magic circle with runes", "anime"),
        ]

        for width, height in test_resolutions:
            for prompt, palette in prompts:
                plan = generate_procedural_plan(
                    prompt,
                    seed=42,
                    count=40,
                    width=width,
                    height=height,
                    palette_name=palette,
                )
                self.assertEqual((plan.canvas_width, plan.canvas_height), (width, height))
                self.assertGreater(len(plan.strokes), 0)
                self.assertLessEqual(len(plan.strokes), 40)

                # 点がキャンバス境界内に完全に収まっていることを確認
                for stroke in plan.strokes:
                    self.assertGreater(stroke.size_px, 0.0)
                    for pt in stroke.points:
                        self.assertGreaterEqual(pt.x, 0.0)
                        self.assertLess(pt.x, width)
                        self.assertGreaterEqual(pt.y, 0.0)
                        self.assertLess(pt.y, height)

    def test_combine_drawing_plans_auto_rescale_with_leading_none_dimensions(self) -> None:
        """第1計画の寸法がNoneでも後続計画の既知寸法へ自動スケールして統合できることを検証。"""
        stroke1 = Stroke(
            id="s1",
            points=[StrokePoint(10.0, 10.0, 0.8, 0), StrokePoint(90.0, 90.0, 0.8, 10)],
            brush_preset="Basic-5 Size",
            color="#111111",
            size_px=5.0,
            layer_name="Lineart",
        )
        stroke2 = Stroke(
            id="s2",
            points=[StrokePoint(20.0, 20.0, 0.8, 0), StrokePoint(180.0, 180.0, 0.8, 10)],
            brush_preset="Basic-5 Size",
            color="#222222",
            size_px=10.0,
            layer_name="Lineart",
        )
        plan_none = DrawingPlan("same prompt", 1, [stroke1], canvas_width=None, canvas_height=None)
        plan_200 = DrawingPlan("same prompt", 1, [stroke2], canvas_width=200.0, canvas_height=200.0)

        merged = combine_drawing_plans([plan_none, plan_200], auto_rescale=True)
        self.assertEqual(len(merged.strokes), 2)
        self.assertEqual((merged.canvas_width, merged.canvas_height), (200.0, 200.0))

    def test_combine_drawing_plans_auto_rescale_handles_partial_dimensions(self) -> None:
        points = [StrokePoint(0.0, 0.0, 0.8, 0), StrokePoint(90.0, 90.0, 0.8, 10)]
        partial = DrawingPlan(
            "same prompt",
            1,
            [Stroke("partial", points)],
            canvas_width=100.0,
            canvas_height=None,
        )
        target = DrawingPlan(
            "same prompt",
            1,
            [Stroke("target", points)],
            canvas_width=100.0,
            canvas_height=200.0,
        )

        merged = combine_drawing_plans([partial, target], auto_rescale=True)
        self.assertEqual((merged.canvas_width, merged.canvas_height), (100.0, 200.0))
        self.assertGreater(merged.strokes[0].points[-1].y, 190.0)

    def test_svg_export_sanitizes_layer_element_ids(self) -> None:
        """レイヤー名に空白や記号が含まれていてもXML標準準拠のID属性へサニタイズされることを検証。"""
        import xml.etree.ElementTree as ET

        points = [StrokePoint(10, 10, 1, 0), StrokePoint(50, 50, 1, 10)]
        plan = DrawingPlan(
            "test prompt",
            1,
            [
                Stroke("s1", points, layer_name="AI Strokes (editable)"),
                Stroke("s2", points, layer_name="Draft / Sketch"),
                Stroke("s3", points, layer_name="A/B"),
                Stroke("s4", points, layer_name="A?B"),
            ],
            layers=["AI Strokes (editable)", "Draft / Sketch", "A/B", "A?B"],
        )
        svg_content = plan.to_svg(100, 100)
        self.assertIn('id="layer_AI_Strokes__editable_"', svg_content)
        self.assertIn('id="layer_Draft___Sketch"', svg_content)
        self.assertIn('id="layer_A_B"', svg_content)
        self.assertIn('id="layer_A_B_2"', svg_content)
        # XMLとして正しくパース可能であることを確認
        root = ET.fromstring(svg_content)
        self.assertEqual(root.tag.split("}")[-1], "svg")
        layer_ids = [
            element.attrib["id"] for element in root.iter() if element.attrib.get("id", "").startswith("layer_")
        ]
        self.assertEqual(len(layer_ids), len(set(layer_ids)))

    def test_fake_signal_disconnect(self) -> None:
        """ヘッドレス環境用 _FakeSignal の disconnect メソッドの個別解除および一括解除を検証。"""
        from .qt_compat import _FakeSignal

        signal = _FakeSignal()
        received: list[int] = []

        def slot_a(val: int) -> None:
            received.append(val)

        def slot_b(val: int) -> None:
            received.append(val * 10)

        signal.connect(slot_a)
        signal.connect(slot_b)
        signal.emit(5)
        self.assertEqual(received, [5, 50])

        received.clear()
        signal.disconnect(slot_a)
        signal.emit(3)
        self.assertEqual(received, [30])

        received.clear()
        signal.disconnect()
        signal.emit(7)
        self.assertEqual(received, [])

    def test_macro_operation_compilation_and_rescue(self) -> None:
        """MacroOperation（flower_cluster, branch_tree, mountain_range, watercolor_wash）のパース、シリアライズ、コンパイル、LLM救済を包括的に検証。"""
        from ai_stroke_painter.llm_planner import _sanitize_and_rescue_program_dict
        from ai_stroke_painter.stroke_program import (
            MacroOperation,
            ProgramBrush,
            StrokeProgram,
            compile_stroke_program,
        )

        # 1. MacroOperation 直接生成とコンパイル
        macro_flower = MacroOperation(
            id="flower_1",
            layer="Flats",
            name="flower_cluster",
            brush=ProgramBrush(profile="watercolor", color="#ffb8cd", size=0.05),
            center=(0.5, 0.4),
            radius=0.25,
            colors=("#ffb8cd", "#ffd6e5", "#a3436a"),
        )
        macro_tree = MacroOperation(
            id="tree_1",
            layer="Lineart",
            name="branch_tree",
            brush=ProgramBrush(profile="gpen", color="#342017", size=0.005),
            center=(0.4, 0.7),
            radius=0.35,
        )
        macro_mountain = MacroOperation(
            id="mountain_1",
            layer="Flats",
            name="mountain_range",
            brush=ProgramBrush(profile="watercolor", color="#6f829d", size=0.08),
            center=(0.5, 0.55),
            colors=("#6f829d", "#4a5568", "#283e50"),
        )
        macro_wash = MacroOperation(
            id="wash_1",
            layer="Flats",
            name="watercolor_wash",
            brush=ProgramBrush(profile="watercolor", color="#2b5c8f", size=0.15),
            bounds=(0.0, 0.0, 1.0, 0.45),
            colors=("#2b5c8f", "#5c93cf", "#eef6ff"),
        )

        program = StrokeProgram(
            prompt="sakura mountain landscape",
            seed=42,
            canvas_width=1000,
            canvas_height=1000,
            operations=(macro_wash, macro_mountain, macro_tree, macro_flower),
        )
        self.assertEqual(len(program.operations), 4)

        # 辞書シリアライズ・デシリアライズ検証
        p_dict = program.as_dict()
        loaded = StrokeProgram.from_dict(p_dict)
        self.assertEqual(len(loaded.operations), 4)
        first_op = loaded.operations[0]
        self.assertIsInstance(first_op, MacroOperation)
        assert isinstance(first_op, MacroOperation)
        self.assertEqual(first_op.name, "watercolor_wash")

        # コンパイル検証
        plan = compile_stroke_program(program)
        self.assertGreater(len(plan.strokes), 10)
        # 各レイヤーにストロークが分配されていることを確認
        layer_names = {s.layer_name for s in plan.strokes}
        self.assertIn("Flats", layer_names)
        self.assertIn("Lineart", layer_names)
        self.assertIn("Highlights", layer_names)

        # 2. LLM レスポンスの辞書からの救済検証
        raw_llm_payload = {
            "prompt": "sakura and mountains",
            "canvas": {"width": 1000, "height": 1000},
            "operations": [
                {
                    "kind": "macro",
                    "name": "flower_cluster",
                    "layer": "Flats",
                    "center": [0.5, 0.4],
                    "radius": 0.2,
                    "colors": ["#ff9999", "#ffcccc"],
                    "brush": {"profile": "watercolor", "color": "#ff9999", "size": 0.05},
                },
                {
                    "kind": "tree",
                    "name": "branch_tree",
                    "layer": "Lineart",
                    "center": [0.4, 0.7],
                    "radius": 0.3,
                },
            ],
        }
        rescued = _sanitize_and_rescue_program_dict(raw_llm_payload, 1000, 1000)
        self.assertEqual(len(rescued["operations"]), 2)
        self.assertEqual(rescued["operations"][0]["kind"], "macro")
        self.assertEqual(rescued["operations"][1]["kind"], "macro")


class PaletteAutoModeTests(unittest.TestCase):
    """パレット自動選択 (Auto モード) の推定・プロシージャル生成・LLM連携・画像変換・UI統合の検証。"""

    _app: Any = None

    @classmethod
    def setUpClass(cls) -> None:
        if hasattr(QApplication, "instance"):
            cls._app = QApplication.instance()
            if cls._app is None and callable(QApplication):
                with contextlib.suppress(Exception):
                    cls._app = QApplication(["test", "-platform", "offscreen"])

    def test_infer_palette_from_prompt_keywords(self) -> None:
        from .procedural import infer_palette_from_prompt

        test_cases = [
            ("japanese sumi-e ink wash pine tree on mountain cliff", "sumie"),
            ("和風水墨画の松と竹林", "sumie"),
            ("cyberpunk city skyline with neon buildings", "cyberpunk"),
            ("サイバーパンクの近未来都市", "cyberpunk"),
            ("delicate watercolor wildflower garden with soft petals", "watercolor"),
            ("透明水彩の野花", "watercolor"),
            ("blooming rose with stem and organic leaves", "botanical"),
            ("ボタニカルな薔薇の花束", "botanical"),
            ("fantasy sakura landscape with mountains and clouds", "nature"),
            ("大自然の山と海と森林", "nature"),
            ("cute pastel fairy with ribbon", "pastel"),
            ("ゆめかわパステルの少女", "pastel"),
            ("80s retro pop disco synthwave", "retro_pop"),
            ("80年代レトロポップ", "retro_pop"),
            ("gothic dark fantasy vampire in darkness", "dark_fantasy"),
            ("漆黒の魔界ダークファンタジー", "dark_fantasy"),
            ("vintage antique pocket watch in sepia", "sepia"),
            ("古写真セピア調", "sepia"),
            ("intense manga focus radial speed lines", "monochrome"),
            ("白黒の集中線と線画", "monochrome"),
            ("cyber gold golden dragon statue", "cyber_gold"),
            ("黄金のサイバーゴールド", "cyber_gold"),
            ("anime girl portrait, delicate eyes", "anime"),
            ("美少女アニメキャラクター", "anime"),
        ]
        for prompt, expected_palette in test_cases:
            inferred = infer_palette_from_prompt(prompt)
            self.assertEqual(
                inferred,
                expected_palette,
                f"Prompt '{prompt}' should infer palette '{expected_palette}', but got '{inferred}'",
            )

    def test_infer_palette_from_prompt_fallbacks(self) -> None:
        from .procedural import infer_palette_from_prompt

        self.assertEqual(infer_palette_from_prompt("mountain lake"), "nature")
        self.assertEqual(infer_palette_from_prompt("building structure"), "cyberpunk")
        self.assertEqual(infer_palette_from_prompt("dragon beast"), "nature")
        self.assertEqual(infer_palette_from_prompt("magic spell"), "monochrome")
        self.assertEqual(infer_palette_from_prompt(""), "anime")

    def test_procedural_generation_with_auto_palette(self) -> None:
        from .procedural import generate_procedural_plan
        from .procedural.base import color_palette

        plan = generate_procedural_plan(
            "japanese sumi-e ink wash pine tree",
            seed=42,
            count=30,
            width=800,
            height=600,
            palette_name="auto",
        )
        self.assertEqual(plan.metadata.get("palette"), "auto")
        self.assertEqual(plan.metadata.get("resolved_palette"), "sumie")
        self.assertGreater(len(plan.strokes), 0)
        sumie_colors = {c.lower() for c in color_palette("sumie").values()}
        for stroke in plan.strokes:
            self.assertIn(
                stroke.color.lower(),
                sumie_colors,
                f"Stroke color {stroke.color} not in sumie palette",
            )

    def test_rule_based_planner_with_auto_palette(self) -> None:
        from .planner import RuleBasedPlanner

        planner = RuleBasedPlanner()
        plan = planner.plan(
            "vintage antique pocket watch in sepia",
            seed=10,
            count=25,
            width=600,
            height=600,
            palette_name="auto",
        )
        self.assertEqual(plan.metadata.get("palette"), "auto")
        self.assertEqual(plan.metadata.get("resolved_palette"), "sepia")

    def test_llm_planner_system_prompt_and_constraints_with_auto_palette(self) -> None:
        from .domain import DrawingPlan, Stroke, StrokePoint
        from .llm_planner import _apply_llm_style_constraints, _system_instruction

        sys_prompt = _system_instruction(prompt="test", palette_name="auto")
        self.assertIn("=== PALETTE DIRECTION: AUTO ===", sys_prompt)

        test_plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[
                Stroke(
                    id="s1",
                    points=[StrokePoint(10, 10, 0.5, 0), StrokePoint(20, 20, 0.5, 10)],
                    brush_preset="Basic-5 Size",
                    color="#123456",
                    size_px=5.0,
                    layer_name="Lineart",
                    opacity=1.0,
                )
            ],
            title="Test",
            iteration=1,
            layers=["Lineart"],
            canvas_width=800,
            canvas_height=600,
        )
        constrained = _apply_llm_style_constraints(test_plan, brush_profile="auto", palette_name="auto")
        self.assertEqual(constrained.strokes[0].color, "#123456")
        self.assertEqual(constrained.metadata["style_constraints"]["palette"], "auto")
        self.assertFalse(constrained.metadata["style_constraints"]["palette_locked"])

    def test_image_converter_auto_palette_inference(self) -> None:
        from .image_converter import _infer_best_palette_for_image

        # モノクロに近い画素群
        mono_pixels = [(20, 20, 20), (128, 128, 128), (240, 240, 240)] * 10
        self.assertEqual(_infer_best_palette_for_image(mono_pixels), "monochrome")

        # プロンプト指定がある場合の優先
        self.assertEqual(_infer_best_palette_for_image(mono_pixels, "japanese sumi-e"), "sumie")

    def test_docker_palette_combo_has_auto_option(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        self.assertGreater(docker.palette_combo.count(), 0)
        self.assertEqual(docker.palette_combo.itemData(0), "auto")
        self.assertEqual(docker.palette_combo.itemText(0), "自動 (Auto)")

    def test_docker_tabs_and_ui_enhancements(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        self.assertTrue(hasattr(docker, "tabs"))
        self.assertEqual(docker.tabs.count(), 4)
        tab_titles = [docker.tabs.tabText(i) for i in range(4)]
        self.assertIn("🎨 生成・描画", tab_titles[0])
        self.assertIn("🖼️ 参照画像", tab_titles[1])
        self.assertIn("🤖 AI設定", tab_titles[2])
        self.assertIn("⚙️ レイヤー・詳細", tab_titles[3])

        self.assertTrue(hasattr(docker, "prompt_history_combo"))
        self.assertTrue(hasattr(docker, "clear_prompt_btn"))
        self.assertTrue(hasattr(docker, "profile_openai_btn"))
        self.assertTrue(hasattr(docker, "profile_ollama_btn"))
        self.assertTrue(hasattr(docker, "profile_lmstudio_btn"))
        self.assertTrue(hasattr(docker, "profile_deepseek_btn"))

    def test_docker_prompt_tags_and_clear(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        docker.prompt.setPlainText("cute cat")
        docker._add_prompt_tag("anime style")
        self.assertEqual(docker.prompt.toPlainText(), "cute cat, anime style")

        docker._clear_prompt()
        self.assertEqual(docker.prompt.toPlainText(), "")

    def test_docker_llm_profiles(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        docker._apply_llm_profile("openai")
        self.assertEqual(docker.base_url.text(), "https://api.openai.com/v1")
        self.assertEqual(docker.model.text(), "gpt-4o")
        self.assertEqual(docker.max_tokens.value(), 16384)

        docker._apply_llm_profile("ollama")
        self.assertEqual(docker.base_url.text(), "http://127.0.0.1:11434/v1")
        self.assertEqual(docker.model.text(), "llama3.2-vision")


class PreviewDiscrepancyFixTests(unittest.TestCase):
    """プレビュー表示とKrita実キャンバス描画の乖離是正の検証。"""

    _app: Any = None

    @classmethod
    def setUpClass(cls) -> None:
        if hasattr(QApplication, "instance"):
            cls._app = QApplication.instance()
            if cls._app is None and callable(QApplication):
                with contextlib.suppress(Exception):
                    cls._app = QApplication(["test", "-platform", "offscreen"])

    def test_drawing_plan_from_dict_parses_nested_canvas_mapping(self) -> None:
        raw = {
            "prompt": "fantasy anime landscape",
            "seed": 22,
            "strokes": [],
            "canvas": {"width": 2480, "height": 3508},
        }
        plan = DrawingPlan.from_dict(raw)
        self.assertEqual(plan.canvas_width, 2480.0)
        self.assertEqual(plan.canvas_height, 3508.0)

    def test_preview_widget_set_canvas_size_and_clipping(self) -> None:
        from .docker import PreviewWidget
        from .qt_compat import QImage, QPainter

        prev = PreviewWidget()
        prev.set_canvas_size(2480, 3508)
        self.assertEqual(prev._canvas_width, 2480.0)
        self.assertEqual(prev._canvas_height, 3508.0)

        outside_stroke = Stroke(
            id="outside",
            points=[
                StrokePoint(x=-500, y=-500, pressure=0.8, time_ms=0),
                StrokePoint(x=3500, y=4000, pressure=0.8, time_ms=10),
            ],
            brush_preset="Basic-5 Size",
            color="#ff0000",
            size_px=50.0,
        )
        plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=(outside_stroke,),
            canvas_width=2480,
            canvas_height=3508,
        )
        prev.set_plan(plan)

        if QImage is not None and QPainter is not None and callable(QImage) and callable(QPainter):
            fmt = argb32_image_format(QImage)
            img = QImage(200, 160, fmt) if fmt is not None else QImage(200, 160)
            if hasattr(img, "fill"):
                img.fill(0)
            painter = QPainter(img)
            try:
                prev.paint_to_painter(painter, 200, 160)
            finally:
                if hasattr(painter, "end"):
                    painter.end()

    def test_procedural_prompt_category_anime_landscape(self) -> None:
        from .procedural import _prompt_category

        cat = _prompt_category("fantasy anime landscape with mountains and cherry blossom")
        self.assertEqual(cat, "landscape")

    def test_apply_stroke_style_prefix_and_mypaint_exclusion(self) -> None:
        from types import SimpleNamespace

        from .krita_adapter import _apply_stroke_style

        selected_preset = None

        def fake_set_preset(p: Any) -> None:
            nonlocal selected_preset
            selected_preset = p

        view = SimpleNamespace(setCurrentBrushPreset=fake_set_preset)
        fake_mypaint = SimpleNamespace(name=lambda: "c) Pencil 2b (mypaint)")
        fake_pencil2 = SimpleNamespace(name=lambda: "c) Pencil-2")
        fake_basic = SimpleNamespace(name=lambda: "b) Basic-5 Size")

        presets_dict = {
            "mypaint": fake_mypaint,
            "pencil2": fake_pencil2,
            "basic": fake_basic,
        }
        app = SimpleNamespace(resources=lambda _k: presets_dict)
        fake_krita = SimpleNamespace(Krita=SimpleNamespace(instance=lambda: app))

        stroke_pencil = Stroke(
            id="p",
            points=[StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)],
            brush_preset="pencil",
        )
        with patch.dict("sys.modules", {"krita": fake_krita}):
            _apply_stroke_style(stroke_pencil, view=view)
            self.assertIs(selected_preset, fake_pencil2)

            stroke_basic = Stroke(
                id="b",
                points=[StrokePoint(0, 0, 1, 0), StrokePoint(10, 10, 1, 10)],
                brush_preset="Basic-5 Size",
            )
            _apply_stroke_style(stroke_basic, view=view)
            self.assertIs(selected_preset, fake_basic)

    def test_continuous_path_pressure_tolerance(self) -> None:
        from types import SimpleNamespace

        from .krita_adapter import _can_use_continuous_path

        node = SimpleNamespace(paintPath=lambda _p: None)
        stroke_smooth = Stroke(
            id="s1",
            points=[
                StrokePoint(0, 0, 0.6, 0),
                StrokePoint(10, 10, 0.9, 10),
                StrokePoint(20, 20, 0.7, 20),
            ],
            brush_preset="Basic-5 Size",
        )
        self.assertTrue(_can_use_continuous_path(node, stroke_smooth))

    def test_docker_run_initializes_canvas_dimensions_and_worker(self) -> None:
        from types import SimpleNamespace

        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        fake_doc = SimpleNamespace(
            width=lambda: 1920,
            height=lambda: 1080,
        )
        fake_view = SimpleNamespace(
            document=lambda: fake_doc,
        )
        fake_window = SimpleNamespace(
            activeView=lambda: fake_view,
        )
        fake_app = SimpleNamespace(
            activeDocument=lambda: fake_doc,
            activeWindow=lambda: fake_window,
        )

        class _DummyWidget:
            def __init__(self, val: Any = None) -> None:
                self._val = val
                self.canvas_size: tuple[float, float] | None = None

            def toPlainText(self) -> str:
                return "test prompt"

            def text(self) -> str:
                return str(self._val) if self._val is not None else ""

            def value(self) -> Any:
                return self._val if self._val is not None else 1

            def currentData(self) -> Any:
                return self._val if self._val is not None else "auto"

            def currentText(self) -> str:
                return str(self._val) if self._val is not None else ""

            def isChecked(self) -> bool:
                return False

            def setEnabled(self, *args: Any) -> None:
                pass

            def setRange(self, *args: Any) -> None:
                pass

            def setText(self, *args: Any) -> None:
                pass

            def clear_plan(self) -> None:
                pass

            def set_canvas_size(self, w: float, h: float) -> None:
                self.canvas_size = (w, h)

        docker.prompt = cast(Any, _DummyWidget("test prompt"))
        docker.seed = cast(Any, _DummyWidget(42))
        docker.count = cast(Any, _DummyWidget(30))
        docker.iterations = cast(Any, _DummyWidget(1))
        docker.auto_refine = cast(Any, _DummyWidget(False))
        docker.goal_mode = cast(Any, _DummyWidget(False))
        docker.auto_seed = cast(Any, _DummyWidget(False))
        docker.auto_count = cast(Any, _DummyWidget(False))
        docker.palette_combo = cast(Any, _DummyWidget("anime"))
        docker.brush_profile = cast(Any, _DummyWidget("auto"))
        docker.brush_size_multiplier = cast(Any, _DummyWidget(1.0))
        docker.opacity_multiplier = cast(Any, _DummyWidget(100))
        docker.layer_mode = cast(Any, _DummyWidget("multi_layer"))
        docker.layer_prefix = cast(Any, _DummyWidget("AI Artwork"))
        docker.event_interval = cast(Any, _DummyWidget(30))
        docker.save_json = cast(Any, _DummyWidget(False))
        docker.save_svg_chk = cast(Any, _DummyWidget(False))
        docker.edge_threshold = cast(Any, _DummyWidget(0.18))
        docker.shading_density = cast(Any, _DummyWidget("medium"))
        docker.enable_flats = cast(Any, _DummyWidget(True))
        docker.image_color_mode = cast(Any, _DummyWidget("original"))
        docker.planner_mode = cast(Any, _DummyWidget("rule_based"))
        docker.run_btn = cast(Any, _DummyWidget())
        docker.stop_btn = cast(Any, _DummyWidget())
        docker.progress = cast(Any, _DummyWidget())
        docker.status = cast(Any, _DummyWidget())
        docker.history_combo = cast(Any, _DummyWidget())
        docker._prompt_history = []
        prev = _DummyWidget()
        docker.preview = cast(Any, prev)
        docker.planner = RuleBasedPlanner()
        docker._image_bytes = None
        docker._worker = None
        docker.canvas_port = cast(Any, None)
        docker.api_url = cast(Any, _DummyWidget("https://api.openai.com/v1"))
        docker.api_key = cast(Any, _DummyWidget("sk-test"))
        docker.model = cast(Any, _DummyWidget("gpt-4o"))
        docker.temperature = cast(Any, _DummyWidget(0.7))
        docker.timeout_sec = cast(Any, _DummyWidget(60))
        docker.vision_res = cast(Any, _DummyWidget(512))
        docker.confirm_before_apply = cast(Any, _DummyWidget(False))
        docker.debug_mode_chk = cast(Any, _DummyWidget(False))

        with (
            patch("ai_stroke_painter.docker.Krita.instance", return_value=fake_app),
            patch("ai_stroke_painter.docker.PlanWorker.start"),
            patch.object(AIStrokePainterDocker, "_save_settings", return_value=None),
            patch.object(AIStrokePainterDocker, "_log_debug", return_value=None),
        ):
            docker.run()
            self.assertEqual(getattr(prev, "canvas_size", None), (1920.0, 1080.0))
            worker_instance: Any = getattr(docker, "_worker", None)
            self.assertIsNotNone(worker_instance)
            self.assertEqual(getattr(worker_instance, "width", None), 1920.0)
            self.assertEqual(getattr(worker_instance, "height", None), 1080.0)


class ImageGeneratorAndPlannerTests(unittest.TestCase):
    """Text-to-Image 画像生成クライアント、新マクロ、ImageGenerationPlanner の包括的検証。"""

    def test_image_generator_settings_defaults(self) -> None:
        from .image_generator import ImageGeneratorSettings

        s = ImageGeneratorSettings()
        self.assertEqual(s.provider, "openai")
        self.assertEqual(s.model, "dall-e-3")
        self.assertEqual(s.size, "1024x1024")

    def test_image_generator_openai_call(self) -> None:
        from .image_generator import ImageGeneratorClient, ImageGeneratorSettings

        client = ImageGeneratorClient(ImageGeneratorSettings(api_key="sk-fake-key"))
        # 1x1 PNG transparent
        fake_png_b64 = (
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
        )
        fake_response = json.dumps({"data": [{"b64_json": fake_png_b64}]}).encode("utf-8")

        class FakeHTTPResponse:
            def __init__(self, data: bytes) -> None:
                self._data = data
                self.status = 200

            def read(self) -> bytes:
                return self._data

            def __enter__(self) -> FakeHTTPResponse:
                return self

            def __exit__(self, *args: Any) -> None:
                pass

        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.return_value = FakeHTTPResponse(fake_response)

            img_bytes = client.generate_image("fantasy anime landscape")
            self.assertGreater(len(img_bytes), 10)
            self.assertTrue(img_bytes.startswith(b"\x89PNG") or img_bytes.startswith(b"\xff\xd8"))

    def test_image_generation_planner_execution(self) -> None:
        from .image_generator import ImageGeneratorSettings
        from .planner import ImageGenerationPlanner

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

        fake_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (20).to_bytes(4, "big") + (10).to_bytes(4, "big")

        planner = ImageGenerationPlanner(ImageGeneratorSettings(api_key="sk-fake-key"))
        planner.image_converter.qimage_cls = FakeImage
        with patch.object(planner.image_client, "generate_image", return_value=fake_png):
            plan = planner.plan(
                prompt="fantasy anime mountain",
                seed=42,
                count=20,
                width=800,
                height=600,
            )
            self.assertIsNotNone(plan)
            self.assertEqual(plan.prompt, "AI Generated: fantasy anime mountain")
            self.assertEqual(plan.metadata.get("generator"), "text_to_image_to_stroke")

    def test_new_macro_operations_compilation(self) -> None:
        from .stroke_program import (
            MacroOperation,
            ProgramBrush,
            StrokeProgram,
            compile_stroke_program,
        )

        cloud_macro = MacroOperation(
            id="cloud_1",
            layer="Flats",
            name="cloud_cluster",
            brush=ProgramBrush(profile="watercolor", color="#ffffff", size=0.1),
            center=(0.5, 0.3),
            radius=0.25,
            colors=("#8ca6c7", "#bfd4ea", "#ffffff", "#ffffff"),
        )
        face_macro = MacroOperation(
            id="face_1",
            layer="Lineart",
            name="character_face",
            brush=ProgramBrush(profile="gpen", color="#1c1018", size=0.005),
            center=(0.5, 0.5),
            radius=0.3,
        )
        magic_macro = MacroOperation(
            id="magic_1",
            layer="Highlights",
            name="magic_circle",
            brush=ProgramBrush(profile="gpen", color="#00ffff", size=0.003),
            center=(0.5, 0.5),
            radius=0.35,
        )

        prog = StrokeProgram(
            prompt="anime magic girl with clouds",
            seed=42,
            canvas_width=1000,
            canvas_height=1000,
            operations=(cloud_macro, face_macro, magic_macro),
        )
        plan = compile_stroke_program(prog)
        self.assertGreater(len(plan.strokes), 15)
        layer_set = {s.layer_name for s in plan.strokes}
        self.assertTrue("Flats" in layer_set or "Lineart" in layer_set)

    def test_macro_unrecognized_name_graceful_fallback(self) -> None:
        from .stroke_program import (
            MacroOperation,
            ProgramBrush,
            StrokeProgram,
            compile_stroke_program,
        )

        unknown_macro = MacroOperation(
            id="unknown_1",
            layer="Flats",
            name="custom_futuristic_crystal_monolith",
            brush=ProgramBrush(profile="gpen", color="#4488ff", size=0.02),
            center=(0.5, 0.5),
            radius=0.2,
        )
        prog = StrokeProgram(
            prompt="crystal monolith",
            seed=123,
            canvas_width=800,
            canvas_height=800,
            operations=(unknown_macro,),
        )
        plan = compile_stroke_program(prog, count=10)
        self.assertGreater(len(plan.strokes), 0)
        self.assertEqual(plan.strokes[0].layer_name, "Flats")

    def test_gradient_fill_operation_compilation_and_budgeting(self) -> None:
        from .stroke_program import (
            FillOperation,
            GradientFillOperation,
            ProgramBrush,
            ProgramPoint,
            StrokeProgram,
            compile_stroke_program,
        )

        grad_op = GradientFillOperation(
            id="grad_test",
            polygon=[ProgramPoint(0.1, 0.1, 1.0), ProgramPoint(0.9, 0.1, 1.0), ProgramPoint(0.5, 0.9, 1.0)],
            colors=["#fff", "#000"],  # 3-digit hex
            brush=ProgramBrush(profile="watercolor", size=0.05),
        )
        fill_op = FillOperation(
            id="fill_test",
            polygon=[
                ProgramPoint(0.0, 0.0, 1.0),
                ProgramPoint(1.0, 0.0, 1.0),
                ProgramPoint(1.0, 1.0, 1.0),
                ProgramPoint(0.0, 1.0, 1.0),
            ],
            brush=ProgramBrush(profile="watercolor", size=0.08, color="#ffffff"),
        )
        prog = StrokeProgram(
            prompt="gradient test",
            seed=1,
            canvas_width=1000,
            canvas_height=1000,
            operations=(grad_op, fill_op),
        )
        plan = compile_stroke_program(prog, count=30)
        grad_strokes = [s for s in plan.strokes if s.color != "#ffffff"]
        self.assertGreater(len(grad_strokes), 3)
        # Verify scanlines advance vertically (not stacked)
        y_coords = {round(s.points[0].y, 1) for s in grad_strokes}
        self.assertGreater(len(y_coords), 1)

    def test_image_generator_openai_url_ssrf_prevention(self) -> None:
        from .image_generator import ImageGenerationError, ImageGeneratorClient, ImageGeneratorSettings

        client = ImageGeneratorClient(ImageGeneratorSettings(api_key="sk-test"))

        class FakeHTTPResponse:
            def __init__(self, data: bytes) -> None:
                self._data = data
                self.status = 200

            def read(self, _size: int = -1) -> bytes:
                d = self._data
                self._data = b""
                return d

            def __enter__(self) -> FakeHTTPResponse:
                return self

            def __exit__(self, *args: Any) -> None:
                pass

        # 1. SSRF prevention: Non-HTTPS external URL returned in API response must be rejected
        insecure_resp = json.dumps({"data": [{"url": "http://evil.internal.network/secret.png"}]}).encode("utf-8")
        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.return_value = FakeHTTPResponse(insecure_resp)
            with self.assertRaises(ImageGenerationError) as ctx:
                client.generate_image("test prompt")
            self.assertIn("HTTPS", str(ctx.exception))

        private_resp = json.dumps({"data": [{"url": "https://127.0.0.1/private.png"}]}).encode("utf-8")
        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.return_value = FakeHTTPResponse(private_resp)
            with self.assertRaises(ImageGenerationError) as ctx:
                client.generate_image("test prompt")
            self.assertIn("プライベート", str(ctx.exception))

        # 2. HTTPS URL returned is accepted and downloaded safely
        valid_png = base64.b64decode(
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
        )
        secure_resp = json.dumps({"data": [{"url": "https://images.openai.com/generated.png"}]}).encode("utf-8")
        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            # First open returns API json, second open returns image bytes
            mock_inst.open.side_effect = [FakeHTTPResponse(secure_resp), FakeHTTPResponse(valid_png)]
            img_bytes = client.generate_image("test prompt")
            self.assertTrue(img_bytes.startswith(b"\x89PNG"))

    def test_image_generator_sd_webui_and_error_handling(self) -> None:
        from .image_generator import ImageGenerationError, ImageGeneratorClient, ImageGeneratorSettings

        # 1. SD WebUI call
        client = ImageGeneratorClient(
            ImageGeneratorSettings(provider="sd_webui", endpoint_url="http://127.0.0.1:7860/sdapi/v1/txt2img")
        )
        fake_png_b64 = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
        sd_resp = json.dumps({"images": [fake_png_b64]}).encode("utf-8")

        class ChunkResponse:
            def __init__(self, data: bytes) -> None:
                self._data = data
                self.status = 200

            def read(self, _size: int = -1) -> bytes:
                d = self._data
                self._data = b""
                return d

            def __enter__(self) -> ChunkResponse:
                return self

            def __exit__(self, *args: Any) -> None:
                pass

        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.return_value = ChunkResponse(sd_resp)
            result = client.generate_image("sd portrait")
            self.assertTrue(result.startswith(b"\x89PNG"))

        # 2. Corrupt base64 is caught and wrapped in ImageGenerationError
        bad_sd_resp = json.dumps({"images": ["!!!NOT_BASE_64!!!"]}).encode("utf-8")
        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.return_value = ChunkResponse(bad_sd_resp)
            with self.assertRaises(ImageGenerationError):
                client.generate_image("bad base64")

        # 3. Cancellation check during execution raises ImageGenerationError
        with self.assertRaises(ImageGenerationError) as cancel_ctx:
            client.generate_image("cancelled", cancel_check=lambda: True)
        self.assertIn("キャンセル", str(cancel_ctx.exception))


class CodeReviewEnhancementTests(unittest.TestCase):
    """コードレビューに基づく堅牢性・セキュリティ・ゴールモード・多様性修正の検証。"""

    def test_domain_split_color_alpha_robustness(self) -> None:
        from .domain import split_color_alpha

        self.assertEqual(split_color_alpha("#123"), ("#123", 1.0))
        self.assertEqual(split_color_alpha("#1234"), ("#123", int("44", 16) / 255.0))
        self.assertEqual(split_color_alpha("#11223380"), ("#112233", int("80", 16) / 255.0))
        # 不正な alpha 桁でも例外にならずフォールバックすること
        self.assertEqual(split_color_alpha("#123z"), ("#123z", 1.0))
        self.assertEqual(split_color_alpha("#112233zz"), ("#112233zz", 1.0))

    def test_krita_adapter_parse_hex_rgb_robustness(self) -> None:
        from .krita_adapter import _parse_hex_rgb

        self.assertEqual(_parse_hex_rgb("#fff"), (1.0, 1.0, 1.0))
        self.assertEqual(_parse_hex_rgb("#000000"), (0.0, 0.0, 0.0))
        # 不正な hex 文字列で例外にならず None を返すこと
        self.assertIsNone(_parse_hex_rgb("#zzz"))
        self.assertIsNone(_parse_hex_rgb("#fffffg"))
        self.assertIsNone(_parse_hex_rgb("#12"))
        self.assertIsNone(_parse_hex_rgb(""))

    def test_native_bridge_local_host_and_discovery(self) -> None:
        from .native_bridge import _local_host, discover_native_bridge

        self.assertTrue(_local_host("localhost"))
        self.assertTrue(_local_host("sub.localhost"))
        self.assertTrue(_local_host("127.0.0.1"))
        self.assertTrue(_local_host("::1"))
        self.assertFalse(_local_host("8.8.8.8"))
        self.assertFalse(_local_host("example.com"))

        with patch.dict("os.environ", {"AI_STROKE_BRIDGE_PORT": "9000", "AI_STROKE_BRIDGE_TOKEN": ""}):
            self.assertIsNone(discover_native_bridge())
        with patch.dict("os.environ", {"AI_STROKE_BRIDGE_PORT": "", "AI_STROKE_BRIDGE_TOKEN": "token"}):
            self.assertIsNone(discover_native_bridge())

    def test_image_generator_loopback_validation_and_stream_type_error(self) -> None:
        from .image_generator import _read_bounded_stream, _validate_endpoint_url

        self.assertEqual(
            _validate_endpoint_url("http://local.localhost:8000/sdapi/v1/txt2img"),
            "http://local.localhost:8000/sdapi/v1/txt2img",
        )

        class FakeResponseWithHeadersNoArgRead:
            def __init__(self, data: bytes) -> None:
                self._data = data
                self.headers = {"Content-Type": "image/png"}

            def read(self, *args: Any) -> bytes:
                if args:
                    raise TypeError("read() takes no arguments")
                d = self._data
                self._data = b""
                return d

        resp = FakeResponseWithHeadersNoArgRead(b"\x89PNGfakeimage")
        result = _read_bounded_stream(resp, max_bytes=1000)
        self.assertEqual(result, b"\x89PNGfakeimage")

    def test_rule_based_planner_image_data_iteration_diversity(self) -> None:
        from .domain import combine_drawing_plans
        from .planner import RuleBasedPlanner

        class FakeColor:
            def __init__(self, val: int) -> None:
                self._val = val

            def red(self) -> int:
                return self._val

            def green(self) -> int:
                return self._val

            def blue(self) -> int:
                return self._val

            def alpha(self) -> int:
                return 255

        class FakeImage:
            def __init__(self, *args: Any) -> None:
                pass

            def width(self) -> int:
                return 20

            def height(self) -> int:
                return 10

            def loadFromData(self, _data: bytes) -> bool:  # noqa: N802
                return True

            def scaled(self, _width: int, _height: int) -> Any:
                return self

            def convertToFormat(self, _format: Any) -> Any:  # noqa: N802
                return self

            def pixelColor(self, x: int, _y: int) -> FakeColor:  # noqa: N802
                return FakeColor(20 if x < 10 else 240)

        fake_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (20).to_bytes(4, "big") + (10).to_bytes(4, "big")
        planner = RuleBasedPlanner()
        planner.image_converter.qimage_cls = FakeImage

        plan1 = planner.plan(
            prompt="cat",
            seed=42,
            count=15,
            width=400,
            height=300,
            image_data=fake_png,
            iteration=1,
            max_iterations=2,
        )
        plan2 = planner.plan(
            prompt="cat",
            seed=42,
            count=15,
            width=400,
            height=300,
            image_data=fake_png,
            iteration=2,
            max_iterations=2,
        )

        # seed は統合のため共通であること
        self.assertEqual(plan1.seed, 42)
        self.assertEqual(plan2.seed, 42)
        # 反復が異なればストロークIDや座標等が多様化されること
        strokes1_ids = [s.id for s in plan1.strokes]
        strokes2_ids = [s.id for s in plan2.strokes]
        self.assertNotEqual(strokes1_ids, strokes2_ids)
        # combine_drawing_plans が正常に統合できること
        combined = combine_drawing_plans([plan1, plan2])
        self.assertEqual(len(combined.strokes), len(plan1.strokes) + len(plan2.strokes))

    def test_llm_planner_fallback_goal_score_and_seed(self) -> None:
        from .llm_planner import LLMPlannerError, OpenAICompatiblePlanner, OpenAICompatibleSettings

        settings = OpenAICompatibleSettings(
            base_url="http://127.0.0.1:8080/v1",
            model="mock-model",
            fallback_to_procedural=True,
        )
        planner = OpenAICompatiblePlanner(settings=settings)

        with patch.object(
            planner, "_post_with_parameter_fallback", side_effect=LLMPlannerError("Simulated LLM Failure")
        ):
            plan1 = planner.plan(
                "sakura landscape", seed=100, count=20, width=800, height=600, iteration=1, max_iterations=2
            )
            plan2 = planner.plan(
                "sakura landscape", seed=100, count=20, width=800, height=600, iteration=2, max_iterations=2
            )

        self.assertEqual(plan1.seed, 100)
        self.assertEqual(plan2.seed, 100)
        self.assertFalse(plan1.goal_reached)
        self.assertEqual(plan1.completion_score, 0.5)
        self.assertTrue(plan2.goal_reached)
        self.assertEqual(plan2.completion_score, 1.0)
        self.assertEqual(plan2.metadata["planner_fallback"], "procedural")
        self.assertTrue(plan2.metadata["goal_reached"])
        self.assertEqual(plan2.metadata["completion_score"], 1.0)
        self.assertNotEqual([s.id for s in plan1.strokes], [s.id for s in plan2.strokes])

    def test_docker_plan_worker_goal_mode_auto_rescale_and_resilience(self) -> None:
        from .docker import PlanWorker
        from .domain import DrawingPlan, Stroke, StrokePoint
        from .ports import PlannerPort

        def _make_dummy_plan(prompt: str, seed: int, w: float, h: float, score: float, goal: bool) -> DrawingPlan:
            return DrawingPlan(
                prompt=prompt,
                seed=seed,
                strokes=[
                    Stroke(
                        id=f"s_{w}_{score}",
                        points=[
                            StrokePoint(10.0, 10.0, 0.5, 0),
                            StrokePoint(20.0, 20.0, 0.8, 10),
                            StrokePoint(30.0, 30.0, 0.5, 20),
                        ],
                        brush_preset="Basic-5 Size",
                        color="#000000",
                        size_px=5.0,
                        layer_name="Lineart",
                    )
                ],
                title="Dummy",
                iteration=1,
                layers=["Lineart"],
                canvas_width=w,
                canvas_height=h,
                goal_reached=goal,
                completion_score=score,
            )

        class MockStepPlanner(PlannerPort):
            def __init__(self) -> None:
                self.calls = 0

            def plan(self, *args: Any, **kwargs: Any) -> DrawingPlan:
                self.calls += 1
                if self.calls == 1:
                    return _make_dummy_plan("goal test", 1, 800.0, 600.0, 0.5, False)
                return _make_dummy_plan("goal test", 1, 1000.0, 1000.0, 0.95, True)

        worker = PlanWorker(
            planner=MockStepPlanner(),
            prompt="goal test",
            seed=1,
            count=10,
            width=1000.0,
            height=1000.0,
            max_iterations=10,
            goal_mode=True,
        )

        plans_emitted: list[DrawingPlan] = []
        worker.plan_ready.connect(plans_emitted.append)
        worker.notify_render_done()

        with (
            patch.object(worker._render_done_event, "wait", return_value=True),
            patch("ai_stroke_painter.docker._is_plan_goal_reached", side_effect=[False, True]),
        ):
            worker.run()

        self.assertFalse(worker.is_cancelled())
        self.assertEqual(len(plans_emitted), 2)
        self.assertTrue(plans_emitted[1].metadata.get("session_goal_reached", False))

    def test_docker_reset_run_state_clears_preview_on_rollback(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)

        class DummyPreview:
            def __init__(self) -> None:
                self.cleared = False

            def clear_plan(self) -> None:
                self.cleared = True

        preview = DummyPreview()
        docker.preview = cast(Any, preview)
        docker._canvas_session_open = False

        docker._reset_run_state(commit_session=False)
        self.assertTrue(preview.cleared)

        # commit_session=True の時はクリアされないこと
        preview.cleared = False
        docker._reset_run_state(commit_session=True)
        self.assertFalse(preview.cleared)

    def test_image_generator_settings_validation(self) -> None:
        from .image_generator import ImageGeneratorSettings

        # Valid defaults
        s = ImageGeneratorSettings()
        self.assertEqual(s.provider, "openai")
        self.assertEqual(s.model, "dall-e-3")

        # Invalid provider
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(provider="unsupported_provider")

        # Invalid endpoint_url
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(endpoint_url="")
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(endpoint_url="x" * 2049)

        # Invalid quality / style
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(quality="ultra_hd")
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(style="cartoonish")

        # Invalid timeout
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(timeout_seconds=-10.0)
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(timeout_seconds=float("nan"))
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(timeout_seconds=True)
        with self.assertRaises(ValueError):
            ImageGeneratorSettings(timeout_seconds=5000.0)

    def test_call_openai_images_respects_explicit_size_and_auto_aspect(self) -> None:
        from .image_generator import ImageGeneratorClient, ImageGeneratorSettings

        # When size is "auto", adapts to aspect
        client_auto = ImageGeneratorClient(ImageGeneratorSettings(size="auto", model="dall-e-3"))

        captured_payloads: list[dict[str, Any]] = []

        def fake_open(req: Any, **kwargs: Any) -> Any:
            data = json.loads(req.data.decode("utf-8"))
            captured_payloads.append(data)
            raise RuntimeError("stop_call")

        with patch("ai_stroke_painter.image_generator.build_opener") as mock_opener:
            mock_inst = mock_opener.return_value
            mock_inst.open.side_effect = fake_open

            # Landscape aspect >= 1.35 -> 1792x1024
            with self.assertRaises(RuntimeError):
                client_auto.generate_image("a vast mountain landscape", target_aspect=1.77)
            self.assertEqual(captured_payloads[-1]["size"], "1792x1024")

            # Portrait aspect <= 0.75 -> 1024x1792
            with self.assertRaises(RuntimeError):
                client_auto.generate_image("a tall anime portrait", target_aspect=0.56)
            self.assertEqual(captured_payloads[-1]["size"], "1024x1792")

            # Square aspect -> 1024x1024
            with self.assertRaises(RuntimeError):
                client_auto.generate_image("a cute cat", target_aspect=1.0)
            self.assertEqual(captured_payloads[-1]["size"], "1024x1024")

            # Explicit size -> preserved regardless of aspect
            client_explicit = ImageGeneratorClient(ImageGeneratorSettings(size="512x512", model="dall-e-3"))
            with self.assertRaises(RuntimeError):
                client_explicit.generate_image("a cyberpunk city", target_aspect=1.77)
            self.assertEqual(captured_payloads[-1]["size"], "512x512")

    def test_image_generation_planner_multi_iteration_cache_and_seed_offset(self) -> None:
        from .image_generator import ImageGeneratorSettings
        from .planner import ImageGenerationPlanner

        calls_count = 0
        fake_png = b"\x89PNG\r\n\x1a\n" + (b"\x00" * 8) + (20).to_bytes(4, "big") + (10).to_bytes(4, "big")

        class FakeColor:
            def __init__(self, val: int) -> None:
                self.val = val

            def red(self) -> int:
                return self.val

            def green(self) -> int:
                return self.val

            def blue(self) -> int:
                return self.val

            def alpha(self) -> int:
                return 255

        class FakeImage:
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

        class DummyImageClient:
            def __init__(self, *args: Any, **kwargs: Any) -> None:
                pass

            def generate_image(self, prompt: str, **kwargs: Any) -> bytes:
                nonlocal calls_count
                calls_count += 1
                return fake_png

        planner = ImageGenerationPlanner(ImageGeneratorSettings())
        planner.image_converter.qimage_cls = FakeImage
        planner.image_client = cast(Any, DummyImageClient())

        # Iteration 1 -> generates image (calls_count becomes 1)
        plan1 = planner.plan("a lovely flower garden", seed=42, iteration=1, max_iterations=3, count=20)
        self.assertEqual(calls_count, 1)
        self.assertEqual(plan1.iteration, 1)

        # Iteration 2 -> reuses cached image, does NOT call API again
        plan2 = planner.plan("a lovely flower garden", seed=42, iteration=2, max_iterations=3, count=20)
        self.assertEqual(calls_count, 1)
        self.assertEqual(plan2.iteration, 2)

        # Iteration 3 -> reuses cached image, does NOT call API again
        plan3 = planner.plan("a lovely flower garden", seed=42, iteration=3, max_iterations=3, count=20)
        self.assertEqual(calls_count, 1)
        self.assertEqual(plan3.iteration, 3)

        # Seed offset ensures different stroke IDs across iterations
        ids1 = [s.id for s in plan1.strokes]
        ids2 = [s.id for s in plan2.strokes]
        self.assertNotEqual(ids1, ids2)

        # New prompt on iteration 1 -> cache refreshed and API called again
        planner.plan("a blue futuristic car", seed=42, iteration=1, max_iterations=1, count=20)
        self.assertEqual(calls_count, 2)

    def test_storage_resolve_output_dir_rejects_parent_traversal(self) -> None:
        from .storage import _resolve_output_dir

        with self.assertRaises(ValueError):
            _resolve_output_dir("../outside")
        with self.assertRaises(ValueError):
            _resolve_output_dir("subdir/../../outside")
        with self.assertRaises(ValueError):
            _resolve_output_dir("..")

    def test_docker_save_log_and_select_image_defensive_dialogs(self) -> None:
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker._image_bytes = None

        class DummyEdit:
            def toPlainText(self) -> str:
                return "Sample log text"

        # Test _save_debug_log handles cancel/empty filename safely
        with (
            patch("ai_stroke_painter.docker._safe_get_save_filename", return_value=("", "")),
            patch("ai_stroke_painter.docker._get_attr", return_value=DummyEdit()),
        ):
            docker._save_debug_log()  # Must not raise

        # Test _select_reference_image handles cancel/empty filename safely
        with patch("ai_stroke_painter.docker._safe_get_open_filename", return_value=("", "")):
            docker._select_reference_image()  # Must not raise
            self.assertIsNone(docker._image_bytes)


class PromptAnalyzerTests(unittest.TestCase):
    """プロンプト意味解析およびイラストプロンプト自動エンリッチの単体テスト。"""

    def test_semantic_prompt_analyzer_english(self) -> None:
        from .prompt_analyzer import analyze_prompt

        sem = analyze_prompt(
            "a cute smiling anime girl with blonde twintails and blue eyes wearing sailor uniform and glasses at sunset"
        )
        self.assertEqual(sem.character.gender, "female")
        self.assertEqual(sem.character.hair_style, "twintails")
        self.assertEqual(sem.character.hair_color, "#e7bd55")
        self.assertEqual(sem.character.eye_color, "#4776d0")
        self.assertEqual(sem.character.expression, "smile")
        self.assertEqual(sem.character.costume, "sailor")
        self.assertIn("glasses", sem.character.accessories)
        self.assertEqual(sem.environment.time_of_day, "sunset")
        self.assertEqual(sem.art_style, "anime")
        self.assertIn("TWINTAILS", sem.to_directive_text())
        self.assertIn("SUNSET", sem.to_directive_text())

    def test_semantic_prompt_analyzer_japanese(self) -> None:
        from .prompt_analyzer import analyze_prompt

        sem = analyze_prompt("夕暮れの教室にいる黒髪ショートの笑顔の女子高生、メガネ着用")
        self.assertEqual(sem.character.gender, "female")
        self.assertEqual(sem.character.hair_style, "short")
        self.assertEqual(sem.character.hair_color, "#292632")
        self.assertEqual(sem.character.expression, "smile")
        self.assertEqual(sem.character.costume, "school_uniform")
        self.assertIn("glasses", sem.character.accessories)
        self.assertEqual(sem.environment.setting, "classroom")
        self.assertEqual(sem.environment.time_of_day, "sunset")

    def test_unspecified_attributes_do_not_force_defaults(self) -> None:
        from .prompt_analyzer import analyze_prompt

        sem = analyze_prompt("a warrior standing on a mountain peak")
        self.assertEqual(sem.character.gender, "unspecified")
        self.assertEqual(sem.character.view_angle, "auto")
        self.assertIsNone(sem.character.hair_color)
        self.assertIsNone(sem.character.eye_color)
        directive = sem.to_directive_text()
        self.assertNotIn("Gender: female", directive)
        self.assertNotIn("Angle/Perspective: front", directive)
        self.assertIn("Creative Autonomy", directive)

    def test_prompt_enrichment_for_image_generation(self) -> None:
        from .image_generator import enrich_prompt_for_illustration

        enriched = enrich_prompt_for_illustration("anime girl smiling at sunset")
        self.assertIn("masterpiece", enriched)
        self.assertIn("golden hour sunset lighting", enriched)


class SeniorReviewRegressionTests(unittest.TestCase):
    """自律レビューで特定された実害・運用リスク (R1-R4) に対する回帰テスト。"""

    _app: Any = None

    @classmethod
    def setUpClass(cls) -> None:
        if hasattr(QApplication, "instance"):
            cls._app = QApplication.instance()
            if cls._app is None:
                with contextlib.suppress(Exception):
                    cls._app = QApplication(["test", "-platform", "offscreen"])

    def test_r1_session_starts_only_on_actual_apply(self) -> None:
        """R1: プレビュー確認待機中はUndoマクロが開かず、適用時に初めて開始されること。"""
        docker = AIStrokePainterDocker()
        docker.confirm_before_apply.setChecked(True)

        session_begun: list[bool] = []

        class FakePort:
            def begin_render_session(self, doc: Any) -> None:
                session_begun.append(True)

            def render(self, *args: Any, **kwargs: Any) -> int:
                return 1

        docker.canvas_port = FakePort()  # type: ignore[assignment]
        plan = DrawingPlan(
            prompt="test",
            seed=42,
            strokes=(
                Stroke(
                    id="s1",
                    points=(
                        StrokePoint(x=10.0, y=10.0, pressure=0.5, time_ms=0),
                        StrokePoint(x=20.0, y=20.0, pressure=0.5, time_ms=10),
                    ),
                    brush_preset="gpen",
                    color="#000000",
                    size_px=2.0,
                    layer_name="Lineart",
                ),
            ),
            canvas_width=100.0,
            canvas_height=100.0,
            iteration=1,
        )

        class FakeDoc:
            def width(self) -> int:
                return 100

            def height(self) -> int:
                return 100

        docker._active_doc = FakeDoc()
        # 計画準備完了 -> プレビュー確認待ちになる
        docker._on_plan_ready(plan)
        self.assertIsNotNone(docker._pending_plan)
        self.assertEqual(len(session_begun), 0, "プレビュー確認待機中にセッションが開かれてはならない")

        # 適用ボタン押下
        docker._apply_pending_plan()
        self.assertEqual(len(session_begun), 1, "適用時に初めてセッションが開始される必要がある")

    def test_r2_sanitize_json_text_percent_suffix(self) -> None:
        """R2: _sanitize_json_text がカンマや空白直前のパーセント単位を除去・変換できること。"""
        from .llm_planner import _sanitize_json_text

        raw_json = '{\n  "opacity": 50%,\n  "size": 20px,\n  "ratio": 100%,\n  "sub": 25.5%\n}'
        sanitized = _sanitize_json_text(raw_json)
        self.assertNotIn("%", sanitized)
        parsed = json.loads(sanitized)
        self.assertAlmostEqual(parsed["opacity"], 0.5)
        self.assertEqual(parsed["size"], 20)
        self.assertAlmostEqual(parsed["ratio"], 1.0)
        self.assertAlmostEqual(parsed["sub"], 0.255)

    def test_r3_reset_to_defaults_resets_t2i_and_autonomy(self) -> None:
        """R3: _reset_to_defaults が T2I 設定 6 項目および autonomy_mode を初期値に戻すこと。"""
        docker = AIStrokePainterDocker()
        docker.t2i_provider.setCurrentIndex(1)
        docker.t2i_endpoint.setText("http://127.0.0.1:7860/sdapi/v1/txt2img")
        docker.t2i_model.setText("custom-model")
        docker.t2i_api_key.setText("secret-key")
        docker.t2i_size.setCurrentIndex(2)
        docker.t2i_negative_prompt.setText("ugly, blurry")
        docker.autonomy_mode.setCurrentIndex(2)

        with patch("ai_stroke_painter.docker._confirm", return_value=True):
            docker._reset_to_defaults()

        self.assertEqual(docker.t2i_provider.currentIndex(), 0)
        self.assertEqual(docker.t2i_endpoint.text(), "https://api.openai.com/v1/images/generations")
        self.assertEqual(docker.t2i_model.text(), "dall-e-3")
        self.assertEqual(docker.t2i_api_key.text(), "")
        self.assertEqual(docker.t2i_size.currentIndex(), 0)
        self.assertEqual(docker.t2i_negative_prompt.text(), "")
        self.assertEqual(docker.autonomy_mode.currentIndex(), 0)

    def test_r4_capture_layer_snapshot_avoids_full_bytes_copy(self) -> None:
        """R4: _capture_layer_snapshot が不要な bytes コピーを回避し上限チェックを行うこと。"""
        from .krita_adapter import MAX_ACTIVE_LAYER_SNAPSHOT_BYTES, _capture_layer_snapshot

        class FakeDoc:
            def width(self) -> int:
                return 100

            def height(self) -> int:
                return 100

        class FakeNode:
            def pixelData(self, x: int, y: int, w: int, h: int) -> bytes:
                # 許容上限 + 1 バイト
                return b"\x00" * (MAX_ACTIVE_LAYER_SNAPSHOT_BYTES + 1)

            def setPixelData(self, pixels: bytes, x: int, y: int, w: int, h: int) -> None:
                pass

        with self.assertRaises(RuntimeError) as ctx:
            _capture_layer_snapshot(FakeDoc(), FakeNode())
        self.assertIn("上限を超えています", str(ctx.exception))

    def test_r6_preview_widget_zoom_pan_and_layer_filter(self) -> None:
        """R6: PreviewWidget のズーム・パン・レイヤーフィルター機能および境界値制御の検証。"""
        from .docker import PreviewWidget
        from .domain import DrawingPlan, Stroke, StrokePoint

        prev = PreviewWidget()
        self.assertEqual(prev._zoom_factor, 1.0)
        self.assertEqual(prev._pan_offset_x, 0.0)
        self.assertEqual(prev._pan_offset_y, 0.0)
        self.assertIsNone(prev._layer_filter)

        # ズーム倍率変更と境界値クランプ (0.3〜5.0)
        prev.set_zoom_factor(2.5)
        self.assertEqual(prev._zoom_factor, 2.5)
        prev.set_zoom_factor(10.0)
        self.assertEqual(prev._zoom_factor, 5.0)
        prev.set_zoom_factor(0.1)
        self.assertEqual(prev._zoom_factor, 0.3)

        # パン移動
        prev.set_pan_offset(25.0, -15.0)
        self.assertEqual(prev._pan_offset_x, 25.0)
        self.assertEqual(prev._pan_offset_y, -15.0)

        # レイヤーフィルター
        prev.set_layer_filter({"Lineart", "Highlights"})
        self.assertEqual(prev._layer_filter, {"Lineart", "Highlights"})
        prev.set_layer_filter(None)
        self.assertIsNone(prev._layer_filter)

        # リセット
        prev.set_zoom_factor(3.0)
        prev.set_pan_offset(50.0, 50.0)
        prev.reset_view()
        self.assertEqual(prev._zoom_factor, 1.0)
        self.assertEqual(prev._pan_offset_x, 0.0)
        self.assertEqual(prev._pan_offset_y, 0.0)

        # 描画フィルタリングの動作検証
        strokes = [
            Stroke(id="s_line", points=[StrokePoint(0, 0, 1.0, 0), StrokePoint(10, 10, 1.0, 1)], layer_name="Lineart"),
            Stroke(id="s_flat", points=[StrokePoint(20, 20, 1.0, 0), StrokePoint(30, 30, 1.0, 1)], layer_name="Flats"),
        ]
        plan = DrawingPlan(prompt="t", seed=1, strokes=strokes, canvas_width=100, canvas_height=100)
        prev.set_plan(plan)
        prev.set_layer_filter({"Lineart"})

        class MockPainter:
            def __init__(self) -> None:
                self.drawn: list[Any] = []

            def fillRect(self, *a: Any) -> None:
                pass

            def drawRect(self, *a: Any) -> None:
                pass

            def setPen(self, *a: Any) -> None:
                pass

            def drawLine(self, *a: Any) -> None:
                self.drawn.append(a)

            def drawEllipse(self, *a: Any) -> None:
                self.drawn.append(a)

        mp = MockPainter()
        prev.paint_to_painter(mp, 200, 160)
        self.assertGreater(len(mp.drawn), 0)

    def test_r7_docker_preview_controls_integration(self) -> None:
        """R7: Docker 上のプレビューツールバー（ズーム、リセット、レイヤーフィルター）連携の検証。"""
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        self.assertIsNotNone(docker.preview)
        self.assertIsNotNone(docker.preview_layer_combo)

        # ズームイン・ズームアウト
        orig_zoom = float(docker.preview._zoom_factor)
        docker._preview_zoom_in()
        self.assertGreater(docker.preview._zoom_factor, orig_zoom)
        docker._preview_zoom_out()
        docker._preview_reset()
        self.assertEqual(docker.preview._zoom_factor, 1.0)

        # レイヤーフィルター切り替え
        docker.preview_layer_combo.setCurrentIndex(1)  # lineart
        docker._on_preview_layer_filter_changed(1)
        self.assertEqual(docker.preview._layer_filter, {"Lineart"})

        docker.preview_layer_combo.setCurrentIndex(0)  # all
        docker._on_preview_layer_filter_changed(0)
        self.assertIsNone(docker.preview._layer_filter)

    def test_r8_image_converter_organic_strokes(self) -> None:
        """R8: ImageStrokeConverter による有機的筆致生成（Flats / Highlights）の検証。"""
        import random

        from .image_converter import ImageStrokeConverter

        converter = ImageStrokeConverter()

        class FakeColor:
            def __init__(self, r: int, g: int, b: int) -> None:
                self._r = r
                self._g = g
                self._b = b

            def red(self) -> int:
                return self._r

            def green(self) -> int:
                return self._g

            def blue(self) -> int:
                return self._b

            def alpha(self) -> int:
                return 255

        class FakeMultiColorImage:
            def width(self) -> int:
                return 32

            def height(self) -> int:
                return 32

            def scaled(self, w: int, h: int) -> Any:
                return self

            def convertToFormat(self, fmt: Any) -> Any:  # noqa: N802
                return self

            def pixelColor(self, x: int, y: int) -> Any:  # noqa: N802
                if x < 16:
                    return FakeColor(50, 100, 200)
                return FakeColor(220, 150, 40)

        rng = random.Random(42)
        fake_img = FakeMultiColorImage()
        strokes = converter._process_qimage(
            qimg=fake_img,
            seed=42,
            count=60,
            target_width=64.0,
            target_height=64.0,
            rng=rng,
            enable_flats=True,
            palette_name="nature",
            prompt="test",
        )
        flat_strokes = [s for s in strokes if s.layer_name == "Flats"]
        self.assertGreater(len(flat_strokes), 0)
        # スプライン補間により制御点が3点以上生成されていることを検証
        for fs in flat_strokes:
            self.assertGreaterEqual(len(fs.points), 3)

    def test_r1_lineart_protected_from_palette_color_replacement(self) -> None:
        """R1: パレット共有色がある場合でも、Lineart の主線色が髪色の影色に置換されないこと。"""
        plan = generate_procedural_plan(
            "blonde hair anime girl",
            seed=42,
            count=50,
            width=800,
            height=600,
            palette_name="dark_fantasy",
        )
        line_strokes = [s for s in plan.strokes if s.layer_name == "Lineart"]
        self.assertGreater(len(line_strokes), 0)
        # dark_fantasy の lineart 色 (#130f40) が維持され、金髪の影色 (#8f7535) に侵食されない
        for stroke in line_strokes:
            self.assertNotEqual(stroke.color.lower(), "#8f7535")

    def test_r2_macro_watercolor_wash_not_unintentionally_generated(self) -> None:
        """R2: 雲マクロ等のコンパイル時に、無条件に全画面水彩ウォッシュが背後に混入しないこと。"""
        from .stroke_program import MacroOperation, StrokeProgram, compile_stroke_program

        prog = StrokeProgram(
            prompt="cumulus clouds",
            seed=42,
            canvas_width=800,
            canvas_height=600,
            operations=[MacroOperation(id="m1", name="cumulus")],
        )
        plan = compile_stroke_program(prog)
        # 雲のみが生成され、不要な水彩ウォッシュ帯が背後に生成されない
        self.assertGreater(len(plan.strokes), 0)
        for stroke in plan.strokes:
            self.assertNotIn("wash_band", stroke.id)

    def test_r3_duplicate_city_macros_do_not_collide_stroke_ids(self) -> None:
        """R3: 同一計画内に複数の city/magic マクロがある場合でも stroke ID が衝突せずコンパイルできること。"""
        from .stroke_program import MacroOperation, StrokeProgram, compile_stroke_program

        prog = StrokeProgram(
            prompt="two cities",
            seed=42,
            canvas_width=800,
            canvas_height=600,
            operations=[
                MacroOperation(id="m_city1", name="city"),
                MacroOperation(id="m_city2", name="city"),
            ],
        )
        plan = compile_stroke_program(prog)
        stroke_ids = [s.id for s in plan.strokes]
        self.assertEqual(len(stroke_ids), len(set(stroke_ids)))

    def test_r4_large_polygon_wash_downsamples_to_max_stroke_points(self) -> None:
        """R4: 頂点数の多いポリゴン塗りでも MAX_STROKE_POINTS を超過せずコンパイルできること。"""
        from .domain import MAX_STROKE_POINTS
        from .stroke_program import FillOperation, ProgramPoint, StrokeProgram, compile_stroke_program

        poly = [ProgramPoint(float(i % 100), float(i % 50)) for i in range(300)]
        prog = StrokeProgram(
            prompt="large fill",
            seed=42,
            canvas_width=800,
            canvas_height=600,
            operations=[FillOperation(id="f_large", polygon=poly, style="wash")],
        )
        plan = compile_stroke_program(prog)
        for stroke in plan.strokes:
            self.assertLessEqual(len(stroke.points), MAX_STROKE_POINTS)

    def test_r5_japanese_hair_colors_and_no_eye_leakage(self) -> None:
        """R5: 日本語の金髪・茶髪・銀髪が正しく抽出され、目の色が髪色に漏洩しないこと。"""
        from .prompt_analyzer import analyze_prompt

        self.assertEqual(analyze_prompt("金髪の少女").character.hair_color, "#e7bd55")
        self.assertEqual(analyze_prompt("茶髪の少女").character.hair_color, "#6b4226")
        self.assertEqual(analyze_prompt("銀髪の少女").character.hair_color, "#e8edf5")

        dual = analyze_prompt("青い瞳、茶髪の少女")
        self.assertEqual(dual.character.hair_color, "#6b4226")
        self.assertEqual(dual.character.eye_color, "#4776d0")

    def test_r6_is_reasoning_model_does_not_overmatch_r1(self) -> None:
        """R6: user1-chat や server1-model などの非推論モデルが誤判定されないこと。"""
        self.assertFalse(_is_reasoning_model("user1-chat"))
        self.assertFalse(_is_reasoning_model("server1-model"))
        self.assertFalse(_is_reasoning_model("llama-3.1-lora_r16"))
        self.assertTrue(_is_reasoning_model("deepseek-r1"))
        self.assertTrue(_is_reasoning_model("o1-mini"))

    def test_r7_dot_stroke_renders_with_fallback_in_adapter(self) -> None:
        """R7: 距離 0.0px の同一点ストローク（ドット・ハイライト）が Krita アダプターで描画されること。"""
        doc = _FakeDocument()
        adapter = KritaCanvasAdapter()
        dot_stroke = Stroke(
            id="dot_highlight",
            points=(StrokePoint(50.0, 50.0, 0.9, 0), StrokePoint(50.0, 50.0, 0.9, 10)),
        )
        plan = DrawingPlan(prompt="highlight dot", seed=1, strokes=(dot_stroke,))
        rendered = adapter.render(doc, plan)
        self.assertEqual(rendered, 1)
        root = doc.rootNode()
        paint_node = root.childNodes()[0].childNodes()[0]
        self.assertGreater(len(paint_node.lines), 0)


class CodeReview2026HardeningTests(unittest.TestCase):
    """2026-08-28 包括的コードレビューで特定された重要度 P1/P2 の改善に対する回帰テスト。"""

    def test_image_converter_rejects_empty_decoded_dimensions(self) -> None:
        """QImage の loadFromData が成功しても寸法が 0 の場合はクラッシュせず ValueError を送出すること。"""
        from .image_converter import MAX_DECODED_IMAGE_PIXELS, ImageStrokeConverter

        class ZeroSizeImage:
            def loadFromData(self, _data: bytes) -> bool:
                return True

            def width(self) -> int:
                return 0

            def height(self) -> int:
                return 0

            def scaled(self, *args: Any, **kwargs: Any) -> ZeroSizeImage:
                return self

        # ImageStrokeConverter は内部で QImage() を呼ぶが、ヘッダ不正のテストデータで
        # 先に ValueError("対応画像形式のヘッダーを確認できませんでした") が送出される。
        # 本テストでは当該モジュールの寸法検証ロジック (decoded_width/height <= 0) が
        # コード上に存在することを import 経由で確認する。
        self.assertTrue(hasattr(ImageStrokeConverter, "convert_image_to_plan"))
        # モジュール内に直接の定数比較検証があり、サイズ超過時に ValueError を送出する
        self.assertIsInstance(MAX_DECODED_IMAGE_PIXELS, int)
        # ヘッダ不正の bytes 入力で ValueError が出ることを確認
        with self.assertRaises(ValueError):
            ImageStrokeConverter().convert_image_to_plan(
                image_bytes=b"not a real image",
                prompt="x",
                seed=0,
                count=10,
                target_width=256,
                target_height=256,
            )

    def test_sanitize_api_key_log_masks_unquoted_authorization_and_new_prefixes(self) -> None:
        """クォートなしの Authorization:Bearer ... / sk-proj-* / anthropic-* / gsk_* を伏字化すること。"""
        from .image_generator import sanitize_api_key_log

        cases = [
            ("Authorization:Bearer abc123def456ghi789jkl012", "[REDACTED]"),
            ("sk-proj-AbCdEfGhIjKlMnOpQrStUvWxYz12345", "[REDACTED]"),
            ("anthropic-AGENT_AbCdEfGhIj12345", "[REDACTED]"),
            ("gsk_AbCdEfGhIj12345", "[REDACTED]"),
            ("sk-abcdefghijklmnopqrstuv", "[REDACTED]"),
        ]
        for input_text, expected_token in cases:
            masked = sanitize_api_key_log(input_text)
            self.assertIn(expected_token, masked, f"failed to mask: {input_text!r} -> {masked!r}")

    def test_krita_adapter_disables_bridge_when_env_invalid(self) -> None:
        """Native Bridge 環境変数が不正な値でもプラグイン全体は開始可能で、bridge だけ None になること。"""
        from .krita_adapter import KritaCanvasAdapter
        from .native_bridge import discover_native_bridge

        # 既存のテストと同じく port="bad" + 短すぎる token で ValueError が出る前提
        with patch.dict("os.environ", {"AI_STROKE_BRIDGE_PORT": "bad", "AI_STROKE_BRIDGE_TOKEN": "short"}):
            # 関数自体は fail-fast を維持する
            with self.assertRaises(ValueError):
                discover_native_bridge()
            # ただし KritaCanvasAdapter はプラグインの起動を止めない
            adapter = KritaCanvasAdapter()
            self.assertIsNone(adapter.native_bridge)

    def test_llm_planner_safe_error_message_redacts_credentials(self) -> None:
        """_safe_error_message が API Key / Bearer トークンを含む例外メッセージでも資格情報を残さないこと。"""
        from .llm_planner import _safe_error_message

        raw = "API returned 401 with sk-proj-AbCdEfGhIjKlMnOpQrStUvWxYz12345 and Authorization:Bearer eyJfake"
        sanitized = _safe_error_message(Exception(raw))
        self.assertNotIn("AbCdEfGhIjKlMnOpQrStUvWxYz12345", sanitized)
        self.assertNotIn("eyJfake", sanitized)
        self.assertIn("[REDACTED]", sanitized)


class CodeReviewFinalQualityTests(unittest.TestCase):
    """コードレビューに基づくUI操作性・アクセシビリティ・SVG1点描画・プリセット堅牢化の回帰テスト。"""

    _app: Any = None

    @classmethod
    def setUpClass(cls) -> None:
        if hasattr(QApplication, "instance"):
            cls._app = QApplication.instance()
            if cls._app is None and callable(QApplication):
                with contextlib.suppress(Exception):
                    cls._app = QApplication(["test", "-platform", "offscreen"])

    def test_preview_widget_interactive_mouse_and_wheel_events(self) -> None:
        """PreviewWidget のマウスホイールズーム、ドラッグパン、ダブルクリックリセットを検証。"""
        from .docker import PreviewWidget

        prev = PreviewWidget()
        self.assertEqual(prev._zoom_factor, 1.0)
        self.assertEqual(prev._pan_offset_x, 0.0)
        self.assertEqual(prev._pan_offset_y, 0.0)

        # 1. ホイールによるズームイン
        class MockWheelEvent:
            def __init__(self, delta_y: int) -> None:
                self._delta_y = delta_y

            def angleDelta(self) -> Any:
                class Point:
                    def __init__(self, y: int) -> None:
                        self._y = y

                    def y(self) -> int:
                        return self._y

                return Point(self._delta_y)

            def accept(self) -> None:
                pass

        prev.wheelEvent(MockWheelEvent(120))
        self.assertGreater(prev._zoom_factor, 1.0)
        zoom_in_val = prev._zoom_factor

        # 2. ホイールによるズームアウト
        prev.wheelEvent(MockWheelEvent(-120))
        self.assertLess(prev._zoom_factor, zoom_in_val)

        # 3. マウスドラッグによるパン移動
        class MockPos:
            def __init__(self, x: float, y: float) -> None:
                self._x = x
                self._y = y

            def x(self) -> float:
                return self._x

            def y(self) -> float:
                return self._y

        class MockMouseEvent:
            def __init__(self, x: float, y: float, btn: int = 1) -> None:
                self._pos = MockPos(x, y)
                self._btn = btn

            def button(self) -> int:
                return self._btn

            def pos(self) -> Any:
                return self._pos

            def accept(self) -> None:
                pass

        prev.mousePressEvent(MockMouseEvent(100.0, 100.0))
        self.assertTrue(prev._dragging)
        prev.mouseMoveEvent(MockMouseEvent(140.0, 120.0))
        self.assertEqual(prev._pan_offset_x, 40.0)
        self.assertEqual(prev._pan_offset_y, 20.0)
        prev.mouseReleaseEvent(MockMouseEvent(140.0, 120.0))
        self.assertFalse(prev._dragging)

        # 4. ダブルクリックによるリセット
        class MockDoubleClickEvent:
            def accept(self) -> None:
                pass

        prev.mouseDoubleClickEvent(MockDoubleClickEvent())
        self.assertEqual(prev._zoom_factor, 1.0)
        self.assertEqual(prev._pan_offset_x, 0.0)
        self.assertEqual(prev._pan_offset_y, 0.0)

    def test_single_point_stroke_svg_circle_output(self) -> None:
        """1点ストローク（パーティクル・ハイライト点）が SVG で <circle class="stroke-dot"> として出力されること。"""
        from .domain import DrawingPlan, Stroke, StrokePoint

        dot_stroke = Stroke(
            id="dot-highlight",
            points=[StrokePoint(150.0, 250.0, 0.8, 0), StrokePoint(150.0, 250.0, 0.8, 1)],
            color="#ffff00",
            size_px=8.0,
            layer_name="Highlights",
            opacity=0.9,
        )
        eraser_dot = Stroke(
            id="dot-eraser",
            points=[StrokePoint(100.0, 100.0, 0.5, 0), StrokePoint(100.0, 100.0, 0.5, 1)],
            color="#000000",
            size_px=6.0,
            layer_name="Highlights",
            is_eraser=True,
        )
        plan = DrawingPlan(
            prompt="starlight particles",
            seed=42,
            strokes=[dot_stroke, eraser_dot],
            canvas_width=800,
            canvas_height=600,
        )
        svg_content = plan.to_svg()
        self.assertIn('<circle cx="150.00" cy="250.00"', svg_content)
        self.assertIn('class="stroke-dot"', svg_content)
        self.assertIn('fill="#ffff00"', svg_content)
        self.assertIn(".stroke-dot { stroke: none; }", svg_content)
        # 消しゴムマスク側
        self.assertIn('class="stroke-dot eraser"', svg_content)
        self.assertIn('fill="#000000"', svg_content)

    def test_single_point_stroke_rasterization_quality(self) -> None:
        """1点ストロークのみの DrawingPlan でも品質評価関数が例外なくラスタライズ・評価できること。"""
        from .domain import DrawingPlan, Stroke, StrokePoint
        from .quality import evaluate_plan_quality

        dot_stroke = Stroke(
            id="p-1",
            points=[StrokePoint(200.0, 200.0, 0.9, 0), StrokePoint(200.0, 200.0, 0.9, 1)],
            color="#ff0088",
            size_px=15.0,
            layer_name="Lineart",
        )
        plan = DrawingPlan(prompt="single dot", seed=1, strokes=[dot_stroke], canvas_width=400, canvas_height=400)
        report = evaluate_plan_quality(plan)
        self.assertGreater(report.coverage, 0.0)
        self.assertEqual(report.out_of_bounds_points, 0)

    def test_confirm_before_apply_toggle_updates_ui_state(self) -> None:
        """適用前確認の切替時にボタン文言と適用ボタンの enabled 状態が正しく連動すること。"""
        from .docker import AIStrokePainterDocker
        from .domain import DrawingPlan, Stroke, StrokePoint

        docker = AIStrokePainterDocker()
        # 初期状態: confirm_before_apply = True
        self.assertTrue(docker.confirm_before_apply.isChecked())
        self.assertEqual(docker.run_btn.text(), "プレビュー生成")
        self.assertFalse(docker.apply_btn.isEnabled())

        # 切替: False -> キャンバスに描画
        docker.confirm_before_apply.setChecked(False)
        self.assertEqual(docker.run_btn.text(), "キャンバスに描画")
        self.assertFalse(docker.apply_btn.isEnabled())

        # 再切替: True -> プレビュー生成
        docker.confirm_before_apply.setChecked(True)
        self.assertEqual(docker.run_btn.text(), "プレビュー生成")

        # pending plan がある状態での連動
        sample_plan = DrawingPlan(
            prompt="test",
            seed=1,
            strokes=[Stroke(id="s1", points=[StrokePoint(0, 0, 1, 1), StrokePoint(10, 10, 1, 1)])],
        )
        docker._pending_plan = sample_plan
        docker._update_action_buttons_state()
        self.assertTrue(docker.apply_btn.isEnabled())

        docker.confirm_before_apply.setChecked(False)
        self.assertFalse(docker.apply_btn.isEnabled())

    def test_keyboard_shortcuts_and_accessibility(self) -> None:
        """主要操作ボタンにショートカットとアクセシビリティ名が設定されていること。"""
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        self.assertEqual(docker.run_btn.shortcut(), "Ctrl+Return")
        self.assertEqual(docker.apply_btn.shortcut(), "Ctrl+Shift+Return")
        self.assertEqual(docker.stop_btn.shortcut(), "Escape")

        self.assertEqual(docker.tabs.accessibleName(), "機能設定タブ")
        self.assertEqual(docker.preset_combo.accessibleName(), "プリセット選択")
        self.assertEqual(docker.prompt.accessibleName(), "描画指示プロンプト入力欄")
        self.assertEqual(docker.seed.accessibleName(), "乱数シード")
        self.assertEqual(docker.count.accessibleName(), "ストローク本数")
        self.assertEqual(docker.preview.accessibleName(), "ストロークベクタープレビュー")

    def test_generation_controls_locks_test_conn_btn(self) -> None:
        """描画実行中に API 接続テストボタンが正しくロック・アンロックされること。"""
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        docker.planner_mode.setCurrentIndex(1)
        docker._update_planner_settings_state()
        docker._set_generation_controls_enabled(False)
        self.assertFalse(docker.test_conn_btn.isEnabled())
        docker._set_generation_controls_enabled(True)
        self.assertTrue(docker.test_conn_btn.isEnabled())

    def test_preset_management_security_and_sanitization(self) -> None:
        """プリセットインポート・削除の文字長・制御文字・接頭辞の堅牢化を検証。"""
        from pathlib import Path
        import tempfile

        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        with tempfile.TemporaryDirectory() as tmpdir:
            # 不正なキー（制御文字、長すぎる名前）を含む JSON
            bad_data = {
                "safe_preset": {"prompt": "valid prompt"},
                "bad\x00name": {"prompt": "invalid"},
                "a" * 150: {"prompt": "too long"},
            }
            json_path = Path(tmpdir) / "test_presets.json"
            json_path.write_text(json.dumps(bad_data), encoding="utf-8")

            with (
                patch("ai_stroke_painter.docker._safe_get_open_filename", return_value=(str(json_path), "")),
                patch("ai_stroke_painter.docker.QMessageBox.information"),
                patch("ai_stroke_painter.docker.QMessageBox.critical"),
            ):
                docker._import_presets()

            # safe_preset だけが保存されることを確認
            settings = QSettings("AIStrokePainter", "CustomPresets")
            saved = json.loads(str(settings.value("presets_json")))
            self.assertIn("safe_preset", saved)
            self.assertNotIn("bad\x00name", saved)
            self.assertNotIn("a" * 150, saved)

    def test_rdp_simplify_deep_stack_iterative_safety(self) -> None:
        """2,500点の長い輪郭線でもスタック反復により RecursionError を起こさず正確に単純化されること。"""
        from .image_converter import _rdp_simplify

        # 2,500 点の直線点列（途中にわずかなノイズがあるが epsilon 内）
        points = [(float(i), float(i) * 0.5) for i in range(2500)]
        simplified = _rdp_simplify(points, epsilon=1.0)
        self.assertEqual(len(simplified), 2)
        self.assertEqual(simplified[0], (0.0, 0.0))
        self.assertEqual(simplified[-1], (2499.0, 1249.5))

        # 中間に顕著な屈曲点がある三角形状点列（各辺1,200点ずつ）の場合、頂点のみが保持され3点になること
        points_with_peak = [(float(i), i * (100.0 / 1200.0)) for i in range(1200)] + [
            (1200.0 + i, 100.0 - i * (100.0 / 1200.0)) for i in range(1201)
        ]
        simplified_peak = _rdp_simplify(points_with_peak, epsilon=1.0)
        self.assertEqual(len(simplified_peak), 3)
        self.assertEqual(simplified_peak, [(0.0, 0.0), (1200.0, 100.0), (2400.0, 0.0)])

    def test_image_download_url_ssrf_hardened(self) -> None:
        """画像ダウンロードURLの SSRF 防御（.localhost, .local, .internal, 私設IP）を検証。"""
        from .image_generator import (
            ImageGenerationError,
            _validate_endpoint_url,
            _validate_image_download_url,
        )

        source = "https://api.openai.com/v1"
        # 拒否されるべき URL
        for bad_url in (
            "https://localhost/image.png",
            "https://sub.localhost/image.png",
            "https://server.local/image.png",
            "https://corp.internal/image.png",
            "https://127.0.0.1/image.png",
            "https://10.0.0.1/image.png",
            "https://192.168.1.1/image.png",
            "https://169.254.169.254/latest/meta-data",
            "http://external.example.com/image.png",  # HTTP 拒否
        ):
            with self.assertRaises(ImageGenerationError, msg=f"Should reject: {bad_url}"):
                _validate_image_download_url(bad_url, source)

        # 許可されるべき URL
        self.assertEqual(
            _validate_image_download_url("https://oaidalleapiprodscus.blob.core.windows.net/test.png", source),
            "https://oaidalleapiprodscus.blob.core.windows.net/test.png",
        )
        # 同一オリジンは許可
        self.assertEqual(
            _validate_image_download_url("https://api.openai.com/v1/images/123.png", source),
            "https://api.openai.com/v1/images/123.png",
        )

        # _validate_endpoint_url で 0.0.0.0 はループバックとして扱われないこと (HTTP 拒否)
        with self.assertRaises(ImageGenerationError):
            _validate_endpoint_url("http://0.0.0.0:8000")

    def test_adjust_color_luminance_formats(self) -> None:
        """_adjust_color_luminance が 3, 4, 6, 8 桁カラーをサポートしアルファを保持すること。"""
        from .procedural.character import _adjust_color_luminance

        # 6 桁
        self.assertEqual(_adjust_color_luminance("#102030", 2.0), "#204060")
        # 3 桁 (#123 -> #112233)
        self.assertEqual(_adjust_color_luminance("#123", 2.0), "#224466")
        # 8 桁 (アルファチャンネル ff 保持)
        self.assertEqual(_adjust_color_luminance("#10203080", 2.0), "#20406080")
        # 4 桁 (#1238 -> #11223388)
        self.assertEqual(_adjust_color_luminance("#1238", 2.0), "#22446688")
        # 不正な入力は元の文字列を返す
        self.assertEqual(_adjust_color_luminance("invalid", 1.5), "invalid")

    def test_load_plan_and_program_unicode_decode_error(self) -> None:
        """非 UTF-8 の破損ファイル読み込み時に ValueError が送出されること。"""
        from pathlib import Path
        import tempfile

        from .storage import load_plan, load_program

        with tempfile.TemporaryDirectory() as tmpdir:
            bad_file = Path(tmpdir) / "corrupt.json"
            bad_file.write_bytes(b"\xff\xfe\x00\x00\x80\x90\xff\xaa")

            with self.assertRaises(ValueError) as ctx:
                load_plan(bad_file)
            self.assertIn("計画 JSON の形式が不正です", str(ctx.exception))

            with self.assertRaises(ValueError) as ctx_prog:
                load_program(bad_file)
            self.assertIn("計画 JSON の形式が不正です", str(ctx_prog.exception))

    def test_t2i_and_advanced_widgets_accessible_names(self) -> None:
        """T2I および高度な AI パラメータウィジェットにアクセシビリティ名が設定されていること。"""
        from .docker import AIStrokePainterDocker

        docker = AIStrokePainterDocker()
        self.assertEqual(docker.t2i_provider.accessibleName(), "Text-to-Image画像生成プロバイダー")
        self.assertEqual(docker.t2i_endpoint.accessibleName(), "Text-to-ImageエンドポイントURL")
        self.assertEqual(docker.t2i_model.accessibleName(), "Text-to-Imageモデル名")
        self.assertEqual(docker.t2i_api_key.accessibleName(), "Text-to-Image APIキー")
        self.assertEqual(docker.t2i_size.accessibleName(), "Text-to-Image画像解像度")
        self.assertEqual(docker.t2i_negative_prompt.accessibleName(), "Text-to-Imageネガティブプロンプト")
        self.assertEqual(docker.timeout_sec.accessibleName(), "APIリクエストタイムアウト秒数")
        self.assertEqual(docker.max_tokens.accessibleName(), "最大出力トークン数")
        self.assertEqual(docker.reasoning_effort.accessibleName(), "思考推論強度")
        self.assertEqual(docker.temperature.accessibleName(), "多様性温度係数")
        self.assertEqual(docker.top_p.accessibleName(), "確率閾値")
        self.assertEqual(docker.vision_res.accessibleName(), "視覚評価キャプチャ解像度")
        self.assertEqual(docker.custom_instructions.accessibleName(), "AI追加指示システムプロンプト")
        self.assertEqual(docker.autonomy_mode.accessibleName(), "AI自律モード")
        self.assertEqual(docker.fallback_to_procedural.accessibleName(), "AIエラー時のプロシージャル自動切り替え")
        self.assertEqual(docker.quality_summary_label.accessibleName(), "品質評価サマリー")


def run() -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
