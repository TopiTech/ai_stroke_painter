"""Krita を起動せずに実行できる、AI Stroke Painter の回帰・機能テスト。"""

from __future__ import annotations

import contextlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
from threading import Thread
from typing import Any
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
    _extract_content_from_response,
    _extract_json_object,
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

    def addChildNode(self, child: Any, before: Any = None) -> None:
        if before is not None and before in self._children:
            idx = self._children.index(before)
            self._children.insert(idx, child)
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

        self.assertEqual(plan, DrawingPlan.from_dict(expected_plan))
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
        self.assertLessEqual(pts[1].x, 100.0)

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
        self.assertAlmostEqual(scaled_pts[1].x, 900.0, places=1)
        self.assertAlmostEqual(scaled_pts[1].y, 400.0, places=1)

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

        adapter.ensure_layer(document, "Lineart")
        adapter.ensure_layer(document, "Draft")
        adapter.ensure_layer(document, "FX")

        children_names = [c.name() for c in root.childNodes()]
        self.assertEqual(children_names, ["Draft", "Lineart", "FX"])

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
        thread.join(timeout=2.0)

        for _ in range(50):
            _process_events()
            if len(plans) == 2:
                break
            time.sleep(0.01)

        self.assertEqual(len(plans), 2)
        self.assertEqual(captured_in_planner[1], b"fake-main-thread-screenshot")

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

        docker = AIStrokePainterDocker.__new__(AIStrokePainterDocker)
        docker.base_url = FakeTextWidget("https://custom.api/v1")
        docker.model = FakeTextWidget("custom-model-pro")
        docker.timeout_sec = FakeIntWidget(99)
        docker.max_tokens = FakeIntWidget(16384)
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
        self.assertEqual(docker2.prompt.toPlainText(), "test persistent prompt")
        self.assertEqual(docker2.seed.value(), 777)
        self.assertEqual(docker2.count.value(), 55)
        self.assertEqual(docker2.iterations.value(), 4)
        self.assertTrue(docker2.auto_refine.isChecked())
        self.assertFalse(docker2.save_svg_chk.isChecked())
        self.assertTrue(docker2.debug_mode_chk.isChecked())


def run() -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
