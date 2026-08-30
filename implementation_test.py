"""品質改善基盤の小さく高速な回帰テスト。"""

from __future__ import annotations

import math
import unittest
import uuid

from .brushes import brush_policy_for_profile
from .domain import DrawingPlan, Stroke, StrokePoint, materialize_render_options
from .procedural import generate_procedural_plan, generate_procedural_program
from .procedural.base import color_palette
from .procedural.color_plan import build_color_plan, contrast_ratio
from .procedural.composition import plan_scene_composition
from .procedural.geometry import geometry_feature_stroke_ids
from .procedural.landscape import landscape_feature_stroke_ids
from .procedural.manga_fx import generate_manga_fx_strokes
from .procedural.semantic_budget import SemanticStrokeGroup, allocate_semantic_groups
from .quality import evaluate_plan_quality
from .scene_spec import analyze_scene
from .stroke_program import (
    FillOperation,
    GradientFillOperation,
    ProgramBrush,
    ProgramPoint,
    StrokeProgram,
    compile_stroke_program,
)


def _stroke(
    stroke_id: str,
    start: tuple[float, float] = (5.0, 5.0),
    end: tuple[float, float] = (95.0, 95.0),
    *,
    color: str = "#202020",
    size_px: float = 4.0,
    layer: str = "Lineart",
) -> Stroke:
    return Stroke(
        stroke_id,
        (
            StrokePoint(start[0], start[1], 0.25, 0),
            StrokePoint(end[0], end[1], 1.0, 10),
        ),
        color=color,
        size_px=size_px,
        layer_name=layer,
    )


class SceneSpecTests(unittest.TestCase):
    def test_mixed_prompt_keeps_subject_environment_and_effect(self) -> None:
        spec = analyze_scene("cute cat in a cyberpunk city with focus lines", palette="cyberpunk")

        self.assertEqual(spec.primary_domain, "creature")
        self.assertIn("cat", spec.subjects)
        self.assertIn("city", spec.environments)
        self.assertIn("cyber_city", spec.motifs)
        self.assertIn("focus_lines", spec.effects)
        self.assertEqual(spec.palette, "cyberpunk")
        self.assertTrue({"cat", "city", "cyber_city", "focus_lines"}.issubset(spec.required_elements))

    def test_landscape_prompt_keeps_independent_motifs(self) -> None:
        spec = analyze_scene("fantasy sakura landscape with mountains and clouds")

        self.assertEqual(spec.primary_domain, "landscape")
        self.assertTrue({"mountain"}.issubset(spec.environments))
        self.assertTrue({"sakura", "mountain", "clouds"}.issubset(spec.motifs))

    def test_unknown_prompt_is_explicitly_unsupported(self) -> None:
        spec = analyze_scene("beautiful mysterious abstraction")

        self.assertEqual(spec.primary_domain, "unknown")
        self.assertFalse(spec.supported)
        self.assertEqual(spec.required_elements, ())

    def test_word_boundaries_do_not_detect_cat_inside_cathedral(self) -> None:
        spec = analyze_scene("gothic cathedral")

        self.assertNotIn("cat", spec.subjects)


