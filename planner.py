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
) -> tuple[str, int, int, float, float]:
    """Planner 実装で共通の入力契約を検証して正規化する。"""
    if not isinstance(prompt, str):
        raise ValueError("prompt は文字列である必要があります")
    if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
        raise ValueError("seed は 0 以上の整数である必要があります")
    if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= 500:
        raise ValueError("count は 1 から 500 の整数である必要があります")
    return prompt, seed, count, _valid_dimension(width, "width"), _valid_dimension(height, "height")


class RuleBasedPlanner(PlannerPort):
    """本格プロシージャル・イラストエンジンおよび画像解析を備えた決定論的 Planner。"""

    def __init__(self) -> None:
        self.image_converter = ImageStrokeConverter()

    def plan(
        self,
        prompt: str,
        seed: int,
        count: int,
        width: float,
        height: float,
        image_data: bytes | None = None,
        canvas_image: bytes | None = None,
        iteration: int = 1,
        max_iterations: int = 1,
        palette_name: str = "anime",
    ) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height
        )

        # 参照画像が渡されている場合は画像ストローク変換を実行
        if image_data:
            return self.image_converter.convert_image_to_plan(
                image_bytes=image_data,
                prompt=valid_prompt,
                seed=valid_seed,
                count=valid_count,
                target_width=valid_width,
                target_height=valid_height,
            )

        # 自律反復改善（イテレーション）時のプロシージャル描画計画
        plan = generate_procedural_plan(
            prompt=valid_prompt,
            seed=valid_seed + (iteration - 1) * 1000,
            count=valid_count,
            width=valid_width,
            height=valid_height,
            palette_name=palette_name,
        )

        # イテレーション番号をセット
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
            },
        )
