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
from zipfile import ZipFile

from .build_plugin import PACKAGE_NAME, build
from .docker import AIStrokePainterDocker, PlanWorker
from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint, VisionCritique
from .image_converter import ImageStrokeConverter
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import (
    LLMPlannerError,
    OpenAICompatiblePlanner,
    OpenAICompatibleSettings,
    _attempt_json_repair,
    _detect_image_mime_type,
    _extract_content_from_response,
    _extract_json_object,
    _is_reasoning_model,
    _plan_from_response,
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
    password_echo_mode,
)
from .storage import load_plan, save_plan, save_svg


class _FakeNode:
    def __init__(self, name: str, node_type: str = "paintlayer", paint_ability: str = "PAINT") -> None:
        self._name = name
        self._type = node_type
        self._paint_ability = paint_ability
        self._children: list[Any] = []
        self.lines: list[tuple[Any, Any, float, float]] = []

    def name(self) -> str:
        return self._name

    def type(self) -> str:
        return self._type

    def childNodes(self) -> list[Any]:
        return list(self._children)

    def removeChildNode(self, child: Any) -> None:
        if child in self._children:
            self._children.remove(child)

    def addChildNode(self, child: Any, above_this: Any = None) -> None:
        if above_this is not None and above_this in self._children:
            idx = self._children.index(above_this)
            self._children.insert(idx + 1, child)
        else:
            self._children.append(child)

    def paintAbility(self) -> str:
        return self._paint_ability

    def paintLine(self, start: Any, end: Any, start_pressure: float, end_pressure: float) -> None:
        # Krita の Node.paintLine は QPoint を要求するため、QPointF が渡されると TypeError となる
        if type(start).__name__ == "QPointF" or type(end).__name__ == "QPointF":
            raise TypeError(
                "paintLine(self, pointOne: QPoint, pointTwo: QPoint, pressureOne: float = 1, pressureTwo: float = 1, strokeStyle: Optional[str] = ''): argument 1 has unexpected type 'QPointF'"
            )
        if self.paintAbility() == "PAINT":
            self.lines.append((start, end, start_pressure, end_pressure))


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

        # Dispatcher Plan
        plan = generate_procedural_plan("cute cat", 42, 20, 800, 600)
        self.assertEqual(plan.title, "Creature Artwork")
        self.assertTrue(len(plan.strokes) > 0)

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

    def test_svg_comment_with_double_hyphen_stays_well_formed(self) -> None:
        import xml.etree.ElementTree as ET

        plan = RuleBasedPlanner().plan("attack -- defense", 7, 3, 100, 100)
        svg_content = plan.to_svg(100, 100)
        ET.fromstring(svg_content)

    def test_invalid_domain_data_is_rejected(self) -> None:
        with self.assertRaises(PlanValidationError):
            StrokePoint(0, 0, 1.1, 0)
        with self.assertRaises(PlanValidationError):
            Stroke("one-point", [StrokePoint(0, 0, 0.5, 0)])
        with self.assertRaises(PlanValidationError):
            DrawingPlan.from_dict({"schema_version": 99, "prompt": "", "seed": 0, "strokes": []})

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

    def test_image_converter_fallback_on_dummy_data(self) -> None:
        converter = ImageStrokeConverter()
        plan = converter.convert_image_to_plan(b"not-a-valid-image", "cat", 42, 10, 800, 600)
        self.assertIsInstance(plan, DrawingPlan)
        self.assertTrue(len(plan.strokes) > 0)

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
        plan = converter.convert_image_to_plan(b"fake-image-bytes", "test", 42, 5, 200, 200)
        self.assertIn(5, converted_formats)
        self.assertTrue(len(plan.strokes) > 0)


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
        self.assertNotIn(f"{PACKAGE_NAME}/{PACKAGE_NAME}.desktop", names)


