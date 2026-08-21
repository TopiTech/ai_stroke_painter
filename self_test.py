"""Krita を起動せずに実行できる、MVP の回帰テスト。"""

from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import tempfile
from threading import Event, Thread
from typing import Any
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from . import build_plugin as build_plugin_module
from .build_plugin import PACKAGE_NAME, build
from .docker import PlanWorker
from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import (
    LLMPlannerError,
    OpenAICompatiblePlanner,
    OpenAICompatibleSettings,
    _extract_json_object,
)
from .planner import RuleBasedPlanner
from .storage import load_plan, save_plan


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

    def addChildNode(self, child: Any, _before: Any) -> None:
        self._children.append(child)

    def paintAbility(self) -> str:
        return self._paint_ability

    def paintLine(self, start: Any, end: Any, start_pressure: float, end_pressure: float) -> None:
        if self.paintAbility() == "PAINT":
            self.lines.append((start, end, start_pressure, end_pressure))


class _FakeDocument:
    def __init__(self, active: Any | None = None, root: Any | None = None) -> None:
        self.active = active
        self.root = root or _FakeNode("root", "grouplayer")
        self.created = 0
        self.refreshed = 0

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


class PlannerAndStorageTests(unittest.TestCase):
    def test_planner_is_deterministic_and_bounded(self) -> None:
        planner = RuleBasedPlanner()
        first = planner.plan("髪のS字", 42, 10, 1024, 768)
        second = planner.plan("髪のS字", 42, 10, 1024, 768)
        self.assertEqual(first.as_dict(), second.as_dict())
        self.assertEqual(len(first.strokes), 10)
        for stroke in first.strokes:
            self.assertGreaterEqual(len(stroke.points), 20)
            for point in stroke.points:
                self.assertGreaterEqual(point.x, 0)
                self.assertLess(point.x, 1024)
                self.assertGreaterEqual(point.y, 0)
                self.assertLess(point.y, 768)
                self.assertGreaterEqual(point.pressure, 0.0)
                self.assertLessEqual(point.pressure, 1.0)

    def test_plan_json_round_trip_and_collision_free_save(self) -> None:
        plan = RuleBasedPlanner().plan("curve", 9, 2, 300, 200)
        with tempfile.TemporaryDirectory() as temp:
            first_path = save_plan(plan, temp)
            second_path = save_plan(plan, temp)
            self.assertNotEqual(first_path, second_path)
            self.assertEqual(load_plan(first_path), plan)
            self.assertEqual(load_plan(second_path), plan)

    def test_invalid_domain_data_is_rejected(self) -> None:
        with self.assertRaises(PlanValidationError):
            StrokePoint(0, 0, 1.1, 0)
        with self.assertRaises(PlanValidationError):
            Stroke("one-point", [StrokePoint(0, 0, 0.5, 0)])
        with self.assertRaises(PlanValidationError):
            DrawingPlan.from_dict({"schema_version": 99, "prompt": "", "seed": 0, "strokes": []})


