# Architecture

`docker.py` (UI / use case) → `PlannerPort` → `StrokeProgram v2` → validated `DrawingPlan v1` → `CanvasPort`.

## Drawing contract

- `stroke_program.py`: normalized, resolution-independent operations: `path`, `fill`, `hatch`, and deterministic `particles`.
- `brushes.py`: one semantic brush registry shared by procedural generation, LLM compilation, and Krita resource resolution.
- `domain.py`: bounded renderer/persistence contract (`DrawingPlan`, max 2,000 strokes / 1,000 points each).
- `storage.py`: reads both v1 and v2; `load_program()` migrates v1 paths without changing stroke IDs or preset hints.

Procedural, Image-to-Stroke, and LLM planners all cross the same compiler boundary. LLM output prefers v2, while existing v1 responses remain accepted.

## Transaction boundary

Generated layer/group modes roll back by removing only their run-owned container. Active-layer mode captures the target paint layer with `Node.pixelData()` before its first mutation and restores that exact layer with `Node.setPixelData()` on cancellation or failure. A multi-pass session retains one original snapshot until commit, so later passes cannot turn a partial result into the rollback baseline.

Some Krita forks expose `createMacro()` / `endMacro()` and those are used opportunistically for nicer history grouping, but correctness never depends on them. The standard Python compatibility path guarantees rollback isolation, not a single `Ctrl+Z` history entry.

## Continuous native stroke bridge

`native_bridge.py` sends one complete stroke per authenticated JSON-lines message to an explicitly configured loopback helper. The message contains:

- protocol version and message type;
- authentication token;
- document and target identifiers;
- the full validated stroke, including all `[x, y, pressure, time_ms]` points and brush style.

The helper must return `{"ok": true, "accepted_point_count": N}` only after applying the whole stroke to the target. A failure to establish the connection falls back to `Node.paintLine`; once sending starts, missing/malformed/rejected responses fail closed because the remote commit state is ambiguous. Configure with `AI_STROKE_BRIDGE_PORT` and a 16+ character `AI_STROKE_BRIDGE_TOKEN`.

## Verification boundaries

- `check.py`: lint, format, two type checkers, normal and dependency-free headless regression suites.
- `quality_check.py`: deterministic representative-scene gates for coverage, layer separation, pressure dynamics, fragmentation, bounds, and generation time.
- `krita_smoke.py`: disposable-document integration smoke for a real Krita 6 runtime.
