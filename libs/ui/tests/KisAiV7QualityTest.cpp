/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiV7QualityTest.h"

#include <QImage>
#include <QPainter>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiLightRig.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"
#include "aiillustration/KisAiStrokeRenderer.h"

#include <cmath>

using namespace QTest;

void KisAiV7QualityTest::testBezierHeadCurvature()
{
    const QPointF center(0.5, 0.38);
    const qreal width = 0.33;
    const qreal height = 0.42;

    const QPolygonF untilted = KisAiRigLibrary::headOutlineBezier(center, width, height, 0.0);
    QVERIFY(untilted.size() >= 36);

    // Untilted contour must be symmetric across center.x()
    for (int i = 0; i < untilted.size(); ++i) {
        const QPointF &p = untilted.at(i);
        bool hasMirror = false;
        for (int j = 0; j < untilted.size(); ++j) {
            const QPointF &other = untilted.at(j);
            if (std::abs((p.x() - center.x()) + (other.x() - center.x())) < 0.005
                && std::abs(p.y() - other.y()) < 0.005) {
                hasMirror = true;
                break;
            }
        }
        QVERIFY2(hasMirror, "Untilted head contour must maintain bilateral symmetry");
    }

    // Tilted contour should rotate points around center
    const QPolygonF tilted = KisAiRigLibrary::headOutlineBezier(center, width, height, 10.0);
    QCOMPARE(tilted.size(), untilted.size());
    QVERIFY(tilted.at(0) != untilted.at(0));
}

void KisAiV7QualityTest::testHierarchicalHairClumpStructure()
{
    KisAiRigParameterSet params;
    params.headCenter = QPointF(0.5, 0.38);
    params.headWidth = 0.33;
    params.headHeight = 0.42;
    params.hairColor = QColor(60, 50, 85);
    params.lineColor = QColor(25, 20, 35);

    const auto clumpOps = KisAiRigLibrary::hierarchicalHairClumpOps(params, QSize(512, 512), 42);
    QVERIFY(!clumpOps.isEmpty());

    int flatsCount = 0;
    int shadingCount = 0;
    int lineartCount = 0;

    for (const auto &op : clumpOps) {
        if (op.layer == QLatin1String("Flats")) {
            ++flatsCount;
            QVERIFY(op.kind == KisAiStrokeOperation::Kind::Fill);
            QVERIFY(op.polygon.size() >= 3);
        } else if (op.layer == QLatin1String("Shading")) {
            ++shadingCount;
            QVERIFY(op.kind == KisAiStrokeOperation::Kind::Fill);
            QVERIFY(op.id.contains(QLatin1String("clump_shadow")));
        } else if (op.layer == QLatin1String("Lineart")) {
            ++lineartCount;
            QVERIFY(op.kind == KisAiStrokeOperation::Kind::Path);
            QVERIFY(op.points.size() >= 3);
        }
    }

    QVERIFY(flatsCount >= 5);
    QVERIFY(shadingCount >= 5);
    QVERIFY(lineartCount >= 5);
}

void KisAiV7QualityTest::testVolumetricShadingConsistency()
{
    KisAiLightSettings rig;
    rig.direction = QPointF(-0.5, -0.7); // Light from top-left
    rig.timeOfDay = QStringLiteral("day");

    KisAiStrokeOperation skinMass;
    skinMass.kind = KisAiStrokeOperation::Kind::Fill;
    skinMass.id = QStringLiteral("face_skin");
    skinMass.layer = QStringLiteral("Flats");
    skinMass.brush.color = QColor(255, 224, 192);
    skinMass.polygon << QPointF(0.35, 0.25) << QPointF(0.65, 0.25) << QPointF(0.50, 0.55);

    QVector<KisAiStrokeOperation> flatsOps = {skinMass};
    const auto vOps = KisAiLightRig::synthesizeVolumetricShading(flatsOps, rig, QSize(512, 512));
    QVERIFY(!vOps.isEmpty());

    bool hasFormShading = false;
    bool hasCastDeep = false;
    for (const auto &op : vOps) {
        if (op.id.contains(QLatin1String("v7_form_shading"))) {
            hasFormShading = true;
            QCOMPARE(op.layer, QStringLiteral("Shading"));
            QVERIFY(op.polygon.size() >= 3);
        }
        if (op.id.contains(QLatin1String("v7_cast_deep"))) {
            hasCastDeep = true;
            QCOMPARE(op.layer, QStringLiteral("Shading"));
        }
    }
    QVERIFY(hasFormShading);
    QVERIFY(hasCastDeep);
}