class SemanticBudgetTests(unittest.TestCase):
    def test_atomic_subject_and_each_minimum_survive_budgeting(self) -> None:
        frame = tuple(_stroke(f"frame-{index}") for index in range(5))
        symbol = tuple(_stroke(f"symbol-{index}") for index in range(2))
        accents = tuple(_stroke(f"accent-{index}") for index in range(4))
        selected = allocate_semantic_groups(
            (
                SemanticStrokeGroup("frame", frame, "frame", priority=90, minimum_count=2),
                SemanticStrokeGroup("symbol", symbol, "subject", priority=100, atomic=True),
                SemanticStrokeGroup("accent", accents, "accent", priority=60, minimum_count=1),
            ),
            5,
        )
        selected_ids = {stroke.id for stroke in selected}

        self.assertEqual(len(selected), 5)
        self.assertTrue({"symbol-0", "symbol-1"}.issubset(selected_ids))
        self.assertEqual(sum(stroke_id.startswith("frame-") for stroke_id in selected_ids), 2)
        self.assertEqual(sum(stroke_id.startswith("accent-") for stroke_id in selected_ids), 1)

    def test_magic_circle_budget_keeps_complete_symbol(self) -> None:
        strokes = generate_manga_fx_strokes("magic circle", 42, 6, 800, 600)
        ids = {stroke.id for stroke in strokes}
        expected_stars = {
            str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/fx/42/magic_star/{index}")) for index in (0, 5)
        }

        self.assertEqual(len(strokes), 6)
        self.assertTrue(expected_stars.issubset(ids))

        main_plan = generate_procedural_plan("magic circle", 42, 6, 800, 600)
        self.assertEqual(len(main_plan.strokes), 6)
        self.assertTrue(expected_stars.issubset({stroke.id for stroke in main_plan.strokes}))

    def test_two_stroke_hatching_keeps_both_directions(self) -> None:
        strokes = generate_manga_fx_strokes("crosshatching", 7, 2, 800, 600)
        ids = {stroke.id for stroke in strokes}

        self.assertIn(str(uuid.uuid5(uuid.NAMESPACE_URL, "ai-stroke/fx/7/hatch_d1/0")), ids)
        self.assertIn(str(uuid.uuid5(uuid.NAMESPACE_URL, "ai-stroke/fx/7/hatch_d2/0")), ids)

    def test_featured_strokes_are_selected_before_decorative_details(self) -> None:
        strokes = tuple(_stroke(f"part-{index}") for index in range(8))
        selected = allocate_semantic_groups(
            (
                SemanticStrokeGroup(
                    "face",
                    strokes,
                    "subject",
                    minimum_count=4,
                    featured_ids=("part-6", "part-2", "part-7", "part-1"),
                ),
            ),
            3,
        )

        self.assertEqual([stroke.id for stroke in selected], ["part-6", "part-2", "part-7"])


class QualityV2Tests(unittest.TestCase):
    def test_uniform_fill_cannot_pass_as_high_visual_quality(self) -> None:
        uniform_stroke = Stroke(
            "fill",
            (
                StrokePoint(0.0, 50.0, 1.0, 0),
                StrokePoint(99.0, 50.0, 1.0, 10),
            ),
            color="#202020",
            size_px=160.0,
            layer_name="Lineart",
        )
        uniform = DrawingPlan(
            "uniform",
            1,
            (uniform_stroke,),
            canvas_width=100,
            canvas_height=100,
        )
        reference = generate_procedural_plan("anime girl portrait", 42, None, 400, 300)
        uniform_report = evaluate_plan_quality(uniform)
        reference_report = evaluate_plan_quality(reference)

        self.assertEqual(uniform_report.quality_version, 3)
        self.assertLess(uniform_report.visual_score, reference_report.visual_score)
        self.assertLess(uniform_report.value_range, 0.08)
        self.assertGreater(uniform_report.dominant_color_ratio, 0.95)
        self.assertTrue(any("単一色" in issue or "明度差" in issue for issue in uniform_report.issues))

    def test_banding_metric_is_context_aware(self) -> None:
        stripes = tuple(
            _stroke(
                f"stripe-{index}",
                (0.0, 5.0 + index * 8.0),
                (99.0, 5.0 + index * 8.0),
                color="#202020" if index % 2 == 0 else "#b0b0b0",
                size_px=5.0,
            )
            for index in range(12)
        )
        ordinary = evaluate_plan_quality(DrawingPlan("stripes", 1, stripes, canvas_width=100, canvas_height=100))
        geometry = evaluate_plan_quality(
            DrawingPlan(
                "city grid",
                1,
                stripes,
                metadata={"scene_spec": {"primary_domain": "geometry"}},
                canvas_width=100,
                canvas_height=100,
            )
        )

        self.assertGreater(ordinary.horizontal_banding_score, 0.75)
        self.assertTrue(any("帯状" in issue for issue in ordinary.issues))
        self.assertFalse(any("帯状" in issue for issue in geometry.issues))

    def test_focus_precedence_creates_radial_lineart(self) -> None:
        width, height = 800.0, 600.0
        strokes = generate_manga_fx_strokes("focus radial speed lines", 42, 20, width, height)
        center = (width * 0.5, height * 0.5)
        expected_focus = {
            str(uuid.uuid5(uuid.NAMESPACE_URL, f"ai-stroke/fx/42/focus_line/{index}")) for index in range(40)
        }
        expected_speed = str(uuid.uuid5(uuid.NAMESPACE_URL, "ai-stroke/fx/42/speed_line/0"))

        self.assertEqual(len(strokes), 20)
        self.assertTrue(all(stroke.id in expected_focus for stroke in strokes))
        self.assertNotIn(expected_speed, {stroke.id for stroke in strokes})
        self.assertTrue(all(stroke.layer_name == "Lineart" for stroke in strokes))
        for stroke in strokes:
            outer, inner = stroke.points
            self.assertGreater(
                math.hypot(outer.x - center[0], outer.y - center[1]),
                math.hypot(inner.x - center[0], inner.y - center[1]),
            )