class PluginBuildTests(unittest.TestCase):
    @staticmethod
    def _create_minimal_source(source: Path) -> None:
        source.mkdir(parents=True)
        (source / f"{PACKAGE_NAME}.desktop").write_text("[Desktop Entry]\n", encoding="utf-8")
        (source / "__init__.py").write_text("", encoding="utf-8")

    def test_build_includes_manifest_once_at_archive_root(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / f"{PACKAGE_NAME}.zip"
            self.assertEqual(build(output), output)
            with ZipFile(output) as archive:
                names = archive.namelist()

        self.assertIn(f"{PACKAGE_NAME}.desktop", names)
        self.assertIn(f"{PACKAGE_NAME}/", names)
        self.assertIn(f"{PACKAGE_NAME}/__init__.py", names)
        self.assertNotIn(f"{PACKAGE_NAME}/{PACKAGE_NAME}.desktop", names)

    def test_build_excludes_output_inside_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source"
            self._create_minimal_source(source)
            output = source / "custom.zip"
            with patch.object(build_plugin_module, "__file__", str(source / "build_plugin.py")):
                build(output)
            with ZipFile(output) as archive:
                names = archive.namelist()

        self.assertNotIn(f"{PACKAGE_NAME}/custom.zip", names)

    def test_build_ignores_excluded_names_above_source(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "dist" / "source"
            self._create_minimal_source(source)
            output = Path(temp) / "plugin.zip"
            with patch.object(build_plugin_module, "__file__", str(source / "build_plugin.py")):
                build(output)
            with ZipFile(output) as archive:
                names = archive.namelist()

        self.assertIn(f"{PACKAGE_NAME}/__init__.py", names)

    def test_build_excludes_unlisted_local_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "source"
            self._create_minimal_source(source)
            (source / "local-not-for-plugin.txt").write_text("dummy", encoding="utf-8")
            output = Path(temp) / "plugin.zip"
            with patch.object(build_plugin_module, "__file__", str(source / "build_plugin.py")):
                build(output)
            with ZipFile(output) as archive:
                names = archive.namelist()

        self.assertNotIn(f"{PACKAGE_NAME}/local-not-for-plugin.txt", names)


class OpenAICompatiblePlannerTests(unittest.TestCase):
    def test_calls_chat_completions_and_validates_plan(self) -> None:
        expected_plan = {
            "schema_version": 1,
            "prompt": "a blue curve",
            "seed": 12,
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
        self.assertEqual(
            Handler.received["body"]["messages"][1]["content"],
            '{"prompt": "a blue curve", "seed": 12, "stroke_count": 1, "canvas": {"width": 100.0, "height": 100.0}}',
        )

    def test_rejects_out_of_bounds_llm_plan(self) -> None:
        response = {
            "choices": [
                {
                    "message": {
                        "content": json.dumps(
                            {
                                "schema_version": 1,
                                "prompt": "curve",
                                "seed": 1,
                                "strokes": [
                                    {
                                        "id": "s",
                                        "points": [
                                            {"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0},
                                            {"x": 200, "y": 0, "pressure": 0.5, "time_ms": 1},
                                        ],
                                    }
                                ],
                            }
                        )
                    }
                }
            ]
        }

        class Response:
            def read(self, _size: int) -> bytes:
                return json.dumps(response).encode("utf-8")

            def __enter__(self) -> Response:
                return self

            def __exit__(self, *_args: Any) -> None:
                pass

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"),
            opener=lambda *_args, **_kwargs: Response(),
        )
        with self.assertRaisesRegex(RuntimeError, "キャンバス範囲外"):
            planner.plan("curve", 1, 1, 100, 100)

    def test_rejects_cross_origin_redirect_before_forwarding_credentials(self) -> None:
        expected_plan = {
            "schema_version": 1,
            "prompt": "curve",
            "seed": 1,
            "strokes": [
                {
                    "id": "redirected-stroke",
                    "points": [
                        {"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0},
                        {"x": 1, "y": 1, "pressure": 0.5, "time_ms": 1},
                    ],
                }
            ],
        }

        class TargetHandler(BaseHTTPRequestHandler):
            reached = False

            def do_GET(self) -> None:
                TargetHandler.reached = True
                response = {"choices": [{"message": {"content": json.dumps(expected_plan)}}]}
                encoded = json.dumps(response).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def log_message(self, _format: str, *_args: Any) -> None:
                pass

        class RedirectHandler(BaseHTTPRequestHandler):
            target_port: int | None = None

            def do_POST(self) -> None:
                self.send_response(302)
                self.send_header("Location", f"http://127.0.0.1:{self.target_port}/result")
                self.end_headers()

            def log_message(self, _format: str, *_args: Any) -> None:
                pass

        target_server = ThreadingHTTPServer(("127.0.0.1", 0), TargetHandler)
        RedirectHandler.target_port = target_server.server_port
        redirect_server = ThreadingHTTPServer(("127.0.0.1", 0), RedirectHandler)
        ready_events = [Event(), Event()]

        def serve(server: ThreadingHTTPServer, ready: Event) -> None:
            ready.set()
            server.serve_forever()

        threads = [
            Thread(target=serve, args=(server, ready), daemon=True)
            for server, ready in zip((target_server, redirect_server), ready_events)
        ]
        for thread in threads:
            thread.start()
        for ready in ready_events:
            self.assertTrue(ready.wait(2))
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings(
                    f"http://127.0.0.1:{redirect_server.server_port}/v1",
                    "model",
                    "dummy-key",
                    2,
                )
            )
            with self.assertRaisesRegex(LLMPlannerError, "別オリジン"):
                planner.plan("curve", 1, 1, 100, 100)
        finally:
            for server in (redirect_server, target_server):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()

        self.assertFalse(TargetHandler.reached)

    def test_extracts_json_with_surrounding_markdown_and_commentary(self) -> None:
        raw_text = (
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

    def test_extracts_json_without_code_fence_but_with_text(self) -> None:
        raw_text = (
            "Sure! Plan: "
            '{"schema_version": 1, "prompt": "line", "seed": 1, "strokes": [{"id": "s", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 1, "y": 1, "pressure": 0.5, "time_ms": 1}]}]} '
            "Enjoy painting."
        )
        parsed = _extract_json_object(raw_text)
        plan = DrawingPlan.from_dict(parsed)
        self.assertEqual(plan.prompt, "line")
        self.assertEqual(len(plan.strokes), 1)


class CanvasAdapterTests(unittest.TestCase):
    def test_existing_target_layer_is_reused_and_pressure_is_unit_range(self) -> None:
        target = _FakeNode(KritaCanvasAdapter.LAYER_NAME)
        document = _FakeDocument(active=_FakeNode("other"), root=_FakeNode("root", "grouplayer"))
        document.root.addChildNode(target, None)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()

        import ai_stroke_painter.krita_adapter as module

        original_qpoint = module._qpoint
        module._qpoint = lambda x, y: (x, y)
        try:
            self.assertEqual(adapter.render(document, plan), 1)
        finally:
            module._qpoint = original_qpoint

        self.assertEqual(document.created, 0)
        self.assertIs(document.active, target)
        self.assertEqual(document.refreshed, 1)
        self.assertTrue(target.lines)
        self.assertTrue(all(0.0 <= line[2] <= 1.0 and 0.0 <= line[3] <= 1.0 for line in target.lines))

    def test_cancel_before_drawing_does_not_paint(self) -> None:
        document = _FakeDocument()
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()
        self.assertEqual(adapter.render(document, plan, cancelled=lambda: True), 0)
        assert document.active is not None
        self.assertEqual(document.active.lines, [])
        self.assertEqual(document.refreshed, 1)

    def test_unpaintable_target_is_rejected(self) -> None:
        target = _FakeNode(KritaCanvasAdapter.LAYER_NAME, paint_ability="UNPAINTABLE")
        document = _FakeDocument(active=target)
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()

        with (
            patch("ai_stroke_painter.krita_adapter._qpoint", lambda x, y: (x, y)),
            self.assertRaisesRegex(RuntimeError, "描画できません"),
        ):
            adapter.render(document, plan)

        self.assertEqual(target.lines, [])

    def test_render_does_not_pass_keyword_arguments_to_zip(self) -> None:
        """Python 3.9 では zip() がキーワード引数を受け付けないため位置引数のみで呼ぶ必要がある。"""
        import builtins

        original_zip = builtins.zip

        def strict_rejecting_zip(*args: Any, **kwargs: Any) -> Any:
            if kwargs:
                raise TypeError("zip() takes no keyword arguments")
            return original_zip(*args)

        document = _FakeDocument()
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()

        with (
            patch("builtins.zip", side_effect=strict_rejecting_zip),
            patch("ai_stroke_painter.krita_adapter._qpoint", lambda x, y: (x, y)),
        ):
            rendered = adapter.render(document, plan)

        self.assertEqual(rendered, 1)

    def test_qt_cached_resolver_and_process_events(self) -> None:
        import ai_stroke_painter.krita_adapter as module

        # _resolve_qt と _process_events が例外なく実行できること
        qpoint, qapp = module._resolve_qt()
        self.assertTrue(qpoint is None or callable(qpoint))
        self.assertTrue(qapp is None or callable(qapp))
        module._process_events()


class WorkerAndDockerTests(unittest.TestCase):
    def test_plan_worker_emits_plan_on_success(self) -> None:
        planner = RuleBasedPlanner()
        worker = PlanWorker(
            planner=planner,
            prompt="curve",
            seed=1,
            count=2,
            width=200,
            height=200,
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
            prompt="curve",
            seed=1,
            count=2,
            width=200,
            height=200,
        )
        received_plans: list[Any] = []
        worker.plan_ready.connect(lambda p: received_plans.append(p))

        worker.cancel()
        self.assertTrue(worker.is_cancelled())
        worker.run()
        self.assertEqual(len(received_plans), 0)


def run() -> bool:
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
