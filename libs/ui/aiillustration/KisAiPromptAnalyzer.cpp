/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPromptAnalyzer.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <cmath>

namespace
{
bool containsWord(const QString &text, const QString &word)
{
    const QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(word) + QStringLiteral("\\b"),
                                QRegularExpression::CaseInsensitiveOption);
    return text.contains(re);
}
}

KisAiPromptAnalyzer::SemanticSpec KisAiPromptAnalyzer::analyze(
    const QString &prompt,
    const QSize &canvasSize
)
{
    SemanticSpec spec;
    const QString text = prompt.toLower();

    // 1. Aspect ratio & Composition
    const qreal w = qMax(1, canvasSize.width());
    const qreal h = qMax(1, canvasSize.height());
    spec.aspectRatio = w / h;
    if (spec.aspectRatio >= 1.25) {
        spec.composition = CompositionType::LandscapeWide;
    } else if (spec.aspectRatio <= 0.80) {
        spec.composition = CompositionType::PortraitVertical;
    } else {
        spec.composition = CompositionType::BalancedStandard;
    }

    // 2. Time of Day & Lighting Tone
    if (text.contains(QStringLiteral("sunset")) || text.contains(QStringLiteral("dusk")) ||
        text.contains(QStringLiteral("twilight")) || text.contains(QStringLiteral("evening")) ||
        text.contains(QStringLiteral("golden hour")) || text.contains(QStringLiteral("夕焼け")) ||
        text.contains(QStringLiteral("夕暮れ")) || text.contains(QStringLiteral("黄昏")) ||
        text.contains(QStringLiteral("夕日"))) {
        spec.timeOfDay = TimeOfDay::Sunset;
        spec.skyGradientColors = {
            QStringLiteral("#4a1c40"), QStringLiteral("#962d3e"),
            QStringLiteral("#d25938"), QStringLiteral("#f8a846"),
            QStringLiteral("#fae19c")
        };
    } else if (text.contains(QStringLiteral("night")) || text.contains(QStringLiteral("starry")) ||
               text.contains(QStringLiteral("midnight")) || text.contains(QStringLiteral("moon")) ||
               text.contains(QStringLiteral("galaxy")) || text.contains(QStringLiteral("夜")) ||
               text.contains(QStringLiteral("星空")) || text.contains(QStringLiteral("月夜")) ||
               text.contains(QStringLiteral("深更"))) {
        spec.timeOfDay = TimeOfDay::Night;
        spec.skyGradientColors = {
            QStringLiteral("#050811"), QStringLiteral("#0d1527"),
            QStringLiteral("#1a2942"), QStringLiteral("#2c4365")
        };
    } else if (text.contains(QStringLiteral("magic")) || text.contains(QStringLiteral("fantasy")) ||
               text.contains(QStringLiteral("dreamy")) || text.contains(QStringLiteral("mystic")) ||
               text.contains(QStringLiteral("ファンタジー")) || text.contains(QStringLiteral("幻想"))) {
        spec.timeOfDay = TimeOfDay::Fantasy;
        spec.skyGradientColors = {
            QStringLiteral("#181432"), QStringLiteral("#3c2b5e"),
            QStringLiteral("#734b8c"), QStringLiteral("#d697b8")
        };
    } else {
        spec.timeOfDay = TimeOfDay::Day;
        spec.skyGradientColors = {
            QStringLiteral("#2b5c8f"), QStringLiteral("#5c93cf"),
            QStringLiteral("#eef6ff")
        };
    }

    // 3. Domain classification
    spec.hasSakura = text.contains(QStringLiteral("sakura")) ||
                     text.contains(QStringLiteral("桜")) ||
                     text.contains(QStringLiteral("cherry blossom"));

    spec.hasCharacter = containsWord(text, QStringLiteral("girl")) ||
                        containsWord(text, QStringLiteral("boy")) ||
                        containsWord(text, QStringLiteral("woman")) ||
                        containsWord(text, QStringLiteral("man")) ||
                        containsWord(text, QStringLiteral("character")) ||
                        containsWord(text, QStringLiteral("anime")) ||
                        containsWord(text, QStringLiteral("portrait")) ||
                        containsWord(text, QStringLiteral("face")) ||
                        text.contains(QStringLiteral("少女")) ||
                        text.contains(QStringLiteral("少年")) ||
                        text.contains(QStringLiteral("美少女")) ||
                        text.contains(QStringLiteral("顔")) ||
                        text.contains(QStringLiteral("人物")) ||
                        text.contains(QStringLiteral("ポートレート"));

    spec.hasEnvironment = containsWord(text, QStringLiteral("landscape")) ||
                          containsWord(text, QStringLiteral("mountain")) ||
                          containsWord(text, QStringLiteral("tree")) ||
                          containsWord(text, QStringLiteral("forest")) ||
                          containsWord(text, QStringLiteral("cloud")) ||
                          containsWord(text, QStringLiteral("ocean")) ||
                          containsWord(text, QStringLiteral("sea")) ||
                          containsWord(text, QStringLiteral("wave")) ||
                          containsWord(text, QStringLiteral("river")) ||
                          containsWord(text, QStringLiteral("lake")) ||
                          text.contains(QStringLiteral("風景")) ||
                          text.contains(QStringLiteral("山")) ||
                          text.contains(QStringLiteral("雲")) ||
                          text.contains(QStringLiteral("海")) ||
                          text.contains(QStringLiteral("波")) ||
                          spec.hasSakura;

    const bool hasCyber = containsWord(text, QStringLiteral("cyber")) ||
                          containsWord(text, QStringLiteral("cyberpunk")) ||
                          containsWord(text, QStringLiteral("city")) ||
                          containsWord(text, QStringLiteral("neon")) ||
                          containsWord(text, QStringLiteral("skyline")) ||
                          containsWord(text, QStringLiteral("sci-fi")) ||
                          containsWord(text, QStringLiteral("mech")) ||
                          text.contains(QStringLiteral("都市")) ||
                          text.contains(QStringLiteral("ビル")) ||
                          text.contains(QStringLiteral("ネオン"));

    const bool hasCreature = containsWord(text, QStringLiteral("cat")) ||
                            containsWord(text, QStringLiteral("kitten")) ||
                            containsWord(text, QStringLiteral("dog")) ||
                            containsWord(text, QStringLiteral("bird")) ||
                            containsWord(text, QStringLiteral("dragon")) ||
                            containsWord(text, QStringLiteral("creature")) ||
                            containsWord(text, QStringLiteral("animal")) ||
                            text.contains(QStringLiteral("猫")) ||
                            text.contains(QStringLiteral("犬")) ||
                            text.contains(QStringLiteral("鳥")) ||
                            text.contains(QStringLiteral("竜")) ||
                            text.contains(QStringLiteral("ドラゴン")) ||
                            text.contains(QStringLiteral("動物"));

    const bool hasBotanical = containsWord(text, QStringLiteral("flower")) ||
                             containsWord(text, QStringLiteral("rose")) ||
                             containsWord(text, QStringLiteral("bouquet")) ||
                             containsWord(text, QStringLiteral("petal")) ||
                             containsWord(text, QStringLiteral("blossom")) ||
                             text.contains(QStringLiteral("花")) ||
                             text.contains(QStringLiteral("バラ")) ||
                             text.contains(QStringLiteral("薔薇")) ||
                             text.contains(QStringLiteral("花束"));

    const bool hasMangaFx = text.contains(QStringLiteral("focus line")) ||
                            text.contains(QStringLiteral("speed line")) ||
                            text.contains(QStringLiteral("magic circle")) ||
                            text.contains(QStringLiteral("rune")) ||
                            text.contains(QStringLiteral("集中線")) ||
                            text.contains(QStringLiteral("流線")) ||
                            text.contains(QStringLiteral("魔法陣")) ||
                            text.contains(QStringLiteral("効果線"));

    if (spec.hasCharacter) {
        spec.domain = DomainType::Character;
    } else if (hasCyber) {
        spec.domain = DomainType::Cyberpunk;
    } else if (hasCreature) {
        spec.domain = DomainType::Creature;
    } else if (hasBotanical) {
        spec.domain = DomainType::Botanical;
    } else if (hasMangaFx) {
        spec.domain = DomainType::MangaFx;
    } else if (spec.hasEnvironment) {
        spec.domain = DomainType::Landscape;
    } else {
        spec.domain = DomainType::General;
    }

    // 4. Character color extraction heuristics
    if (text.contains(QStringLiteral("silver")) || text.contains(QStringLiteral("white hair")) ||
        text.contains(QStringLiteral("銀髪")) || text.contains(QStringLiteral("白髪"))) {
        spec.hairColor = QStringLiteral("#e0e4f0");
    } else if (text.contains(QStringLiteral("blonde")) || text.contains(QStringLiteral("gold")) ||
               text.contains(QStringLiteral("金髪"))) {
        spec.hairColor = QStringLiteral("#f8d376");
    } else if (text.contains(QStringLiteral("pink hair")) || text.contains(QStringLiteral("ピンク髪"))) {
        spec.hairColor = QStringLiteral("#ff9ebb");
    } else if (text.contains(QStringLiteral("blue hair")) || text.contains(QStringLiteral("青髪"))) {
        spec.hairColor = QStringLiteral("#4a7ee6");
    } else if (text.contains(QStringLiteral("black hair")) || text.contains(QStringLiteral("黒髪"))) {
        spec.hairColor = QStringLiteral("#1c1c26");
    } else if (text.contains(QStringLiteral("brown hair")) || text.contains(QStringLiteral("茶髪"))) {
        spec.hairColor = QStringLiteral("#553526");
    }

    if (text.contains(QStringLiteral("red eyes")) || text.contains(QStringLiteral("赤目")) ||
        text.contains(QStringLiteral("紅"))) {
        spec.eyeColor = QStringLiteral("#e63946");
    } else if (text.contains(QStringLiteral("green eyes")) || text.contains(QStringLiteral("緑目"))) {
        spec.eyeColor = QStringLiteral("#2a9d8f");
    } else if (text.contains(QStringLiteral("purple eyes")) || text.contains(QStringLiteral("紫目"))) {
        spec.eyeColor = QStringLiteral("#9b5de5");
    } else if (text.contains(QStringLiteral("gold eyes")) || text.contains(QStringLiteral("金目"))) {
        spec.eyeColor = QStringLiteral("#f4a261");
    }

    // 5. Art Style Classification
    if (text.contains(QStringLiteral("anime")) || text.contains(QStringLiteral("manga")) ||
        text.contains(QStringLiteral("cel")) || text.contains(QStringLiteral("アニメ")) ||
        text.contains(QStringLiteral("セル画")) || text.contains(QStringLiteral("漫画"))) {
        spec.style = ArtStyle::AnimeCel;
    } else if (text.contains(QStringLiteral("watercolor")) || text.contains(QStringLiteral("aquarelle")) ||
               text.contains(QStringLiteral("水彩")) || text.contains(QStringLiteral("透明水彩"))) {
        spec.style = ArtStyle::Watercolor;
    } else if (text.contains(QStringLiteral("impasto")) || text.contains(QStringLiteral("oil painting")) ||
               text.contains(QStringLiteral("painterly")) || text.contains(QStringLiteral("厚塗り")) ||
               text.contains(QStringLiteral("油絵")) || text.contains(QStringLiteral("油彩"))) {
        spec.style = ArtStyle::Impasto;
    } else if (text.contains(QStringLiteral("fine lineart")) || text.contains(QStringLiteral("fine line")) ||
               text.contains(QStringLiteral("細密画")) || text.contains(QStringLiteral("細密")) ||
               text.contains(QStringLiteral("細かい線画")) || text.contains(QStringLiteral("繊細な線")) ||
               text.contains(QStringLiteral("delicate line")) || text.contains(QStringLiteral("intricate line")) ||
               text.contains(QStringLiteral("極細")) || text.contains(QStringLiteral("ペン画"))) {
        spec.style = ArtStyle::FineLineart;
    } else if (text.contains(QStringLiteral("sketch")) || text.contains(QStringLiteral("ink")) ||
               text.contains(QStringLiteral("hatching")) || text.contains(QStringLiteral("スケッチ")) ||
               text.contains(QStringLiteral("線画"))) {
        spec.style = ArtStyle::InkSketch;
    } else if (text.contains(QStringLiteral("cyber")) || text.contains(QStringLiteral("neon")) ||
               text.contains(QStringLiteral("glowing")) || text.contains(QStringLiteral("ネオン")) ||
               text.contains(QStringLiteral("サイバー"))) {
        spec.style = ArtStyle::CyberNeon;
    } else {
        spec.style = (spec.domain == DomainType::Cyberpunk) ? ArtStyle::CyberNeon :
                     (spec.domain == DomainType::MangaFx) ? ArtStyle::InkSketch :
                     (spec.domain == DomainType::Character) ? ArtStyle::AnimeCel : ArtStyle::General;
    }

    // 6. Color Harmony Computation
    switch (spec.timeOfDay) {
    case TimeOfDay::Day:
        spec.harmony.keyLight = QColor(255, 250, 240);       // Warm sunlight
        spec.harmony.ambientShadow = QColor(45, 55, 80);     // Cool sky ambient
        spec.harmony.accentColor = QColor(255, 110, 130);    // Vibrant coral
        break;
    case TimeOfDay::Sunset:
        spec.harmony.keyLight = QColor(255, 180, 110);       // Golden amber
        spec.harmony.ambientShadow = QColor(60, 30, 70);     // Deep plum/violet
        spec.harmony.accentColor = QColor(255, 230, 130);    // Rim gold
        break;
    case TimeOfDay::Night:
        spec.harmony.keyLight = QColor(160, 200, 255);       // Cool lunar blue
        spec.harmony.ambientShadow = QColor(15, 20, 35);     // Obsidian navy
        spec.harmony.accentColor = QColor(100, 240, 255);    // Cyan bioluminescence
        break;
    case TimeOfDay::Fantasy:
        spec.harmony.keyLight = QColor(240, 190, 255);       // Ethereal lilac
        spec.harmony.ambientShadow = QColor(40, 25, 60);     // Mystical purple
        spec.harmony.accentColor = QColor(255, 150, 220);    // Radiant magenta
        break;
    }

    return spec;
}

