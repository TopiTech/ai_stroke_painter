"""意味のまとまりを壊さずに描画本数を削減する予算配分。"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from ..domain import Stroke


@dataclass(frozen=True)
class SemanticStrokeGroup:
    """同じ視覚的役割を担うストローク群。

    ``atomic`` なグループは全本数を採用できる場合だけ残す。通常グループは
    ``minimum_count`` を先に確保し、残りを優先度順のラウンドロビンで配る。
    """

    id: str
    strokes: tuple[Stroke, ...]
    role: str
    priority: int = 50
    minimum_count: int = 1
    atomic: bool = False
    selection_strategy: Literal["even", "layer_priority"] = "even"
    featured_ids: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.id, str) or not self.id.strip():
            raise ValueError("SemanticStrokeGroup.id は空でない文字列である必要があります")
        if not isinstance(self.role, str) or not self.role.strip():
            raise ValueError("SemanticStrokeGroup.role は空でない文字列である必要があります")
        if not self.strokes:
            raise ValueError("SemanticStrokeGroup.strokes は1本以上必要です")
        if len({stroke.id for stroke in self.strokes}) != len(self.strokes):
            raise ValueError("SemanticStrokeGroup 内で stroke.id が重複しています")
        if isinstance(self.priority, bool) or not isinstance(self.priority, int) or not 0 <= self.priority <= 100:
            raise ValueError("SemanticStrokeGroup.priority は0から100の整数である必要があります")
        if (
            isinstance(self.minimum_count, bool)
            or not isinstance(self.minimum_count, int)
            or not 0 <= self.minimum_count <= len(self.strokes)
        ):
            raise ValueError("minimum_count は0からグループ本数までの整数である必要があります")
        if not isinstance(self.atomic, bool):
            raise ValueError("SemanticStrokeGroup.atomic は真偽値である必要があります")
        if self.selection_strategy not in {"even", "layer_priority"}:
            raise ValueError("selection_strategy は even または layer_priority である必要があります")
        if len(set(self.featured_ids)) != len(self.featured_ids):
            raise ValueError("featured_ids は重複できません")
        stroke_ids = {stroke.id for stroke in self.strokes}
        if any(stroke_id not in stroke_ids for stroke_id in self.featured_ids):
            raise ValueError("featured_ids はグループ内の stroke.id である必要があります")


def _evenly_spaced(strokes: tuple[Stroke, ...], count: int) -> list[Stroke]:
    if count <= 0:
        return []
    if count >= len(strokes):
        return list(strokes)
    # 先頭だけへ偏らず、放射線やハッチの画面分布を維持する。
    return [strokes[(index * len(strokes)) // count] for index in range(count)]


def _select_group_strokes(group: SemanticStrokeGroup, count: int) -> list[Stroke]:
    if count <= 0:
        return []
    by_id = {stroke.id: stroke for stroke in group.strokes}
    featured = [by_id[stroke_id] for stroke_id in group.featured_ids[:count]]
    if len(featured) >= count:
        return featured
    featured_ids = {stroke.id for stroke in featured}
    remaining = tuple(stroke for stroke in group.strokes if stroke.id not in featured_ids)
    remaining_count = count - len(featured)
    if group.selection_strategy == "layer_priority" and not group.atomic:
        # import を遅延し、基礎幾何モジュールと契約モジュールの依存を一方向に保つ。
        from .base import sample_strokes_by_priority

        selected = sample_strokes_by_priority(list(remaining), remaining_count)
    else:
        selected = _evenly_spaced(remaining, remaining_count)
    return [*featured, *selected]


def allocate_semantic_groups(groups: tuple[SemanticStrokeGroup, ...], target_count: int | None) -> list[Stroke]:
    """意味グループの最低表現と不可分性を守って指定本数へ縮約する。"""
    if not groups:
        return []
    all_ids = [stroke.id for group in groups for stroke in group.strokes]
    if len(set(all_ids)) != len(all_ids):
        raise ValueError("複数の SemanticStrokeGroup に同じ stroke.id を指定できません")
    if target_count is not None and (isinstance(target_count, bool) or not isinstance(target_count, int)):
        raise TypeError("target_count は整数または None である必要があります")
    if target_count is not None and target_count <= 0:
        return []

    total_count = len(all_ids)
    if target_count is None or target_count >= total_count:
        return [stroke for group in groups for stroke in group.strokes]

    budget = target_count
    allocations = [0] * len(groups)
    priority_order = sorted(range(len(groups)), key=lambda index: (-groups[index].priority, index))

    # 不可分シンボルと最低表現を最優先で確保する。
    for index in priority_order:
        group = groups[index]
        requested = len(group.strokes) if group.atomic else group.minimum_count
        if group.atomic and requested <= budget:
            allocations[index] = requested
            budget -= requested
        elif not group.atomic and budget > 0:
            allocated = min(requested, budget)
            allocations[index] = allocated
            budget -= allocated

    # 最低表現を確保した後は、1本ずつ配って低優先グループも画面から消えにくくする。
    while budget > 0:
        changed = False
        for index in priority_order:
            group = groups[index]
            if group.atomic or allocations[index] >= len(group.strokes):
                continue
            allocations[index] += 1
            budget -= 1
            changed = True
            if budget <= 0:
                break
        if not changed:
            break

    selected: list[Stroke] = []
    for group, count in zip(groups, allocations, strict=True):
        selected.extend(_select_group_strokes(group, count))
    return selected