class GradientFillBudgetTests(unittest.TestCase):
    def test_full_canvas_solid_fill_uses_canvas_clipping_without_scalloped_gaps(self) -> None:
        operation = FillOperation(
            id="full-solid",
            polygon=(ProgramPoint(0, 0), ProgramPoint(1, 0), ProgramPoint(1, 1), ProgramPoint(0, 1)),
            brush=ProgramBrush(profile="airbrush", color="#8cb5de", size=0.04),
        )
        plan = compile_stroke_program(
            StrokeProgram(
                prompt="solid mass",
                seed=1,
                canvas_width=200,
                canvas_height=200,
                operations=(operation,),
            ),
            count=2,
        )

        self.assertGreaterEqual(evaluate_plan_quality(plan).coverage, 0.99)
        self.assertTrue(all(min(stroke.points[0].x, stroke.points[-1].x) <= 0.5 for stroke in plan.strokes))

    def test_two_gradient_strokes_expand_to_preserve_full_color_mass(self) -> None:
        operation = GradientFillOperation(
            id="full-gradient",
            polygon=(ProgramPoint(0, 0), ProgramPoint(1, 0), ProgramPoint(1, 1), ProgramPoint(0, 1)),
            colors=("#16213e", "#f4d9c6"),
            brush=ProgramBrush(profile="airbrush", color="#16213e", size=0.04),
        )
        plan = compile_stroke_program(
            StrokeProgram(
                prompt="gradient mass",
                seed=1,
                canvas_width=200,
                canvas_height=200,
                operations=(operation,),
            ),
            count=2,
        )
        report = evaluate_plan_quality(plan)

        self.assertEqual(len(plan.strokes), 2)
        self.assertEqual(len({stroke.color for stroke in plan.strokes}), 2)
        self.assertEqual(set(plan.metadata["compiled_gradient_colors"]), {stroke.color for stroke in plan.strokes})
        self.assertGreaterEqual(report.coverage, 0.90)
        self.assertTrue(all(125 <= stroke.size_px <= 140 for stroke in plan.strokes))

    def test_ellipse_gradient_uses_pressure_shaped_axis_and_inset_value_strokes(self) -> None:
        ellipse = tuple(
            ProgramPoint(0.5 + math.cos(index / 24 * math.tau) * 0.2, 0.5 + math.sin(index / 24 * math.tau) * 0.4)
            for index in range(24)
        )
        operation = GradientFillOperation(
            id="ellipse-value",
            polygon=ellipse,
            colors=("#f8d8c8", "#e8a890", "#9a5848"),
            brush=ProgramBrush(profile="airbrush", color="#e8a890", size=0.04),
        )
        plan = compile_stroke_program(
            StrokeProgram(
                prompt="ellipse value mass",
                seed=1,
                canvas_width=200,
                canvas_height=200,
                operations=(operation,),
            ),
            count=3,
        )

        self.assertEqual([stroke.color for stroke in plan.strokes], ["#e8a890", "#9a5848", "#f8d8c8"])
        self.assertEqual(len(plan.strokes[0].points), 5)
        self.assertGreater(max(point.pressure for point in plan.strokes[0].points), 0.9)
        self.assertLess(min(point.pressure for point in plan.strokes[0].points), 0.2)
        self.assertAlmostEqual(plan.strokes[0].size_px, 80.0, delta=1.0)
        self.assertLess(plan.strokes[1].size_px, plan.strokes[0].size_px * 0.5)


