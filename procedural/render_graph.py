"""SceneSpec を、順序付きの意味描画ノードへ展開する契約と合成器。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Literal

from ..domain import Stroke
from ..scene_spec import SceneDomain, SceneSpec
from .semantic_budget import SemanticStrokeGroup, allocate_semantic_groups

RenderRole = Literal["background", "subject", "motif", "effect"]


@dataclass(frozen=True)
class RenderNode:
    """独立して縮約できる、1つの視覚的責務を持つ描画ノード。"""

    id: str
    domain: SceneDomain
    role: RenderRole
    strokes: tuple[Stroke, ...]
    z_index: int
    priority: int
    minimum_count: int
    bounds: tuple[float, float, float, float] = (0.0, 0.0, 1.0, 1.0)
    required_for: tuple[str, ...] = ()
    atomic: bool = False
    selection_strategy: Literal["even", "layer_priority"] = "layer_priority"
    featured_ids: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.id, str) or not self.id.strip():
            raise ValueError("RenderNode.id は空でない文字列である必要があります")
        if self.domain not in {"character", "creature", "landscape", "geometry", "fx", "unknown"}:
            raise ValueError(f"未対応の RenderNode.domain です: {self.domain}")
        if self.role not in {"background", "subject", "motif", "effect"}:
            raise ValueError(f"未対応の RenderNode.role です: {self.role}")
        if not self.strokes:
            raise ValueError("RenderNode.strokes は1本以上必要です")
        if len({stroke.id for stroke in self.strokes}) != len(self.strokes):
            raise ValueError("RenderNode 内で stroke.id が重複しています")
        if isinstance(self.z_index, bool) or not isinstance(self.z_index, int):
            raise ValueError("RenderNode.z_index は整数である必要があります")
        if isinstance(self.priority, bool) or not isinstance(self.priority, int) or not 0 <= self.priority <= 100:
            raise ValueError("RenderNode.priority は0から100の整数である必要があります")
        if (
            isinstance(self.minimum_count, bool)
            or not isinstance(self.minimum_count, int)
            or not 0 <= self.minimum_count <= len(self.strokes)
        ):
            raise ValueError("RenderNode.minimum_count は0からノード本数までの整数である必要があります")
        if any(not isinstance(element, str) or not element for element in self.required_for):
            raise ValueError("RenderNode.required_for は空でない文字列のタプルである必要があります")
        if len(self.bounds) != 4 or any(
            isinstance(value, bool) or not isinstance(value, (int, float)) for value in self.bounds
        ):
            raise ValueError("RenderNode.bounds は4つの数値である必要があります")
        x0, y0, x1, y1 = self.bounds
        if not (0.0 <= x0 < x1 <= 1.0 and 0.0 <= y0 < y1 <= 1.0):
            raise ValueError("RenderNode.bounds は0から1の範囲で正の面積を持つ必要があります")
        if not isinstance(self.atomic, bool):
            raise ValueError("RenderNode.atomic は真偽値である必要があります")
        if self.selection_strategy not in {"even", "layer_priority"}:
            raise ValueError("RenderNode.selection_strategy は even または layer_priority である必要があります")
        if len(set(self.featured_ids)) != len(self.featured_ids):
            raise ValueError("RenderNode.featured_ids は重複できません")
        stroke_ids = {stroke.id for stroke in self.strokes}
        if any(stroke_id not in stroke_ids for stroke_id in self.featured_ids):
            raise ValueError("RenderNode.featured_ids はノード内の stroke.id である必要があります")


@dataclass(frozen=True)
class RenderGraph:
    scene_spec: SceneSpec
    nodes: tuple[RenderNode, ...]

    def __post_init__(self) -> None:
        if not isinstance(self.scene_spec, SceneSpec):
            raise TypeError("RenderGraph.scene_spec は SceneSpec である必要があります")
        if not self.nodes:
            raise ValueError("RenderGraph.nodes は1件以上必要です")
        if len({node.id for node in self.nodes}) != len(self.nodes):
            raise ValueError("RenderGraph の node.id が重複しています")
        stroke_ids = [stroke.id for node in self.nodes for stroke in node.strokes]
        if len(set(stroke_ids)) != len(stroke_ids):
            raise ValueError("複数の RenderNode に同じ stroke.id を指定できません")


@dataclass(frozen=True)
class ComposedScene:
    strokes: tuple[Stroke, ...]
    rendered_elements: tuple[str, ...]
    missing_elements: tuple[str, ...]
    semantic_fidelity: float
    manifest: tuple[dict[str, Any], ...]
    stroke_node_map: dict[str, str]


def compose_render_graph(graph: RenderGraph, target_count: int | None) -> ComposedScene:
    """Z順と意味グループの最低品質を保ち、描画グラフをストロークへ縮約する。"""
    if target_count is not None and (
        isinstance(target_count, bool) or not isinstance(target_count, int) or target_count < 0
    ):
        raise ValueError("target_count は0以上の整数または None である必要があります")

    ordered_nodes = tuple(sorted(graph.nodes, key=lambda node: (node.z_index, node.id)))
    groups = tuple(
        SemanticStrokeGroup(
            id=node.id,
            strokes=node.strokes,
            role=node.role,
            priority=node.priority,
            minimum_count=node.minimum_count,
            atomic=node.atomic,
            selection_strategy=node.selection_strategy,
            featured_ids=node.featured_ids,
        )
        for node in ordered_nodes
    )
    selected = allocate_semantic_groups(groups, target_count)
    selected_ids = {stroke.id for stroke in selected}

    manifest: list[dict[str, Any]] = []
    requirement_states: dict[str, list[bool]] = {}
    for node in ordered_nodes:
        selected_count = sum(stroke.id in selected_ids for stroke in node.strokes)
        completion_threshold = len(node.strokes) if node.atomic else node.minimum_count
        complete = selected_count >= completion_threshold
        for element in node.required_for:
            requirement_states.setdefault(element, []).append(complete)
        manifest.append(
            {
                "node_id": node.id,
                "domain": node.domain,
                "role": node.role,
                "z_index": node.z_index,
                "priority": node.priority,
                "available_count": len(node.strokes),
                "selected_count": selected_count,
                "minimum_count": completion_threshold,
                "atomic": node.atomic,
                "complete": complete,
                "bounds": list(node.bounds),
                "featured_count": sum(stroke_id in selected_ids for stroke_id in node.featured_ids),
                "featured_available": len(node.featured_ids),
                "required_for": list(node.required_for),
            }
        )

    required = tuple(dict.fromkeys(graph.scene_spec.required_elements))
    rendered = tuple(
        element for element in required if element in requirement_states and all(requirement_states[element])
    )
    missing = tuple(element for element in required if element not in rendered)
    fidelity = len(rendered) / len(required) if required else 1.0
    stroke_node_map = {
        stroke.id: node.id for node in ordered_nodes for stroke in node.strokes if stroke.id in selected_ids
    }
    return ComposedScene(
        strokes=tuple(selected),
        rendered_elements=rendered,
        missing_elements=missing,
        semantic_fidelity=fidelity,
        manifest=tuple(manifest),
        stroke_node_map=stroke_node_map,
    )