QString KisAiPromptAnalyzer::generateArtDirection(
    const SemanticSpec &spec,
    const QSize &canvasSize
)
{
    const qreal minDim = qMin(canvasSize.width(), canvasSize.height());
    const int flatsMin = qMax(40, qRound(minDim * 0.08));
    const int flatsMax = qMax(140, qRound(minDim * 0.20));
    const int formShadMin = qMax(20, qRound(minDim * 0.03));
    const int formShadMax = qMax(55, qRound(minDim * 0.06));

    QString out;
    out += QStringLiteral("=== ART STYLE DIRECTIVE: %1 ===\n").arg(styleName(spec.style));
    out += QStringLiteral("COLOR HARMONY: Key Light (%1) | Ambient Shadow (%2) | Accent (%3)\n\n")
        .arg(spec.harmony.keyLight.name())
        .arg(spec.harmony.ambientShadow.name())
        .arg(spec.harmony.accentColor.name());

    // Composition anchor
    switch (spec.composition) {
    case CompositionType::LandscapeWide:
        out += QStringLiteral(
            "=== SPATIAL COMPOSITION: Landscape Wide (Aspect %1:1) ===\n"
            "Directive: Cinematic panoramic layout. Distribute scenery and secondary landmarks horizontally.\n"
            "Place the primary focal subject around Rule-of-Thirds vertical zones (x=0.33 or x=0.67).\n\n"
        ).arg(spec.aspectRatio, 0, 'f', 2);
        break;
    case CompositionType::PortraitVertical:
        out += QStringLiteral(
            "=== SPATIAL COMPOSITION: Portrait Vertical (Aspect 1:%1) ===\n"
            "Directive: Dynamic vertical layout. Emphasize vertical depth hierarchy from zenith/head at top to foreground ground/torso at bottom.\n"
            "Anchor primary facial/hero landmarks at y=0.30-0.45.\n\n"
        ).arg(1.0 / qMax(0.01, spec.aspectRatio), 0, 'f', 2);
        break;
    case CompositionType::BalancedStandard:
        out += QStringLiteral(
            "=== SPATIAL COMPOSITION: Balanced Harmonious (Aspect %1:1) ===\n"
            "Directive: Strong focal anchor with generous framing and multi-layered circular/triangular flow.\n\n"
        ).arg(spec.aspectRatio, 0, 'f', 2);
        break;
    }

    // Brush sizing guidance
    out += QStringLiteral(
        "=== ADAPTIVE BRUSH SIZES FOR CANVAS (%1x%2) ===\n"
        "- Flats (Base Volumes): %3px to %4px (or size 0.04-0.15 ratio). Leave NO white canvas gaps!\n"
        "- Shading (3D Form & AO): %5px to %6px (size 0.02-0.05) for form shadows; 8px to 20px for crevice AO.\n"
        "- Lineart (Main & Detail): 3.5px to 5.5px (size 0.004-0.007) for main contours.\n"
        "  CRITICAL: Must use 1.5px to 3.0px (size 0.0015-0.003) for exquisite details (eyes, lashes, double eyelids, lips, hair tips)!\n"
        "- Highlights: 2.0px to 3.5px for specular glints; 6px to 14px for rim lighting halos.\n\n"
    ).arg(canvasSize.width()).arg(canvasSize.height())
     .arg(flatsMin).arg(flatsMax)
     .arg(formShadMin).arg(formShadMax);

    // Domain-specific art direction
    switch (spec.domain) {
    case DomainType::Character: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Character / Figure Portrait & Dynamic Figure]\n"
            "GUIDELINES & PRIORITIES:\n"
            "- Strictly derive character gender, age, skin tone, hairstyle, attire, pose, and emotional expression from the USER REQUEST.\n"
            "- Dynamic Staging: Place and orient the character according to prompt intent (full body, dynamic action, profile, high/low angle, or expressive portrait).\n"
            "- STRICT NO RANDOM PARTICLES: Do NOT emit scattered noise dots across faces or bodies. Keep skin, eyes, and hair pristine.\n"
            "- Avoid mechanical 'hatch' across smooth skin surfaces; use soft volumetric 'fill' (brush: watercolor/brush, style: wash or directional, fill_profile: watercolor) for natural curvature.\n"
            "1. Layer 'Flats' (Volumetric Masses & 3D Planes):\n"
            "   - Skin & Anatomy Base: Solid, continuous coverage for head, neck, and exposed anatomy with fill, establishing 3D planes.\n"
            "     Assign explicit ID (e.g. 'face_skin', 'body_base') to enable clip_to_id for all shadows and blush!\n"
            "     Use fill_profile: 'watercolor' for genuine wet-edge pigmentation and soft paper grain.\n"
            "   - Hair Masses: Cohesive primary hair masses (id: 'hair_bangs', 'hair_back', color: %1) using fill or ribbon.\n"
            "     Use ribbon with brush.profile: 'hair' for automatic procedural synthesis of multi-strand locks, flyaways, and halo accents!\n"
            "   - Eyes & Features: Almond sclera discs with fill, or use 'anime_eye' (iris_color: %2, secondary_color, style: 'sparkle').\n"
            "   - Attire & Drapery: Distinct opaque color masses defining garments, folds, and silhouette (e.g. id: 'cloth_base').\n"
            "2. Layer 'Shading' (3D Depth & Volumetric Lighting - MULTIPLY BLEND):\n"
            "   - Form Shadows (Tier 1): Soft gradient transitions across facial curvature (warm-shifted peach/rose shadow tones).\n"
            "     Always specify 'clip_to_id': 'face_skin' to strictly prevent shadow spill outside the face silhouette!\n"
            "   - Cast Shadows (Tier 2): Sharp occlusion shadows under bangs and jawline (clip_to_id: 'face_skin', blend_mode: 'multiply').\n"
            "   - Deep Contact AO: Darkest crevices between overlapping locks and garment creases.\n"
            "3. Layer 'Lineart' (Master Inking - 1.5 to 4.0 px):\n"
            "   - Eyes & Expression: Draw sharp, delicate lash arches, pupil cores, iris details, and double eyelids with gpen or maru_pen, or position 'anime_eye' with iris_color: %2.\n"
            "   - Expressive Contours: Fluid S/C-curves with Catmull-Rom splines for jawline, mouth, and anatomy.\n"
            "   - Hair & Drapery Strands: Individual flowing tapered paths indicating volume and motion.\n"
            "4. Layer 'Highlights' & 'FX' (Specular Vitality):\n"
            "   - Eye Catchlights: Brilliant specular points (#ffffff) inside pupils to convey life.\n"
            "   - Hair Halo Luster: Use 'blend_mode': 'color_dodge' and 'clip_to_id': 'hair_bangs' for intense luminous hair sheen!\n"
            "   - Luminous Polish: Refined specular points on lip gloss, nose tip, and rim lighting along silhouettes.\n"
        ).arg(!spec.hairColor.isEmpty() ? QStringLiteral("'%1'").arg(spec.hairColor) : QStringLiteral("prompt-specified hue"),
             !spec.eyeColor.isEmpty() ? QStringLiteral("'%1'").arg(spec.eyeColor) : QStringLiteral("harmonious eye color"));
        break;
    }
    case DomainType::Landscape: {
        QString skyColorsStr = QStringLiteral("[\"") + spec.skyGradientColors.join(QStringLiteral("\", \"")) + QStringLiteral("\"]");
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Landscape, Scenery & Environment]\n"
            "GUIDELINES & PRIORITIES:\n"
            "- Strictly derive environmental theme (time of day, weather, biome, structures) from the USER REQUEST.\n"
            "- Do not place radial manga focus lines across serene natural skies.\n"
            "- Avoid rigid polygonal shapes for organic forms like mountain crests, clouds, and trees.\n"
            "1. Layer 'Flats' (Depth Horizons & Atmospheric Underpainting):\n"
            "   - Sky / Atmosphere: Use 'gradient_fill' with colors %1 (angle_deg: 90) across background. Leave zero unpainted gaps.\n"
            "   - Distant Horizons: Soft silhouette masses with fill for mountains, cityscapes, or horizons.\n"
            "   - Midground & Foreground Terrain: Solid foundational terrain masses setting the camera perspective.\n"
        ).arg(skyColorsStr);
        if (spec.hasSakura) {
            out += QStringLiteral(
                "   - Foliage / Canopy Masses: Billowing, cloud-like organic clusters using fill.\n"
            );
        }
        out += QStringLiteral(
            "2. Layer 'Shading' (Atmospheric Depth & Form Volumes):\n"
            "   - Atmospheric Haze & Shadows: Gentle wash fills grading non-lit slopes and cloud undersides.\n"
            "   - Occlusion: Deep shadow volumes under terrain overhangs and botanical clusters.\n"
            "3. Layer 'Lineart' (Structural & Natural Contours):\n"
            "   - Limbs / Structures: Tapered ribbon and path strokes defining organic trunks or architectural outlines.\n"
            "   - Edge Silhouettes: Crisp sweeping lines defining focal ridges.\n"
            "4. Layer 'Highlights' & 'FX' (Atmospheric Accents):\n"
            "   - Key Lighting Rim: Luminous rim highlights along sun-facing crests or canopy crowns.\n"
            "   - Atmospheric Particles: Gentle floating embers, dust motes, or petals if matching prompt theme.\n"
        );
        break;
    }
    case DomainType::Cyberpunk: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Cyberpunk City & Sci-Fi Architecture]\n"
            "1. Layer 'Flats': Atmospheric night sky and architectural silhouette masses derived from user prompt.\n"
            "2. Layer 'Shading': Deep crevice ambient occlusion between buildings and misty ground wash (style: 'wash').\n"
            "3. Layer 'Lineart': Sharp perspective lines, structural framework, and window grids (brush: 'gpen').\n"
            "4. Layer 'Highlights' & 'FX': Vivid neon signs, laser accents, and holographic particles derived from prompt palette.\n"
        );
        break;
    }
    case DomainType::Creature: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Creature & Animal Art]\n"
            "1. Layer 'Flats': Organic body volume and fur/scale base silhouette with fill (style: 'wash', color derived from subject).\n"
            "2. Layer 'Shading': Anatomical musculature shadows and tonal gradations with soft wash or directional hatch.\n"
            "3. Layer 'Lineart': Expressive eye contours, ears, snout, paws, and delicate tapering whiskers (brush: 'gpen').\n"
            "4. Layer 'Highlights': Glistening eye glints, fur/scale rim lighting, and moist specular highlights.\n"
        );
        break;
    }
    case DomainType::Botanical: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Botanical & Floral Art]\n"
            "1. Layer 'Flats': Petal and foliage base color masses with fill, matching user-requested species and tones.\n"
            "2. Layer 'Shading': Deep spiral crevice shadows between overlapping petals and leaf clusters with fill (style: 'contour').\n"
            "3. Layer 'Lineart': Graceful curving petal contours, leaf vein networks, and organic stem outlines (brush: 'gpen').\n"
            "4. Layer 'Highlights': Fresh dewdrops with pinpoint specular glints and luminous petal edge rim light.\n"
        );
        break;
    }
    case DomainType::MangaFx: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Manga Effects, Speed Lines & Energy]\n"
            "1. Layer 'Flats': Deep atmospheric or energy backdrop wash.\n"
            "2. Layer 'Shading': Dramatic contrast hatching and shadow cast accents.\n"
            "3. Layer 'Lineart': High-impact radial focus lines (with open center), speed strokes, or geometric runic circles.\n"
            "4. Layer 'Highlights' & 'FX': Vibrant energy sparkles, crackling lightning arcs, and magical bloom particles.\n"
        );
        break;
    }
    case DomainType::General: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Painterly Digital Art]\n"
            "1. Layer 'Flats': Solid opaque foundation blocking every major element strictly from user prompt. Ensure zero white gaps.\n"
            "2. Layer 'Shading': 3D form shadows and contact ambient occlusion with soft wash or directional fill.\n"
            "3. Layer 'Lineart': Structural contours with Catmull-Rom spline curves and tapering pressure dynamics.\n"
            "4. Layer 'Highlights' & 'FX': Strategic focal lighting, specular points, and atmospheric particles.\n"
        );
        break;
    }
    }

    return out;
}

