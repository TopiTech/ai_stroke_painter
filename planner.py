"""プロシージャルイラストエンジンと画像変換を統合した本格 Planner。"""

from __future__ import annotations

import math
from typing import Any

from .domain import DrawingPlan
from .image_converter import ImageStrokeConverter
from .ports import PlannerPort
from .procedural import generate_procedural_plan


def _valid_dimension(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 2:
        raise ValueError(f"{name} は 2 以上の有限数である必要があります")
    return float(value)


def validate_plan_request(
    prompt: Any,
    seed: Any,
    count: Any,
    width: Any,
    height: Any,
    *,
    auto_count: bool = False,
) -> tuple[str, int, int | None, float, float]:
    """Planner 実装で共通の入力契約を検証する。count=None は各実装の品質予算へ委ねる。"""
    if not isinstance(prompt, str):
        raise ValueError("prompt は文字列である必要があります")
    if len(prompt) > 20_000:
        raise ValueError("prompt は 20,000 文字以下である必要があります")
    if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
        raise ValueError("seed は 0 以上の整数である必要があります")
    valid_count: int | None = None
    if not auto_count and count is not None and count != 0 and count != "auto":
        if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= 500:
            raise ValueError("count は 1 から 500 の整数または None である必要があります")
        valid_count = count
    return prompt, seed, valid_count, _valid_dimension(width, "width"), _valid_dimension(height, "height")


def validate_iterations(iteration: Any, max_iterations: Any) -> tuple[int, int]:
    """反復番号が UI と Planner の共通契約内にあることを検証する。"""
    if isinstance(max_iterations, bool) or not isinstance(max_iterations, int) or not 1 <= max_iterations <= 10:
        raise ValueError("max_iterations は 1 から 10 の整数である必要があります")
    if isinstance(iteration, bool) or not isinstance(iteration, int) or not 1 <= iteration <= max_iterations:
        raise ValueError("iteration は 1 から max_iterations の整数である必要があります")
    return iteration, max_iterations


class RuleBasedPlanner(PlannerPort):
    """本格プロシージャル・イラストエンジンおよび画像解析を備えた決定論的 Planner。"""

    def __init__(self) -> None:
        self.image_converter = ImageStrokeConverter()

    def plan(
        self,
        prompt: str,
        seed: int,
        count: int | None = None,
        width: float = 1000.0,
        height: float = 1000.0,
        image_data: bytes | None = None,
        canvas_image: bytes | None = None,
        iteration: int = 1,
        max_iterations: int = 1,
        palette_name: str = "anime",
        brush_profile: str = "auto",
        edge_threshold: float = 0.18,
        shading_density: str = "medium",
        enable_flats: bool = True,
        color_mode: str = "original",
        auto_count: bool = False,
        **kwargs: Any,
    ) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height, auto_count=auto_count
        )
        iteration, max_iterations = validate_iterations(iteration, max_iterations)
        is_final = iteration >= max_iterations

        # 参照画像が渡されている場合は画像ストローク変換を実行
        if image_data:
            image_plan = self.image_converter.convert_image_to_plan(
                image_bytes=image_data,
                prompt=valid_prompt,
                seed=valid_seed,
                count=valid_count,
                target_width=valid_width,
                target_height=valid_height,
                edge_threshold=edge_threshold,
                shading_density=shading_density,
                enable_flats=enable_flats,
                color_mode=color_mode,
                palette_name=palette_name,
                brush_profile=brush_profile,
            )
            return DrawingPlan(
                prompt=image_plan.prompt,
                seed=image_plan.seed,
                strokes=image_plan.strokes,
                title=image_plan.title,
                iteration=iteration,
                layers=image_plan.layers,
                metadata={
                    **dict(image_plan.metadata),
                    "iteration": iteration,
                    "max_iterations": max_iterations,
                    "goal_reached": is_final,
                    "completion_score": 1.0 if is_final else float(iteration) / float(max_iterations),
                },
                canvas_width=valid_width,
                canvas_height=valid_height,
                goal_reached=is_final,
                completion_score=1.0 if is_final else float(iteration) / float(max_iterations),
            )

        # 自律反復改善（イテレーション）時のプロシージャル描画計画
        plan = generate_procedural_plan(
            prompt=valid_prompt,
            seed=valid_seed + (iteration - 1) * 1000,
            count=valid_count,
            width=valid_width,
            height=valid_height,
            palette_name=palette_name,
            brush_profile=brush_profile,
        )

        # イテレーション番号とゴール達成度をセット
        return DrawingPlan(
            prompt=plan.prompt,
            seed=plan.seed,
            strokes=plan.strokes,
            title=plan.title,
            iteration=iteration,
            layers=plan.layers,
            metadata={
                "iteration": iteration,
                "max_iterations": max_iterations,
                "palette": palette_name,
                "brush_profile": brush_profile,
                "goal_reached": is_final,
                "completion_score": 1.0 if is_final else float(iteration) / float(max_iterations),
            },
            canvas_width=valid_width,
            canvas_height=valid_height,
            goal_reached=is_final,
            completion_score=1.0 if is_final else float(iteration) / float(max_iterations),
        )
