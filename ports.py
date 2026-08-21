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
        count: int,
        width: float,
        height: float,
        image_data: bytes | None = None,
        canvas_image: bytes | None = None,
        iteration: int = 1,
        max_iterations: int = 1,
        palette_name: str = "anime",
    ) -> DrawingPlan: ...


class CanvasPort(ABC):
    @abstractmethod
    def ensure_target(self, document: Any) -> Any: ...

    @abstractmethod
    def render(self, document: Any, plan: DrawingPlan, cancelled: Callable[[], bool] = lambda: False) -> int: ...

    @abstractmethod
    def capture_canvas(self, document: Any, width: int = 512, height: int = 512) -> bytes: ...


class NativeStrokeBridgePort(ABC):
    """Future C++/Krita-fork seam. Implement this without changing UI or planner."""

    @abstractmethod
    def submit_stroke(self, stroke: Stroke) -> None: ...