void KisAiV7QualityTest::testMaterialOpticsSssAndSheen()
{
    KisAiLightSettings rig;
    rig.direction = QPointF(-0.5, -0.7);
    rig.timeOfDay = QStringLiteral("sunset");

    KisAiStrokeOperation skinOp;
    skinOp.kind = KisAiStrokeOperation::Kind::Fill;
    skinOp.id = QStringLiteral("face_skin");
    skinOp.polygon << QPointF(0.35, 0.25) << QPointF(0.65, 0.25) << QPointF(0.50, 0.55);

    KisAiStrokeOperation hairOp;
    hairOp.kind = KisAiStrokeOperation::Kind::Fill;
    hairOp.id = QStringLiteral("hair_back");
    hairOp.polygon << QPointF(0.30, 0.15) << QPointF(0.70, 0.15) << QPointF(0.70, 0.60) << QPointF(0.30, 0.60);

    KisAiLightRig::HeadAnchor anchor;
    anchor.headCenter = QPointF(0.5, 0.38);
    anchor.headWidth = 0.33;
    anchor.headHeight = 0.42;

    QVector<KisAiStrokeOperation> flats = {skinOp, hairOp};
    const auto optics = KisAiLightRig::synthesizeMaterialOptics(flats, rig, QSize(512, 512), &anchor);
    QVERIFY(!optics.isEmpty());

    bool hasSss = false;
    bool hasSheen = false;
    for (const auto &op : optics) {
        if (op.id.contains(QLatin1String("terminator_sss"))) {
            hasSss = true;
            QCOMPARE(op.layer, QStringLiteral("Shading"));
        }
        if (op.id.contains(QLatin1String("anisotropic_sheen"))) {
            hasSheen = true;
            QCOMPARE(op.layer, QStringLiteral("Highlights"));
            QCOMPARE(op.blendMode, QStringLiteral("screen"));
        }
    }
    QVERIFY(hasSss);
    QVERIFY(hasSheen);
}

void KisAiV7QualityTest::testArtStylePipelineSwitching()
{
    KisAiStrokeOperation fillOp;
    fillOp.kind = KisAiStrokeOperation::Kind::Fill;
    fillOp.layer = QStringLiteral("Flats");
    fillOp.brush.profile = QStringLiteral("brush");

    KisAiStrokeOperation lineOp;
    lineOp.kind = KisAiStrokeOperation::Kind::Path;
    lineOp.layer = QStringLiteral("Lineart");
    lineOp.brush.profile = QStringLiteral("gpen");
    lineOp.brush.size = 0.005;

    QVector<KisAiStrokeOperation> ops = {fillOp, lineOp};

    // 1. Test Watercolor
    KisAiSceneStyleV2 wcStyle;
    wcStyle.artStyleId = QStringLiteral("watercolor");
    auto wcOps = ops;
    KisAiLayoutEngine::applyArtStylePipeline(wcOps, wcStyle);
    QCOMPARE(wcOps[0].brush.profile, QStringLiteral("watercolor"));
    QCOMPARE(wcOps[0].fillStyle, QStringLiteral("wash"));
    QCOMPARE(wcOps[1].brush.profile, QStringLiteral("pencil"));

    // 2. Test Impasto
    KisAiSceneStyleV2 impStyle;
    impStyle.artStyleId = QStringLiteral("impasto");
    auto impOps = ops;
    KisAiLayoutEngine::applyArtStylePipeline(impOps, impStyle);
    QCOMPARE(impOps[0].brush.profile, QStringLiteral("brush"));
    QVERIFY(impOps[1].brush.size > lineOp.brush.size);

    // 3. Test Cyber Neon
    KisAiStrokeOperation fxOp;
    fxOp.layer = QStringLiteral("Highlights");
    auto neonOps = QVector<KisAiStrokeOperation>{fxOp};
    KisAiSceneStyleV2 neonStyle;
    neonStyle.artStyleId = QStringLiteral("cyber_neon");
    KisAiLayoutEngine::applyArtStylePipeline(neonOps, neonStyle);
    QCOMPARE(neonOps[0].brush.profile, QStringLiteral("neon"));
    QCOMPARE(neonOps[0].blendMode, QStringLiteral("screen"));
}