class RenderGraphIntegrationTests(unittest.TestCase):
    def test_mixed_scene_composes_background_subject_companion_and_effect(self) -> None:
        prompt = "anime girl with a cat in a cyberpunk city and focus lines"
        plan = generate_procedural_plan(prompt, 42, 40, 800, 600)
        report = evaluate_plan_quality(plan)
        manifest = {entry["node_id"]: entry for entry in plan.metadata["semantic_manifest"]}

        self.assertEqual(len(plan.strokes), 40)
        self.assertEqual(plan.metadata["render_graph_version"], 1)
        self.assertEqual(
            set(plan.metadata["rendered_elements"]),
            {"character", "cat", "city", "cyber_city", "focus_lines"},
        )
        self.assertFalse(plan.metadata["missing_elements"])
        self.assertTrue(
            {
                "background-geometry",
                "subject-character",
                "subject-cat",
                "effect-focus-lines",
            }.issubset(manifest)
        )
        self.assertTrue(all(entry["complete"] for entry in manifest.values() if entry["required_for"]))
        self.assertEqual(manifest["subject-character"]["featured_count"], 10)
        self.assertEqual(manifest["subject-cat"]["featured_count"], 8)
        self.assertEqual(
            sum(stroke.id not in plan.metadata["stroke_node_map"] for stroke in plan.strokes),
            13,
        )
        self.assertGreaterEqual(report.semantic_fidelity_score, 0.90)
        self.assertGreaterEqual(report.feature_geometry_score, 0.85)
        self.assertFalse(report.missing_required_elements)
        self.assertGreater(report.subject_background_contrast, 0.05)
        self.assertLess(report.effect_subject_intrusion_ratio, 0.10)
        self.assertEqual(plan.metadata["composition_plan"]["mode"], "subject-companion")

    def test_low_budget_reports_missing_semantics_instead_of_false_success(self) -> None:
        prompt = "anime girl with a cat in a cyberpunk city and focus lines"
        low_budget = generate_procedural_plan(prompt, 42, 12, 800, 600)
        standard = generate_procedural_plan(prompt, 42, 40, 800, 600)
        low_report = evaluate_plan_quality(low_budget)
        standard_report = evaluate_plan_quality(standard)

        self.assertLess(low_report.semantic_fidelity_score, 1.0)
        self.assertTrue(low_report.missing_required_elements)
        self.assertTrue(low_report.incomplete_semantic_groups)
        self.assertTrue(any("必須要素" in issue for issue in low_report.issues))
        self.assertLess(low_report.score, standard_report.score)

    def test_multiple_creatures_receive_independent_nodes(self) -> None:
        plan = generate_procedural_plan("cat and dog companions", 9, 40, 800, 600)
        manifest = {entry["node_id"]: entry for entry in plan.metadata["semantic_manifest"]}

        self.assertIn("subject-cat", manifest)
        self.assertIn("subject-dog", manifest)
        self.assertTrue(manifest["subject-cat"]["complete"])
        self.assertTrue(manifest["subject-dog"]["complete"])
        self.assertTrue({"cat", "dog"}.issubset(plan.metadata["rendered_elements"]))

    def test_glow_uses_aura_geometry_instead_of_fallback_focus_lines(self) -> None:
        strokes = generate_manga_fx_strokes("glowing aura", 3, 5, 800, 600)
        focus_id = str(uuid.uuid5(uuid.NAMESPACE_URL, "ai-stroke/fx/3/focus_line/0"))

        self.assertEqual(len(strokes), 5)
        self.assertNotIn(focus_id, {stroke.id for stroke in strokes})
        self.assertEqual({stroke.layer_name for stroke in strokes}, {"FX", "Highlights"})

        plan = generate_procedural_plan("glowing aura around an anime hero", 3, 40, 800, 600)
        report = evaluate_plan_quality(plan)
        self.assertLess(report.effect_subject_intrusion_ratio, 0.15)
        self.assertFalse(any("安全領域" in issue for issue in report.issues))

    def test_city_background_keeps_distributed_outlines_and_windows(self) -> None:
        prompt = "anime girl with a cat in a cyberpunk city and focus lines"
        plan = generate_procedural_plan(prompt, 42, 40, 800, 600, palette_name="cyberpunk")
        manifest = {entry["node_id"]: entry for entry in plan.metadata["semantic_manifest"]}
        node_map = plan.metadata["stroke_node_map"]
        selected_background = {stroke_id for stroke_id, node_id in node_map.items() if node_id == "background-geometry"}
        expected = set(geometry_feature_stroke_ids(prompt, 42 + 4_267)[:5])

        self.assertEqual(manifest["background-geometry"]["featured_count"], 5)
        self.assertEqual(selected_background, expected)
        self.assertEqual(
            sum(stroke.layer_name == "Lineart" for stroke in plan.strokes if stroke.id in selected_background),
            5,
        )

    def test_wave_budget_keeps_main_arc_foam_and_distant_fuji(self) -> None:
        prompt = "hokusai great wave with foam and ripples"
        plan = generate_procedural_plan(prompt, 7, 12, 800, 600)
        selected = set(plan.metadata["stroke_node_map"])
        features = landscape_feature_stroke_ids(prompt, 7)

        self.assertTrue(set(features).issubset(selected))
        self.assertEqual(plan.metadata["semantic_fidelity"], 1.0)


