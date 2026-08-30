# Architecture

`docker.py` (UI / use case) → `PlannerPort` → `SceneSpec` / semantic `RenderGraph` → `StrokeProgram v2` → validated `DrawingPlan v1` → `CanvasPort`.

## Drawing contract

- `stroke_program.py`: normalized, resolution-independent operations: `path`, `fill`, `gradient_fill`, `ribbon`, `hatch`, and deterministic `particles`.
- `brushes.py`: one semantic brush registry and role-compatibility policy shared by procedural generation, LLM compilation, and Krita resource resolution.
- `domain.py`: bounded renderer/persistence contract (`DrawingPlan`, max 2,000 strokes / 1,000 points each).
- `storage.py`: reads both v1 and v2; `load_program()` migrates v1 paths without changing stroke IDs or preset hints.

Procedural, Image-to-Stroke, and LLM planners all cross the same compiler boundary. LLM output prefers v2, while existing v1 responses remain accepted.

## Semantic scene composition

- `scene_spec.py` extracts subjects, environments, motifs, effects, style, and composition independently.
- `procedural/composition.py` assigns normalized primary, companion, effect, and subject-safe boxes before generators are composed.
- `procedural/color_plan.py` converts palette entries into background, subject, line, accent, and highlight value roles with explicit contrast floors.
- `procedural/render_graph.py` represents background, subject, motif, and effect as ordered nodes with priorities, bounds, minimum budgets, featured strokes, atomicity, and explicit required elements.
- `procedural/semantic_budget.py` selects recognizable facial, creature, city, mandala, mountain, tree, flower, and wave features first, preserves complete atomic symbols and minimum structure, then distributes decorative detail strokes.
- Fill compilation reserves 32% of a mixed plan for color masses. Under constrained budgets it derives brush width from polygon span and allotted rows, insets round caps at polygon edges, and preserves legacy wash behavior when widening is unnecessary. Ellipse-like foundations compile to one pressure-shaped major-axis mass plus inset value accents instead of disconnected scanline bands.
- Procedural foundations place background, environment, and subject value masses in render order. Characters receive separate hair, clothing, and face roles; creatures and companions receive bounded body masses; landscapes receive sky/ground/sea/mountain roles; city scenes receive a broad horizon mass while building identity remains in featured line geometry. At fewer than 48 requested strokes, full-canvas and horizon gradients degrade to deliberate solid value masses instead of visible posterized bands; localized subject gradients remain. Strict ink palettes quantize these roles instead of synthesizing intermediate colors.
- The composed plan records a semantic manifest, rendered elements, missing elements, node bounds, selected featured counts, the declarative foundation value plan, exact compiled gradient gamut, brush policy, and a source-version fingerprint. Draft strokes remain available to preview but are removed from final materialization. `quality.py` v3 recomputes semantic fidelity, actual raster-footprint coverage, feature geometry, character-feature occlusion, oversized-stroke ratios, role/brush compatibility, Draft leakage, subject/background luminance-distribution separation, and effect intrusion from the post-budget result, so broad coverage cannot make a structurally weak plan pass.
- `docker.py` renders that report below the non-destructive preview. Missing required elements are translated into user-facing labels and shown before canvas mutation, with a recommendation to increase the stroke budget.

## Transaction boundary

Generated layer/group modes roll back by removing only their run-owned container. Active-layer mode captures the target paint layer with `Node.pixelData()` before its first mutation and restores that exact layer with `Node.setPixelData()` on cancellation or failure. Direct canvas input is guarded while a synchronous active-layer render pumps Qt events, and the session records a digest of each completed target state. If an external change is detected before another pass or rollback, it fails closed rather than overwriting that change. A multi-pass session retains one original snapshot until commit, so later passes cannot turn a partial result into the rollback baseline.

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
- `quality_check.py`: deterministic representative-scene gates for raster appearance, semantic fidelity, feature geometry, layer separation, bounds, role-aware brush/palette contracts, and generation time.
- `krita_smoke.py`: disposable-document integration smoke for a real Krita 6 runtime.
