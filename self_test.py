"""Krita を起動せずに実行できる、MVP の回帰テスト。"""

from __future__ import annotations

import tempfile
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from threading import Thread
import unittest
from pathlib import Path

from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint
from .krita_adapter import KritaCanvasAdapter
from .llm_planner import OpenAICompatiblePlanner, OpenAICompatibleSettings
from .planner import RuleBasedPlanner
from .storage import load_plan, save_plan


class _FakeNode:
    def __init__(self, name, node_type="paintlayer"):
        self._name = name
        self._type = node_type
        self._children = []
        self.lines = []

    def name(self):
        return self._name

    def type(self):
        return self._type

    def childNodes(self):
        return list(self._children)

    def addChildNode(self, child, _before):
        self._children.append(child)

    def paintLine(self, start, end, start_pressure, end_pressure):
        self.lines.append((start, end, start_pressure, end_pressure))


class _FakeDocument:
    def __init__(self, active=None, root=None):
        self.active = active
        self.root = root or _FakeNode("root", "grouplayer")
        self.created = 0
        self.refreshed = 0

    def activeNode(self):
        return self.active

    def rootNode(self):
        return self.root

    def createNode(self, name, node_type):
        self.created += 1
        return _FakeNode(name, node_type)

    def setActiveNode(self, node):
        self.active = node

    def refreshProjection(self):
        self.refreshed += 1


class PlannerAndStorageTests(unittest.TestCase):
    def test_planner_is_deterministic_and_bounded(self):
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

    def test_plan_json_round_trip_and_collision_free_save(self):
        plan = RuleBasedPlanner().plan("curve", 9, 2, 300, 200)
        with tempfile.TemporaryDirectory() as temp:
            first_path = save_plan(plan, temp)
            second_path = save_plan(plan, temp)
            self.assertNotEqual(first_path, second_path)
            self.assertEqual(load_plan(first_path), plan)
            self.assertEqual(load_plan(second_path), plan)

    def test_invalid_domain_data_is_rejected(self):
        with self.assertRaises(PlanValidationError):
            StrokePoint(0, 0, 1.1, 0)
        with self.assertRaises(PlanValidationError):
            Stroke("one-point", [StrokePoint(0, 0, 0.5, 0)])
        with self.assertRaises(PlanValidationError):
            DrawingPlan.from_dict({"schema_version": 99, "prompt": "", "seed": 0, "strokes": []})


class OpenAICompatiblePlannerTests(unittest.TestCase):
    def test_calls_chat_completions_and_validates_plan(self):
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
            received = None

            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                type(self).received = {"path": self.path, "authorization": self.headers.get("Authorization"), "body": json.loads(body)}
                response = {"choices": [{"message": {"content": "```json\n" + json.dumps(expected_plan) + "\n```"}}]}
                encoded = json.dumps(response).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(encoded)))
                self.end_headers()
                self.wfile.write(encoded)

            def log_message(self, _format, *_args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            planner = OpenAICompatiblePlanner(
                OpenAICompatibleSettings("http://127.0.0.1:%d/v1" % server.server_port, "test-model", "test-key", 2)
            )
            plan = planner.plan("a blue curve", 12, 1, 100, 100)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()

        self.assertEqual(plan, DrawingPlan.from_dict(expected_plan))
        self.assertEqual(Handler.received["path"], "/v1/chat/completions")
        self.assertEqual(Handler.received["authorization"], "Bearer test-key")
        self.assertEqual(Handler.received["body"]["model"], "test-model")
        self.assertEqual(Handler.received["body"]["messages"][1]["content"], '{"prompt": "a blue curve", "seed": 12, "stroke_count": 1, "canvas": {"width": 100.0, "height": 100.0}}')

    def test_rejects_out_of_bounds_llm_plan(self):
        response = {
            "choices": [
                {"message": {"content": json.dumps({"schema_version": 1, "prompt": "curve", "seed": 1, "strokes": [{"id": "s", "points": [{"x": 0, "y": 0, "pressure": 0.5, "time_ms": 0}, {"x": 200, "y": 0, "pressure": 0.5, "time_ms": 1}]}]})}}
            ]
        }

        class Response:
            def read(self, _size):
                return json.dumps(response).encode("utf-8")

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

        planner = OpenAICompatiblePlanner(
            OpenAICompatibleSettings("https://example.test/v1", "model"), opener=lambda *_args, **_kwargs: Response()
        )
        with self.assertRaisesRegex(RuntimeError, "キャンバス範囲外"):
            planner.plan("curve", 1, 1, 100, 100)


class CanvasAdapterTests(unittest.TestCase):
    def test_existing_target_layer_is_reused_and_pressure_is_unit_range(self):
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

    def test_cancel_before_drawing_does_not_paint(self):
        document = _FakeDocument()
        plan = RuleBasedPlanner().plan("curve", 3, 1, 100, 100)
        adapter = KritaCanvasAdapter()
        self.assertEqual(adapter.render(document, plan, cancelled=lambda: True), 0)
        self.assertEqual(document.active.lines, [])
        self.assertEqual(document.refreshed, 1)


def run():
    suite = unittest.defaultTestLoader.loadTestsFromModule(__import__(__name__, fromlist=["*"]))
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(0 if run() else 1)