class CompositionAndColorPlanTests(unittest.TestCase):
    def test_subject_companion_layout_separates_focal_regions(self) -> None:
        scene = analyze_scene("anime girl with a cat in a cyberpunk city and focus lines")
        composition = plan_scene_composition(scene)

        self.assertEqual(composition.mode, "subject-companion")
        self.assertEqual(len(composition.companions), 1)
        self.assertLess(composition.primary.center[0], composition.companions[0].center[0])
        self.assertAlmostEqual(composition.effect.center[0], composition.primary.center[0], delta=0.03)
        self.assertGreaterEqual(composition.subject_safe.x0, composition.primary.x0)
        self.assertLessEqual(composition.subject_safe.x1, composition.primary.x1)

    def test_color_plan_enforces_role_contrast_for_light_and_dark_palettes(self) -> None:
        anime = build_color_plan("anime", color_palette("anime"))
        cyberpunk = build_color_plan("cyberpunk", color_palette("cyberpunk"))

        self.assertGreaterEqual(contrast_ratio(anime.subject_base, anime.background), 1.35)
        self.assertGreaterEqual(contrast_ratio(anime.accent, anime.background), 1.8)
        self.assertTrue(cyberpunk.dark_background)
        self.assertGreaterEqual(contrast_ratio(cyberpunk.subject_base, cyberpunk.background), 4.5)
        self.assertGreaterEqual(contrast_ratio(cyberpunk.accent, cyberpunk.background), 1.8)

    def test_mixed_scene_foundations_encode_environment_and_subject_value_masses(self) -> None:
        prompt = "anime girl with a cat in a cyberpunk city and focus lines"
        program = generate_procedural_program(prompt, 42, 40, 800, 600)
        value_plan = {entry["id"]: entry for entry in program.metadata["foundation_value_plan"]}

        self.assertTrue(
            {
                "foundation-background",
                "foundation-skyline-mass",
                "foundation-back-hair",
                "foundation-clothing",
                "foundation-face",
                "foundation-companion-1-body",
            }.issubset(value_plan)
        )
        self.assertEqual(value_plan["foundation-background"]["kind"], "fill")
        self.assertEqual(value_plan["foundation-skyline-mass"]["kind"], "fill")
        for entry_id in (
            "foundation-back-hair",
            "foundation-clothing",
            "foundation-face",
            "foundation-companion-1-body",
        ):
            self.assertEqual(value_plan[entry_id]["kind"], "gradient_fill")
            self.assertGreaterEqual(len(set(value_plan[entry_id]["colors"])), 2)

        report = evaluate_plan_quality(generate_procedural_plan(prompt, 42, 40, 800, 600))
        self.assertGreater(report.value_range, 0.55)
        self.assertGreater(report.subject_background_contrast, 0.10)

    def test_domain_foundations_use_value_gradients_for_major_masses(self) -> None:
        scenarios = {
            "hokusai great wave": {
                "foundation-sky": "fill",
                "foundation-sea": "fill",
            },
            "cute cat": {
                "foundation-background": "fill",
                "foundation-body": "gradient_fill",
            },
            "cyberpunk city skyline": {
                "foundation-background": "fill",
                "foundation-skyline-mass": "fill",
            },
        }
        for prompt, expected_kinds in scenarios.items():
            with self.subTest(prompt=prompt):
                program = generate_procedural_program(prompt, 7, 40, 800, 600)
                value_plan = {entry["id"]: entry for entry in program.metadata["foundation_value_plan"]}
                self.assertTrue(expected_kinds.keys() <= value_plan.keys())
                self.assertTrue(all(value_plan[entry_id]["kind"] == kind for entry_id, kind in expected_kinds.items()))

    def test_broad_gradients_degrade_to_clean_solid_masses_only_when_budget_is_constrained(self) -> None:
        prompt = "hokusai great wave with foam and ripples"
        constrained = generate_procedural_program(prompt, 7, 24, 800, 600, palette_name="nature")
        unconstrained = generate_procedural_program(prompt, 7, None, 800, 600, palette_name="nature")
        constrained_values = {entry["id"]: entry for entry in constrained.metadata["foundation_value_plan"]}
        unconstrained_values = {entry["id"]: entry for entry in unconstrained.metadata["foundation_value_plan"]}

        self.assertEqual(constrained_values["foundation-sky"]["kind"], "fill")
        self.assertEqual(constrained_values["foundation-sea"]["kind"], "fill")
        self.assertEqual(unconstrained_values["foundation-sky"]["kind"], "gradient_fill")
        self.assertEqual(unconstrained_values["foundation-sea"]["kind"], "gradient_fill")
        report = evaluate_plan_quality(generate_procedural_plan(prompt, 7, 24, 800, 600, palette_name="nature"))
        self.assertLess(report.horizontal_banding_score, 0.15)
        self.assertGreater(report.visual_score, 0.95)

    def test_strict_palette_foundations_do_not_invent_intermediate_colors(self) -> None:
        program = generate_procedural_program(
            "anime girl portrait",
            7,
            40,
            800,
            600,
            palette_name="monochrome",
        )
        registered = {color.lower() for color in color_palette("monochrome").values()}

        self.assertTrue(program.metadata["foundation_value_plan"])
        self.assertTrue(
            all(
                entry["kind"] == "fill" and set(entry["colors"]).issubset(registered)
                for entry in program.metadata["foundation_value_plan"]
            )
        )

    def test_strict_monochrome_color_plan_uses_registered_colors_only(self) -> None:
        colors = color_palette("monochrome")
        plan = build_color_plan("monochrome", colors)
        registered = {color.lower() for color in colors.values()}

        self.assertTrue(
            {
                plan.background,
                plan.background_deep,
                plan.background_detail,
                plan.subject_base,
                plan.subject_mid,
                plan.subject_shadow,
                plan.lineart,
                plan.accent,
                plan.highlight,
            }.issubset(registered)
        )