void KisAiV7QualityTest::testStrokeBeautifierTaperAndSmoothing()
{
    QVector<KisAiStrokePoint> raw;
    raw << KisAiStrokePoint(0.10, 0.10, 0.8)
        << KisAiStrokePoint(0.101, 0.102, 0.8) // Micro jitter
        << KisAiStrokePoint(0.20, 0.18, 0.8)
        << KisAiStrokePoint(0.35, 0.28, 0.8)
        << KisAiStrokePoint(0.50, 0.40, 0.8);

    const auto smoothed = KisAiStrokeQualityUtils::stabilizeAndBeautifyStroke(raw, false);
    QVERIFY(smoothed.size() >= 3);

    // Entrance and exit must be properly tapered (< baseline 0.8)
    QVERIFY(smoothed.first().pressure < 0.60);
    QVERIFY(smoothed.last().pressure < 0.60);
}

void KisAiV7QualityTest::testCornerInkingFillet()
{
    // Right angle corner (90 deg)
    const QPointF pPrev(10.0, 50.0);
    const QPointF pCurr(50.0, 50.0);
    const QPointF pNext(50.0, 90.0);

    const QPolygonF fillet = KisAiStrokeQualityUtils::generateCornerInkingPolygon(pPrev, pCurr, pNext, 4.0);
    QVERIFY(!fillet.isEmpty());
    QVERIFY(fillet.size() >= 3);

    // Flat line (180 deg) should not produce inking fillet
    const QPointF pFlatNext(90.0, 50.0);
    const QPolygonF noFillet = KisAiStrokeQualityUtils::generateCornerInkingPolygon(pPrev, pCurr, pFlatNext, 4.0);
    QVERIFY(noFillet.isEmpty());
}

void KisAiV7QualityTest::testLineartOcclusionWeighting()
{
    KisAiStrokeOperation shadowLine;
    shadowLine.kind = KisAiStrokeOperation::Kind::Path;
    shadowLine.layer = QStringLiteral("Lineart");
    shadowLine.brush.size = 0.005;
    // Downward line: left normal points towards (-1, 0)
    shadowLine.points << KisAiStrokePoint(0.5, 0.2, 0.8) << KisAiStrokePoint(0.5, 0.8, 0.8);

    QVector<KisAiStrokeOperation> ops = {shadowLine};
    // Key light pointing left (-1, 0) means normal facing right is shadow side
    KisAiStrokeQualityUtils::applyLineartOcclusionWeights(ops, QPointF(1.0, 0.0));
    QVERIFY(ops[0].brush.size > 0.005);
}

void KisAiV7QualityTest::testDraperyFoldsSynthesis()
{
    const QPointF origin(0.3, 0.4);
    const QPointF target(0.7, 0.4);
    const QColor cloth(50, 60, 90);
    const QColor shadow(30, 35, 55);

    const auto folds = KisAiRigLibrary::draperyFoldOps(origin, target, 2.0, cloth, shadow);
    QVERIFY(folds.size() >= 2);

    bool hasLineart = false;
    bool hasShading = false;
    for (const auto &op : folds) {
        if (op.layer == QLatin1String("Lineart")) hasLineart = true;
        if (op.layer == QLatin1String("Shading")) hasShading = true;
    }
    QVERIFY(hasLineart);
    QVERIFY(hasShading);
}

KISTEST_MAIN(KisAiV7QualityTest)
