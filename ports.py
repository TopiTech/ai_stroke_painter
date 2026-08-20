from abc import ABC, abstractmethod

class PlannerPort(ABC):
    @abstractmethod
    def plan(self, prompt, seed, count, width, height): ...

class CanvasPort(ABC):
    @abstractmethod
    def ensure_target(self, document): ...
    @abstractmethod
    def render(self, document, plan, cancelled): ...

class NativeStrokeBridgePort(ABC):
    """Future C++/Krita-fork seam. Implement this without changing UI or planner."""
    @abstractmethod
    def submit_stroke(self, stroke): ...