class QualityV3ContractTests(unittest.TestCase):
    def test_builtin_goldens_use_role_aware_brushes_and_bounded_auto_budgets(self) -> None:
        cases = (
            ("anime girl portrait, delicate eyes, flowing hair", "anime", "gpen", 132),
            ("fantasy sakura landscape with mountains and clouds", "nature", "brush", 96),
        )
        for prompt, palette, profile, maximum in cases:
            with self.subTest(prompt=prompt):
                plan = generate_procedural_plan(
                    prompt,
                    42,
                    None,
                    2480,
                    3508,
                    palette_name=palette,
                    brush_profile=profile,
                )
                report = evaluate_plan_quality(plan)
                self.assertLessEqual(len(plan.strokes), maximum)
                self.assertEqual(plan.metadata["draft_policy"], "preview_only")
                self.assertEqual(plan.metadata["brush_policy"]["mode"], "role_aware")
                self.assertGreaterEqual(report.brush_role_compatibility_score, 0.95)
                self.assertGreaterEqual(report.feature_geometry_score, 0.85)
                self.assertFalse(report.issues)
                self.assertGreater(len({stroke.brush_preset for stroke in plan.strokes}), 1)

    def test_old_single_pen_contract_is_rejected(self) -> None:
        strokes = tuple(
            _stroke(f"s-{index}", layer=layer)
            for index, layer in enumerate(("Flats", "Shading", "Lineart", "Highlights"))
        )
        strokes = tuple(
            Stroke(
                stroke.id,
                stroke.points,
                brush_preset="Ink-2 Fineliner",
                color=stroke.color,
                size_px=stroke.size_px,
                layer_name=stroke.layer_name,
            )
            for stroke in strokes
        )
        plan = DrawingPlan(
            "single pen",
            1,
            strokes,
            metadata={"brush_policy": brush_policy_for_profile("gpen").as_dict()},
            canvas_width=100,
            canvas_height=100,
        )

        report = evaluate_plan_quality(plan)

        self.assertLess(report.brush_role_compatibility_score, 0.75)
        self.assertTrue(any("役割" in issue for issue in report.issues))

    def test_materialized_final_omits_preview_only_draft(self) -> None:
        draft = _stroke("draft", layer="Draft")
        line = _stroke("line", layer="Lineart")
        plan = DrawingPlan(
            "draft policy",
            1,
            (draft, line),
            layers=("Draft", "Lineart"),
            metadata={"draft_policy": "preview_only"},
        )

        final_plan = materialize_render_options(plan)

        self.assertEqual([stroke.id for stroke in final_plan.strokes], ["line"])
        self.assertEqual(final_plan.layers, ("Lineart",))

    def test_pressure_variation_is_bounded_into_continuous_sections(self) -> None:
        from .krita_adapter import _pressure_path_sections

        points = tuple(
            StrokePoint(float(index * 10), float(index * 4), 0.1 if index % 2 == 0 else 1.0, index * 10)
            for index in range(24)
        )
        sections = _pressure_path_sections(Stroke("pressure", points))

        self.assertLessEqual(len(sections), 6)
        self.assertTrue(all(len(section) >= 2 for section in sections))
        self.assertEqual(sections[0][0], points[0])
        self.assertEqual(sections[-1][-1], points[-1])

    def test_generation_trace_identifies_installed_sources(self) -> None:
        plan = generate_procedural_plan("anime girl portrait", 7, 40, 800, 600)
        trace = plan.metadata["generation_trace"]

        self.assertEqual(trace["plugin_version"], "1.2.0")
        self.assertEqual(len(trace["source_fingerprint"]), 16)
        self.assertEqual(trace["count_mode"], "manual")


if __name__ == "__main__":
    unittest.main()
