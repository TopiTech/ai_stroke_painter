/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPromptAnalyzer.h"

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
    } else if (text.contains(QStringLiteral("sketch")) || text.contains(QStringLiteral("ink")) ||
               text.contains(QStringLiteral("hatching")) || text.contains(QStringLiteral("スケッチ")) ||
               text.contains(QStringLiteral("ペン画")) || text.contains(QStringLiteral("線画"))) {
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
            "[DOMAIN ART DIRECTION: Anime / Manga Character Portrait]\n"
            "1. Layer 'Flats' (Seamless Base Volumes & 3-Layer Hair):\n"
            "   - Skin Base: Clean complete coverage of face, neck, and shoulders with fill (style: 'wash', color: #fff0e6 or #fef2ea).\n"
            "   - Back Hair Mass: Darker deep hair silhouette behind neck with fill or ribbon (color: '%1').\n"
            "   - Sclera Base: Bright white discs (#f8f9fa) under eye sockets.\n"
            "   - Iris Base: Vivid circular/oval color blocks with fill (style: 'radial', color: '%2').\n"
            "   - Front/Side Hair Masses: Volumetric framing clumps with fill or ribbon (color: '%1').\n"
            "2. Layer 'Shading' (3D Facial Planes & 4-Tier Lighting):\n"
            "   - Forehead Cast Shadow: Soft cast shadow directly beneath front bangs onto forehead (#e09f90, opacity: 0.45).\n"
            "   - Eye Socket AO & Sclera Top Shadow: Soft shadow under brow bone and top half of eyeballs for spherical depth.\n"
            "   - Soft Facial Blush: Warm pinkish tint (#ff9fb2, opacity: 0.30) across cheek apples and nose bridge.\n"
            "   - Nose & Subnasal Shadow: Subtle side bridge contour + delicate shadow right beneath nose tip (#d88c7d).\n"
            "   - Lip Groove AO & Upper Lip Tone: Shaded upper lip + dark ambient occlusion under lower lip center.\n"
            "   - Neck Contact AO: Deep contact shadow directly under chin/jawline grading down the neck.\n"
            "   - Hair Lock Crevices: Directional shadow grooves between major hair strands with hatch or directional fill.\n"
            "3. Layer 'Lineart' (EXQUISITE MICRO-DETAILS - Use 1.5 to 3.0 px):\n"
            "   - Jawline Contour: Smooth tapered jaw and chin line with path (brush: 'gpen', color: #231c26).\n"
            "   - Upper Eyelash Arches: Thick, expressive curved strokes with taper (brush: 'gpen', color: #1a1622).\n"
            "   - Lash Flicks & Double Eyelids: Delicate upward flicks at outer corners + subtle crease arch above each eye.\n"
            "   - Iris Contours & Pupil Core: Deep dark pupil center + crisp iris rim contour.\n"
            "   - Nose Tip & Soft Mouth Line: Tiny delicate point/crease for nose + gentle expressive lips line.\n"
            "   - Hair Strands & Flow: Fine tapering strand ribbons and wispy flyaway hairs framing the face.\n"
            "4. Layer 'Highlights' & 'FX':\n"
            "   - Eye Specular Catchlights: Sharp pure white glints (#ffffff, size: 2-4px) at 10 o'clock or 2 o'clock on irises.\n"
            "   - Iris Crescent Glow: Delicate bright curved glow at lower rim of iris for luminous depth.\n"
            "   - Nose & Lip Glints: Tiny pinpoint specular dot on nose tip and lower lip plump center.\n"
            "   - Hair Angel Halo: Luminous rim highlights along top crown and hair ridges.\n"
        ).arg(spec.hairColor, spec.eyeColor);
        break;
    }
    case DomainType::Landscape: {
        QString skyColorsStr = QStringLiteral("[\"") + spec.skyGradientColors.join(QStringLiteral("\", \"")) + QStringLiteral("\"]");
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Landscape, Scenery & Nature]\n"
            "1. Layer 'Flats' (Depth Horizons & Atmospheric Silhouettes):\n"
            "   - Sky Gradient: Use 'gradient_fill' with colors %1 (angle_deg: 90) across upper canvas. Leave NO white gaps!\n"
            "   - Distant Mountain Ridges: Silhouetted peaks with fill (style: 'directional', angle_deg: 25) in soft haze.\n"
            "   - Midground Terrain / Trees: Rich ground wash and tree canopy masses.\n"
        ).arg(skyColorsStr);
        if (spec.hasSakura) {
            out += QStringLiteral(
                "   - Sakura Blossom Canopies: Puffy billowing foliage masses using fill (style: 'contour') with colors #ffb8cd, #ffd6e5.\n"
            );
        }
        out += QStringLiteral(
            "2. Layer 'Shading' (3D Strata & Ridge Facets):\n"
            "   - Mountain Shading: Dramatic crag shadow facets along ridge lines using hatch or directional fill.\n"
            "   - Cloud Undersides: Soft purplish shadow bulges beneath puffy clouds (style: 'contour', opacity: 0.5-0.7).\n"
            "   - Foliage Deep Shadow: Core occlusion beneath tree branches and canopy clumps.\n"
            "3. Layer 'Lineart' (Organic Contours & Tree Anatomy):\n"
            "   - Tree Trunks & Branches: Twisting organic limbs using ribbon or path with S-curves (brush: 'gpen', color: #342017).\n"
            "   - Mountain Crests & Cloud Edges: Crisp jagged ridge lines and billowing cloud rims.\n"
            "4. Layer 'Highlights' & 'FX':\n"
            "   - Sunlight Rim Lighting: Pure luminous highlights on sunny ridge crests and cloud tops.\n"
            "   - Atmospheric Accents: Use 'particles' (shape: 'petal' or 'sparkle') drifting through foreground.\n"
        );
        break;
    }
    case DomainType::Cyberpunk: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Cyberpunk City & Sci-Fi Architecture]\n"
            "1. Layer 'Flats': Dark atmospheric sky (#0a0e17) and monolithic skyscraper silhouette masses (#131b2e, #1a233a).\n"
            "2. Layer 'Shading': Deep crevice occlusion between building blocks and misty ground wash (style: 'wash').\n"
            "3. Layer 'Lineart': Sharp architectural edges, perspective vanishing lines, window grid frames (brush: 'gpen').\n"
            "4. Layer 'Highlights' & 'FX': Luminous neon signs, glowing window matrices, laser light beams, holographic particles (#00f0ff, #ff007f, #ffe600).\n"
        );
        break;
    }
    case DomainType::Creature: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Creature & Animal Art]\n"
            "1. Layer 'Flats': Organic body volume and fur/scale base silhouette with fill (style: 'wash').\n"
            "2. Layer 'Shading': Anatomical musculature shadows and fur tone gradations with hatch (style: 'directional').\n"
            "3. Layer 'Lineart': Expressive eye contours, ears, snout, paws, and delicate tapering whiskers (brush: 'gpen').\n"
            "4. Layer 'Highlights': Glistening eye glints, fur rim lighting, wet nose specular highlight (#ffffff).\n"
        );
        break;
    }
    case DomainType::Botanical: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Botanical & Floral Art]\n"
            "1. Layer 'Flats': Petal base color masses (#e63946, #ff758f, #ffd6e5) and stem/leaf volumes (#2d6a4f, #52b788).\n"
            "2. Layer 'Shading': Deep spiral crevice shadows between overlapping petals with fill (style: 'contour').\n"
            "3. Layer 'Lineart': Graceful curving petal contours, leaf vein networks, thorny stem outlines (brush: 'gpen').\n"
            "4. Layer 'Highlights': Fresh morning dewdrops with pinpoint specular glints and luminous petal edge rim light.\n"
        );
        break;
    }
    case DomainType::MangaFx: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Manga Effects, Speed Lines & Runes]\n"
            "1. Layer 'Flats': Deep atmospheric or energy backdrop wash.\n"
            "2. Layer 'Shading': Dramatic contrast hatching and shadow cast accents.\n"
            "3. Layer 'Lineart': High-impact radial focus lines (with open center), speed strokes, or geometric runic circles.\n"
            "4. Layer 'Highlights' & 'FX': Vibrant energy sparkles, crackling lightning arcs, magical bloom particles.\n"
        );
        break;
    }
    case DomainType::General: {
        out += QStringLiteral(
            "[DOMAIN ART DIRECTION: Painterly Digital Art]\n"
            "1. Layer 'Flats': Solid opaque foundation blocking every major element. Ensure zero white gaps.\n"
            "2. Layer 'Shading': 3D form shadows and contact ambient occlusion with hatch or wash.\n"
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
    case ArtStyle::CyberNeon:
        return QStringLiteral("Cyberpunk Neon");
    case ArtStyle::General:
    default:
        return QStringLiteral("Painterly Digital");
    }
}

