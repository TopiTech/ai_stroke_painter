/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PROMPT_ANALYZER_H
#define KIS_AI_PROMPT_ANALYZER_H

#include <QColor>
#include <QSize>
#include <QString>
#include <QVector>

#ifdef AI_STROKE_STANDALONE
#define KRITAUI_EXPORT
#else
#include "kritaui_export.h"
#endif

/**
 * Semantic prompt analyzer that extracts subject motifs, environment lighting,
 * composition spatial anchors, and domain-specific art direction from natural language prompts.
 * Ported and enhanced from the Python plugin prompt_analyzer and llm_planner architectures.
 */
class KRITAUI_EXPORT KisAiPromptAnalyzer
{
public:
    enum class DomainType {
        Character,   // Anime/manga portraits, characters, faces, eyes, hair
        Landscape,   // Mountains, sakura trees, clouds, ocean, nature
        Cyberpunk,   // Futuristic city, skyscrapers, neon, sci-fi
        Creature,    // Animals, cats, dogs, birds, dragons
        Botanical,   // Flowers, roses, bouquets, petals, plants
        MangaFx,     // Focus lines, speed lines, magic circles, runes
        General      // General painting
    };

    enum class TimeOfDay {
        Day,
        Sunset,
        Night,
        Fantasy
    };

    enum class CompositionType {
        LandscapeWide,   // Aspect >= 1.25
        PortraitVertical,// Aspect <= 0.80
        BalancedStandard // 0.80 < Aspect < 1.25
    };

    enum class ArtStyle {
        General,
        AnimeCel,    // Crisp cel-shading, anime linework, flat highlights
        Watercolor,  // Soft washes, wet fringes, bleeding edges
        Impasto,     // Rich textured paint, heavy shading, dramatic contrast
        InkSketch,   // Hatching lines, manga ink, monochrome or subtle tint
        CyberNeon    // High-contrast neon glows, dark backdrop, electric accents
    };

    struct ColorHarmony {
        QColor keyLight {QColor(255, 250, 240)};
        QColor ambientShadow {QColor(35, 40, 60)};
        QColor accentColor {QColor(255, 100, 130)};
    };

    struct SemanticSpec {
        DomainType domain {DomainType::General};
        TimeOfDay timeOfDay {TimeOfDay::Day};
        CompositionType composition {CompositionType::BalancedStandard};
        ArtStyle style {ArtStyle::General};
        ColorHarmony harmony;
        qreal aspectRatio {1.0};
        bool hasSakura {false};
        bool hasCharacter {false};
        bool hasEnvironment {false};
        QString hairColor {QStringLiteral("#2d2036")};
        QString eyeColor {QStringLiteral("#3884ff")};
        QVector<QString> skyGradientColors;
        QString domainGuidance;
    };

    /**
     * Analyze a prompt and canvas dimensions to produce a rich semantic specification.
     */
    static SemanticSpec analyze(const QString &prompt, const QSize &canvasSize);

    /**
     * Generate the complete artistic direction text for system prompt injection.
     */
    static QString generateArtDirection(const SemanticSpec &spec, const QSize &canvasSize);

    /**
     * Generate phase-specific guidance for Goal Mode (Phase 1: Blocking, Phase 2: Shading, Phase 3: Lineart, Phase 4: Finishing).
     */
    static QString generateGoalPhaseGuidance(int phase, const SemanticSpec &spec, const QSize &canvasSize);

    /**
     * Human-readable label for an ArtStyle.
     */
    static QString styleName(ArtStyle style);
};

#endif // KIS_AI_PROMPT_ANALYZER_H
