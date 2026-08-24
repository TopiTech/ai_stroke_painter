from __future__ import annotations

from abc import ABC, abstractmethod
from collections.abc import Callable
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .domain import DrawingPlan, Stroke


class PlannerPort(ABC):
    @abstractmethod
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
        **kwargs: Any,
    ) -> DrawingPlan: ...


class CanvasPort(ABC):
    @abstractmethod
    def ensure_target(self, document: Any) -> Any: ...

    @abstractmethod
    def render(
        self,
        document: Any,
        plan: DrawingPlan,
        cancelled: Callable[[], bool] = lambda: False,
        **kwargs: Any,
    ) -> int: ...

    @abstractmethod
    def capture_canvas(self, document: Any, width: int = 512, height: int = 512) -> bytes: ...


class NativeStrokeBridgePort(ABC):
    """1本の全点列を単一の連続Kritaストロークとして処理する境界。"""

    @abstractmethod
    def submit_stroke(self, document: Any, target: Any, stroke: Stroke) -> int: ...