QString KisAiPromptAnalyzer::generateGoalPhaseGuidance(int phase, const SemanticSpec &spec, const QSize &canvasSize)
{
    QString out;
    const QString styleStr = styleName(spec.style);
    out += QStringLiteral("=== GOAL MODE PHASE %1 EXECUTION ===\n").arg(phase);
    out += QStringLiteral("Active Art Style: %1\n").arg(styleStr);
    out += QStringLiteral("Key Light: %1 | Ambient Shadow: %2 | Accent: %3\n\n")
        .arg(spec.harmony.keyLight.name())
        .arg(spec.harmony.ambientShadow.name())
        .arg(spec.harmony.accentColor.name());

    switch (phase) {
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
            "- Add core form shadows, contact ambient occlusion (AO) under chin/hair/folds, and delicate blush/hatch tones.\n"
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
            "PHASE 4 MISSION: [SPECULAR HIGHLIGHTS, MANGA LINES & FX POLISH]\n"
            "- Target Layers: 'Highlights' (Screen blend) and 'FX' (Particles, MangaLines).\n"
            "- Specular Glints: Eye catchlights (#ffffff), lip glints, nose tip point, hair angel halo rim lighting.\n"
            "- Dynamic FX: Floating petals, embers, stars, or manga_lines (speed/focus lines) directed toward the focal anchor.\n"
            "- Final Goal Check: Bring the illustration to 100% presentation readiness.\n"
        );
        break;
    }

    return out;
}
