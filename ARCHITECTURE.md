# Fork-ready architecture

UI -> Use case (`docker.py`) -> Ports (`ports.py`) -> Adapters.

## Native migration contract
C++ bridge should accept line-delimited JSON over a local authenticated IPC channel. Required fields: schema_version, stroke id, points[x,y,pressure,time_ms], brush_preset, color, size_px. It should return job_id, accepted point count, undo token, and errors.

## Planned increments
1. [x] QThread worker and cooperative cancellation
2. [x] LLM adapter producing validated DrawingPlan JSON
3. [ ] Krita action/undo macro integration
4. [ ] Native bridge prototype
5. [ ] Replace segment rendering with native continuous stroke
6. [ ] Vision critic and bounded retry policy
