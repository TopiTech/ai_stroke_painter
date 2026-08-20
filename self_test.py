"""Krita を起動せずに実行できる、MVP の回帰テスト。"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from .domain import DrawingPlan, PlanValidationError, Stroke, StrokePoint
from .krita_adapter import KritaCanvasAdapter
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