QString KisAiPromptAnalyzer::styleName(ArtStyle style)
{
    switch (style) {
    case ArtStyle::AnimeCel:
        return QStringLiteral("Anime Cel-Shading");
    case ArtStyle::Watercolor:
        return QStringLiteral("Luminous Watercolor");
    case ArtStyle::Impasto:
        return QStringLiteral("Textured Impasto");
    case ArtStyle::InkSketch:
        return QStringLiteral("Manga Ink Sketch");
    case ArtStyle::FineLineart:
        return QStringLiteral("Fine Lineart & Delicate Pen");
    case ArtStyle::CyberNeon:
        return QStringLiteral("Cyberpunk Neon");
    case ArtStyle::General:
    default:
        return QStringLiteral("Painterly Digital");
    }
}

QString KisAiPromptAnalyzer::generateGoalPhaseGuidance(int phase, const SemanticSpec &spec, const QSize &canvasSize, int totalSteps)
{
    Q_UNUSED(canvasSize);
    QString out;
    const QString styleStr = styleName(spec.style);
    out += QStringLiteral("=== GOAL MODE PHASE %1/%2 EXECUTION ===\n").arg(phase).arg(totalSteps);
    out += QStringLiteral("Active Art Style: %1\n").arg(styleStr);
    out += QStringLiteral("Key Light: %1 | Ambient Shadow: %2 | Accent: %3\n\n")
        .arg(spec.harmony.keyLight.name())
        .arg(spec.harmony.ambientShadow.name())
        .arg(spec.harmony.accentColor.name());

    if (totalSteps <= 2) {
        if (phase == 1) {
            out += QStringLiteral(
                "PHASE 1 MISSION: [FOUNDATION, SILHOUETTES & 3D VOLUMES]\n"
                "- Target Layers: 'Background', 'Flats' and 'Shading'.\n"
                "- Establish complete colored base shapes without white canvas gaps, then render volumetric shading.\n"
                "- Form clear silhouette landmarks and key shadow volumes.\n"
            );
        } else {
            out += QStringLiteral(
                "PHASE 2 MISSION: [LINEART, HIGHLIGHTS & FINAL POLISH (COMPLETION)]\n"
                "- Target Layers: 'Lineart', 'Highlights', and 'FX'.\n"
                "- Review canvas image: draw crisp tapering contours, specular glints, eye catchlights, and dynamic particles.\n"
                "- Final Goal Check: Bring illustration to complete presentation readiness.\n"
            );
        }
        return out;
    }

    if (totalSteps == 3) {
        if (phase == 1) {
            out += QStringLiteral(
                "PHASE 1 MISSION: [BACKGROUND & SILHOUETTE FLATS]\n"
                "- Target Layers: 'Background' and 'Flats'.\n"
                "- Establish complete seamless foundation masses with zero white gaps.\n"
            );
        } else if (phase == 2) {
            out += QStringLiteral(
                "PHASE 2 MISSION: [SHADING & STRUCTURAL LINEART]\n"
                "- Target Layers: 'Shading' and 'Lineart'.\n"
                "- Add volumetric shadow depths and crisp contour lines over the base silhouettes.\n"
            );
        } else {
            out += QStringLiteral(
                "PHASE 3 MISSION: [SPECULAR HIGHLIGHTS & FX POLISH (COMPLETION)]\n"
                "- Target Layers: 'Highlights' and 'FX'.\n"
                "- CRITICAL: Do NOT redraw earlier foundation layers ('Background', 'Flats'). Output only new additions.\n"
                "- Specular glints, catchlights, blooming effects, and particles. Bring to 100% completion.\n"
            );
        }
        return out;
    }

    // Default 4-step or extended 5/6-step mapping
    int effectivePhase = 3;
    if (totalSteps == 4) {
        effectivePhase = phase;
    } else if (totalSteps == 5) {
        if (phase <= 1) {
            effectivePhase = 1;
        } else if (phase == 2) {
            effectivePhase = 2;
        } else if (phase == 3) {
            effectivePhase = 3;
        } else {
            effectivePhase = 4;
        }
    } else if (totalSteps >= 6) {
        if (phase <= 2) {
            effectivePhase = 1;
        } else if (phase == 3) {
            effectivePhase = 2;
        } else if (phase == 4) {
            effectivePhase = 3;
        } else {
            effectivePhase = 4;
        }
    } else {
        if (phase >= totalSteps) {
            effectivePhase = 4;
        } else if (phase == 1) {
            effectivePhase = 1;
        } else if (phase == 2) {
            effectivePhase = 2;
        }
    }

    switch (effectivePhase) {
    case 1: // Phase 1: Background & Flats (Silhouette & Base Volumes)
        out += QStringLiteral(
            "PHASE 1 MISSION: [BACKGROUND & SILHOUETTE FLATS]\n"
            "- Target Layers: 'Background' (skies, far distance) and 'Flats' (character masses, skin, hair, clothes).\n"
            "- CRITICAL: Do NOT draw linework, shadows, or highlights yet! Dedicate 100% of geometry to solid base coverage.\n"
            "- Ensure zero white canvas gaps. Large fills and gradient fills should establish seamless foundations.\n"
            "- Reusable Landmark Coordinates: Form clear geometric silhouettes so later Shading and Lineart can register cleanly.\n"
        );
        break;
    case 2: // Phase 2: Shading & Ambient Occlusion
        out += QStringLiteral(
            "PHASE 2 MISSION: [3D SHADING & FORM VOLUMES]\n"
            "- Target Layer: 'Shading' ONLY (rendered with Multiply and clipped to Flats).\n"
            "- Look at the Phase 1 canvas image: locate the key light and cast shadows beneath forms.\n"
            "- Add core form shadows, contact ambient occlusion (AO) under chin/hair/folds, and delicate blush washes.\n"
            "- STRICT: NEVER use 'hatch' on facial features or skin! Use smooth 'fill' with watercolor/brush profiles.\n"
            "- Color Selection: Use cool ambient tones (%1) for shadows to create warm-cool color harmony.\n"
        ).arg(spec.harmony.ambientShadow.name());
        break;
    case 3: // Phase 3: Precision Lineart
        out += QStringLiteral(
            "PHASE 3 MISSION: [PRECISION LINEART & ANATOMICAL DETAIL]\n"
            "- Target Layer: 'Lineart' ONLY (rendered with crisp tapering Catmull-Rom splines).\n"
            "- Look at the canvas image with Flats & Shading in place: draw sharp structural contours and expressive details.\n"
            "- Facial Micro-Details: Upper lash arches, iris rings, pupil cores, subtle double eyelids, delicate nose/lips.\n"
            "- Hair & Fabric: Continuous fluid S-curves for hair clumps and cloth folds. Use gpen or brush profile.\n"
            "- Avoid pure black #000000; use rich deep ink tones like #1c1828 or #241a18.\n"
        );
        break;
    case 4: // Phase 4: Highlights, FX & Polish
    default:
        out += QStringLiteral(
            "PHASE 4 MISSION: [SPECULAR HIGHLIGHTS & POLISH]\n"
            "- Target Layers: 'Highlights' (Screen blend) and 'FX'.\n"
            "- CRITICAL FOUNDATION PRESERVATION: Do NOT output operations for 'Background', 'Flats', or 'Shading'! Foundation layers are already established and cumulatively preserved. Focus 100% of your budget on delicate 'Highlights' and 'FX'.\n"
            "- ACCUMULATIVE ADDITIONS ONLY: Do NOT output a full redrawn artwork. Output ONLY the new finishing operations for this phase.\n"
            "- Specular Glints: Eye catchlights (#ffffff, 2-5px glints on irises), lip glints, nose tip point, hair angel halo rim lighting.\n"
            "- STRICT NO RANDOM PARTICLES / NO STIPPLING: DO NOT emit 'particles' (stars/snow/dots) unless explicitly requested in the prompt! Never spray noise over faces.\n"
            "- STRICT: DO NOT use 'manga_lines' (radial speed lines) unless explicitly requested as an action/battle scene!\n"
            "- Final Goal Check: Bring the illustration to 100% presentation readiness with pristine, clean render quality.\n"
        );
        break;
    }

    return out;
}

