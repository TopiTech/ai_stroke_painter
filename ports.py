from __future__ import annotations

from abc import ABC, abstractmethod
from typing import TYPE_CHECKING, Any, Callable

if TYPE_CHECKING:
    from .domain import DrawingPlan, Stroke


class PlannerPort(ABC):
    @abstractmethod
    def plan(self, prompt: str, seed: int, count: int, width: float, height: float) -> DrawingPlan: ...


class CanvasPort(ABC):
    @abstractmethod
    def ensure_target(self, document: Any) -> Any: ...

    @abstractmethod
    def render(self, document: Any, plan: DrawingPlan, cancelled: Callable[[], bool] = lambda: False) -> int: ...


class NativeStrokeBridgePort(ABC):
    """Future C++/Krita-fork seam. Implement this without changing UI or planner."""

    @abstractmethod
    def submit_stroke(self, stroke: Stroke) -> None: ...