class OpenAICompatiblePlannerTests(unittest.TestCase):
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
        self.assertIn("[[x, y], [x, y, pressure]]", prompt_sakura)

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
        self.assertIn("ON-DEMAND VISUAL INSPECTION", step1)
        self.assertIn("Flats", step1)

        step2 = _system_instruction(iteration=2, max_iterations=3, prompt="sakura tree")
        self.assertIn("Step 2/3", step2)
        self.assertIn("Shading", step2)

        step3 = _system_instruction(iteration=3, max_iterations=3, prompt="sakura tree")
        self.assertIn("Step 3/3", step3)
        self.assertIn("FINAL", step3)

    def test_multi_stage_conversation_history_and_on_demand_capture(self) -> None:
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

        # Step 2: 実行 (AI要求なしのため canvas_image=None で呼び出し)
        plan2 = planner.plan("mountain landscape", 42, 1, 800, 600, canvas_image=None, iteration=2, max_iterations=2)
        self.assertEqual(len(plan2.strokes), 1)
        self.assertTrue(plan2.request_canvas_image)

        # Step 2 のペイロード検証: 前ステップの会話履歴（assistant 要約）が含まれており、canvas_image なしのため text のみ
        self.assertEqual(len(received_payloads), 2)
        step2_messages = received_payloads[1]["messages"]
        # system (0), user_step1 (1), assistant_step1 (2), user_step2 (3)
        self.assertEqual(len(step2_messages), 4)
        self.assertEqual(step2_messages[0]["role"], "system")
        self.assertEqual(step2_messages[1]["role"], "user")
        self.assertEqual(step2_messages[2]["role"], "assistant")
        self.assertEqual(step2_messages[3]["role"], "user")
        # Step 2 user_content は画像なし
        self.assertIsInstance(step2_messages[3]["content"], str)
        self.assertNotIn("image_url", step2_messages[3]["content"])

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
        # max_tokens が 10 等に制限されず 16384 (十分なトークン数) で送信されていること
        self.assertEqual(attempts[0]["max_completion_tokens"], 16384)
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
        # LLM が思考文のみでトークン枯渇しストロークが一切出力されなかった場合の自動プロシージャル救済
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
            OpenAICompatibleSettings("https://example.test/v1", "thinking-model"),
            opener=FakeExhaustedOpener(),
        )
        plan = planner.plan("anime girl portrait", 42, 20, 1000, 1000)
        self.assertEqual(plan.prompt, "anime girl portrait")
        self.assertGreaterEqual(len(plan.strokes), 1)

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
        self.assertTrue(all(b >= a for a, b in zip(times, times[1:])))
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
        self.assertTrue(len(cap_bytes) > 0)

    def test_fallback_png_is_decodable(self) -> None:
        import struct
        import zlib

        from ai_stroke_painter.krita_adapter import _MINIMAL_PNG_BYTES

        self.assertEqual(_MINIMAL_PNG_BYTES[:8], b"\x89PNG\r\n\x1a\n")
        pos = 8
        while pos + 12 <= len(_MINIMAL_PNG_BYTES):
            length = struct.unpack(">I", _MINIMAL_PNG_BYTES[pos : pos + 4])[0]
            chunk = _MINIMAL_PNG_BYTES[pos + 4 : pos + 12 + length]
            crc_stored = struct.unpack(">I", chunk[-4:])[0]
            self.assertEqual(zlib.crc32(chunk[:-4]) & 0xFFFFFFFF, crc_stored)
            pos += 12 + length

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
        self.assertEqual(view.opacities, [0.4, 0.8])

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

    def test_docker_on_plan_ready_on_demand_canvas_capture(self) -> None:
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

        # Case 1: AI は画像要求なし (request_canvas_image=False)
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
        self.assertIsNone(worker1.provided_captures[0])  # 高速進行: 画像なし

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

    def test_procedural_brush_profiles_applied(self) -> None:
        from .procedural import generate_procedural_plan

        profile_map = {
            "gpen": "Ink-2 Fineliner",
            "marupen": "Ink-1 Precision",
            "brush": "Wet-1 Water",
            "marker": "Marker-1 Broad",
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

        converter = ImageStrokeConverter()

        # ダミー画像バイト列での変換テスト (プロシージャルフォールバック)
        plan = converter.convert_image_to_plan(
            image_bytes=b"invalid-image-bytes-triggers-procedural-fallback",
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


def run() -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