QByteArray KisAiPromptAnalyzer::buildPromptExpansionPayload(const QString &shortPrompt, const QString &model, ArtStyle style)
{
    const QString styleInstruction = (style != ArtStyle::General)
        ? QStringLiteral(" Art style emphasis: %1.").arg(styleName(style))
        : QString();

    const QString systemPrompt = QStringLiteral(
        "You are an expert art director and anime illustrator prompt engineer. "
        "Given a concise user prompt, elaborate it into a vivid, descriptive, high-quality prompt in the same language "
        "(use Japanese if input is in Japanese, English if input is in English). "
        "Elaborate on subject features (expression, gaze, hair details, eyes), costume styling, lighting mood (key/fill/rim light), "
        "atmosphere, and color palette.%1 "
        "Output ONLY the elaborated prompt string. Do not include conversational filler, preamble, markdown formatting, or quotes."
    ).arg(styleInstruction);

    QJsonObject systemMessage;
    systemMessage[QStringLiteral("role")] = QStringLiteral("system");
    systemMessage[QStringLiteral("content")] = systemPrompt;

    QJsonObject userMessage;
    userMessage[QStringLiteral("role")] = QStringLiteral("user");
    userMessage[QStringLiteral("content")] = shortPrompt.trimmed();

    QJsonArray messages;
    messages.append(systemMessage);
    messages.append(userMessage);

    QJsonObject payload;
    payload[QStringLiteral("model")] = model.isEmpty() ? QStringLiteral("gpt-4o") : model;
    payload[QStringLiteral("messages")] = messages;
    payload[QStringLiteral("temperature")] = 0.7;
    payload[QStringLiteral("max_tokens")] = 300;

    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QString KisAiPromptAnalyzer::parseExpandedPrompt(const QByteArray &responseBytes, QString *errorMessage)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("JSONパースエラー: %1").arg(parseError.errorString());
        }
        return QString();
    }

    const QJsonObject root = doc.object();
    if (root.contains(QStringLiteral("error"))) {
        const QJsonObject errorObj = root.value(QStringLiteral("error")).toObject();
        const QString msg = errorObj.value(QStringLiteral("message")).toString();
        if (errorMessage) {
            *errorMessage = msg.isEmpty() ? QStringLiteral("APIエラーが発生しました") : msg;
        }
        return QString();
    }

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("応答にchoicesが含まれていません");
        }
        return QString();
    }

    const QJsonObject firstChoice = choices.first().toObject();
    const QJsonObject message = firstChoice.value(QStringLiteral("message")).toObject();
    QString content = message.value(QStringLiteral("content")).toString().trimmed();

    if (content.startsWith(QLatin1Char('"')) && content.endsWith(QLatin1Char('"')) && content.size() >= 2) {
        content = content.mid(1, content.size() - 2).trimmed();
    }

    return content;
}
