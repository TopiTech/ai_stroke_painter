"""プロシージャルイラストエンジンと画像変換を統合した本格 Planner。"""

from __future__ import annotations

from collections.abc import Callable
import hashlib
import math
from typing import Any, cast

from .domain import MAX_PLAN_STROKES, DrawingPlan
from .image_converter import ImageStrokeConverter
from .ports import PlannerPort
from .procedural import generate_procedural_plan

MAX_CANVAS_DIMENSION = 16384
MAX_IMAGE_DATA_BYTES = 25 * 1024 * 1024


def _valid_dimension(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 2:
        raise ValueError(f"{name} は 2 以上の有限数である必要があります")
    dimension = float(value)
    if dimension > MAX_CANVAS_DIMENSION:
        raise ValueError(f"{name} は {MAX_CANVAS_DIMENSION} 以下である必要があります")
    return dimension


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
    if not prompt.strip():
        raise ValueError("prompt は空でない文字列である必要があります")
    if len(prompt) > 20_000:
        raise ValueError("prompt は 20,000 文字以下である必要があります")
    if isinstance(seed, bool) or not isinstance(seed, int) or seed < 0:
        raise ValueError("seed は 0 以上の整数である必要があります")
    valid_count: int | None = None
    if not auto_count and count is not None and count != 0 and count != "auto":
        if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_PLAN_STROKES:
            raise ValueError(f"count は 1 から {MAX_PLAN_STROKES} の整数または None である必要があります")
        valid_count = count
    elif auto_count and count is not None and count not in (0, "auto"):
        if isinstance(count, bool) or not isinstance(count, int) or not 1 <= count <= MAX_PLAN_STROKES:
            raise ValueError(f"count は 1 から {MAX_PLAN_STROKES} の整数または None である必要があります")
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

        if image_data is not None and len(image_data) > MAX_IMAGE_DATA_BYTES:
            raise ValueError(f"参照画像が上限 ({MAX_IMAGE_DATA_BYTES // (1024 * 1024)}MB) を超えています")

        # 参照画像が渡されている場合は画像ストローク変換を実行
        if image_data is not None and len(image_data) > 0:
            generation_seed = (valid_seed + (iteration - 1) * 1000) & 0x7FFFFFFF
            image_plan = self.image_converter.convert_image_to_plan(
                image_bytes=image_data,
                prompt=valid_prompt,
                seed=generation_seed,
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
                seed=valid_seed,
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
        generation_seed = (valid_seed + (iteration - 1) * 1000) & 0x7FFFFFFF
        plan = generate_procedural_plan(
            prompt=valid_prompt,
            seed=generation_seed,
            count=valid_count,
            width=valid_width,
            height=valid_height,
            palette_name=palette_name,
            brush_profile=brush_profile,
        )

        # イテレーション番号とゴール達成度をセット
        return DrawingPlan(
            prompt=plan.prompt,
            # 反復間で累積計画へ統合できるよう、公開シードは要求値を維持する。
            # 反復ごとの多様性は上の generation_seed で内部生成へだけ適用する。
            seed=valid_seed,
            strokes=plan.strokes,
            title=plan.title,
            iteration=iteration,
            layers=plan.layers,
            metadata={
                **dict(plan.metadata),
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


class ImageGenerationPlanner(PlannerPort):
    """Text-to-Image 画像生成 AI と ImageStrokeConverter を連携させた高品質イラスト Planner。"""

    def __init__(
        self,
        settings: Any | None = None,
        log_callback: Any | None = None,
    ) -> None:
        from .image_generator import ImageGeneratorClient, ImageGeneratorSettings

        self.settings = settings or ImageGeneratorSettings()
        self.log_callback = log_callback
        self.image_client = ImageGeneratorClient(self.settings, log_callback=self._log)
        self.image_converter = ImageStrokeConverter()
        self._cached_image_data: bytes | None = None
        self._last_prompt: str = ""

    def _log(self, message: str) -> None:
        if self.log_callback:
            self.log_callback(message)

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
        cancelled: Any | None = None,
        **kwargs: Any,
    ) -> DrawingPlan:
        valid_prompt, valid_seed, valid_count, valid_width, valid_height = validate_plan_request(
            prompt, seed, count, width, height, auto_count=auto_count
        )
        iteration, max_iterations = validate_iterations(iteration, max_iterations)
        is_final = iteration >= max_iterations

        cancel_fn: Callable[[], bool] | None = cast(Callable[[], bool], cancelled) if callable(cancelled) else None
        target_aspect = valid_width / max(1.0, valid_height)

        if image_data is not None and len(image_data) > MAX_IMAGE_DATA_BYTES:
            raise ValueError(f"参照画像が上限 ({MAX_IMAGE_DATA_BYTES // (1024 * 1024)}MB) を超えています")

        if cancel_fn is not None and cancel_fn():
            from .image_generator import ImageGenerationError as _CancelErr

            raise _CancelErr("画像生成がキャンセルされました")

        generation_seed = (valid_seed + (iteration - 1) * 1000) & 0x7FFFFFFF

        if image_data is not None and len(image_data) > 0:
            active_image_bytes = image_data
        elif iteration > 1 and self._cached_image_data is not None and self._last_prompt == valid_prompt:
            active_image_bytes = self._cached_image_data
            self._log(f"ステップ {iteration}/{max_iterations}: 生成済み基準画像からストロークを分解・洗練中...")
        else:
            prompt_digest = hashlib.blake2b(valid_prompt.encode("utf-8"), digest_size=6).hexdigest()
            self._log(
                f"Text-to-Image 画像生成を開始します (Prompt: {len(valid_prompt)} chars, "
                f"digest={prompt_digest}, Aspect: {target_aspect:.2f})"
            )
            active_image_bytes = self.image_client.generate_image(
                valid_prompt,
                cancel_check=cancel_fn,
                target_aspect=target_aspect,
            )
            self._cached_image_data = active_image_bytes
            self._last_prompt = valid_prompt
            self._log("画像生成が完了しました。手描きストロークへ自動分解中...")

        image_plan = self.image_converter.convert_image_to_plan(
            image_bytes=active_image_bytes,
            prompt=valid_prompt,
            seed=generation_seed,
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
            prompt=f"AI Generated: {valid_prompt}",
            seed=valid_seed,
            strokes=image_plan.strokes,
            title="AI Generated Illustration",
            iteration=iteration,
            layers=image_plan.layers,
            metadata={
                **dict(image_plan.metadata),
                "generator": "text_to_image_to_stroke",
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
