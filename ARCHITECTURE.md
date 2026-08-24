# Fork-ready architecture

UI -> Use case (`docker.py`) -> Ports (`ports.py`) -> Adapters.

## Native migration contract
C++ bridge should accept line-delimited JSON over a local authenticated IPC channel. Required fields: schema_version, stroke id, points[x,y,pressure,time_ms], brush_preset, color, size_px. It should return job_id, accepted point count, undo token, and errors.

## Planned increments
1. [x] Background worker and cooperative cancellation
2. [x] LLM adapter producing validated DrawingPlan JSON
3. [x] Per-run output groups, generated-output rollback, and active view pinning
4. [x] Automatic canvas feedback between Auto-Refine iterations
5. [x] Krita action/undo macro integration (especially active-layer mode)
6. [ ] Native bridge prototype
7. [ ] Replace segment rendering with native continuous stroke
