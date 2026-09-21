/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiLineartModeTest.h"

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
#include "aiillustration/KisAiPromptAnalyzer.h"
#include "aiillustration/KisAiRigLibrary.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeRenderer.h"

using namespace QTest;

void KisAiLineartModeTest::testLineartPromptDetection()
{
    const QSize canvas(1024, 1024);

    {
        const auto spec = KisAiPromptAnalyzer::analyze(QStringLiteral("女の子の線画、高精細なペンタッチ"), canvas);
        QCOMPARE(spec.style, KisAiPromptAnalyzer::ArtStyle::PureLineart);
    }
    {
        const auto spec = KisAiPromptAnalyzer::analyze(QStringLiteral("アニメ美少女の塗り絵、きれいな主線"), canvas);
        QCOMPARE(spec.style, KisAiPromptAnalyzer::ArtStyle::PureLineart);
    }
    {
        const auto spec = KisAiPromptAnalyzer::analyze(QStringLiteral("clean lineart anime girl coloring book"), canvas);
        QCOMPARE(spec.style, KisAiPromptAnalyzer::ArtStyle::PureLineart);
    }
    {
        const auto spec = KisAiPromptAnalyzer::analyze(QStringLiteral("manga inking line art portrait"), canvas);
        QCOMPARE(spec.style, KisAiPromptAnalyzer::ArtStyle::PureLineart);
    }
}

void KisAiLineartModeTest::testPureLineartArtDirection()
{
    KisAiPromptAnalyzer::SemanticSpec spec;
    spec.domain = KisAiPromptAnalyzer::DomainType::Character;
    spec.style = KisAiPromptAnalyzer::ArtStyle::PureLineart;

    const QString artDir = KisAiPromptAnalyzer::generateArtDirection(spec, QSize(1024, 1024));
    QVERIFY2(artDir.contains(QStringLiteral("PURE LINE ART")), "Art direction must announce pure lineart");
    QVERIFY2(artDir.contains(QStringLiteral("ZERO-TOLERANCE ON COLORED FILLS")), "Must prohibit colored fills in Flats layer");
    QVERIFY2(artDir.contains(QStringLiteral("PURE WHITE CANVAS")), "Must mandate solid white canvas");
    QVERIFY2(artDir.contains(QStringLiteral("G-PEN vs MARU-PEN")), "Must prescribe line weight hierarchy");
    QVERIFY2(artDir.contains(QStringLiteral("ANIME EYE INKING SPECIFICATION")), "Must mandate uncolored anime eye contour architecture");
}

void KisAiLineartModeTest::testPureLineartGoalPhases()
{
    KisAiPromptAnalyzer::SemanticSpec spec;
    spec.style = KisAiPromptAnalyzer::ArtStyle::PureLineart;
    const QSize canvas(1024, 1024);

    // 4-phase goal mode validation
    const QString p1 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(1, spec, canvas, 4);
    QVERIFY2(p1.contains(QStringLiteral("FOUNDATIONAL SILHOUETTE")), "Phase 1 must establish foundation silhouette");
    QVERIFY2(p1.contains(QStringLiteral("ZERO colored fills")), "Phase 1 must enforce zero color fills");

    const QString p2 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(2, spec, canvas, 4);
    QVERIFY2(p2.contains(QStringLiteral("HAIR CLUSTERS")), "Phase 2 must define hair clusters");

    const QString p3 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(3, spec, canvas, 4);
    QVERIFY2(p3.contains(QStringLiteral("EYE INKING")), "Phase 3 must inscribe eye inking and details");

    const QString p4 = KisAiPromptAnalyzer::generateGoalPhaseGuidance(4, spec, canvas, 4);
    QVERIFY2(p4.contains(QStringLiteral("CORNER FILLETS & HATCHING")), "Phase 4 must add line weight modulation and fillets");
}

void KisAiLineartModeTest::testLineartProgramGeneration()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("美しいアニメ風美少女の繊細な線画、ペン画");
    spec.style.artStyleId = QStringLiteral("pure_lineart");
    spec.composition.headCenter = QPointF(0.5, 0.38);
    spec.composition.headHeight = 0.42;

    const QVector<KisAiStrokeOperation> ops = KisAiLayoutEngine::lineartProgram(spec, QSize(1024, 1024));
    QVERIFY(!ops.isEmpty());

    // 1. Must have Background white canvas
    bool hasWhiteCanvas = false;
    int flatsFillCount = 0;
    int lineartPathCount = 0;
    bool hasJawContour = false;
    bool hasEyeLash = false;
    bool hasEyeIrisContour = false;
    bool hasEyeCatch = false;
    bool hasNeckOrClothing = false;

    for (const KisAiStrokeOperation &op : ops) {
        const QString normLayer = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        if (normLayer == QLatin1String("Background") && op.id == QLatin1String("white_canvas")) {
            hasWhiteCanvas = true;
        }
        if (normLayer == QLatin1String("Flats") && op.kind == KisAiStrokeOperation::Kind::Fill) {
            flatsFillCount++;
        }
        if (normLayer == QLatin1String("Lineart") && op.kind == KisAiStrokeOperation::Kind::Path) {
            lineartPathCount++;
            if (op.id.contains(QStringLiteral("jaw"))) hasJawContour = true;
            if (op.id.contains(QStringLiteral("lash"))) hasEyeLash = true;
            if (op.id.contains(QStringLiteral("iris_contour"))) hasEyeIrisContour = true;
            if (op.id.contains(QStringLiteral("catch"))) hasEyeCatch = true;
            if (op.id.contains(QStringLiteral("neck")) || op.id.contains(QStringLiteral("cloth"))) hasNeckOrClothing = true;
        }
    }

    QVERIFY2(hasWhiteCanvas, "Lineart mode must provide solid white background canvas");
    QCOMPARE(flatsFillCount, 0); // Strict zero colored fills check
    QVERIFY2(lineartPathCount >= 15, "Must generate rich set of inking paths");
    QVERIFY2(hasJawContour, "Must include jaw contour lineart");
    QVERIFY2(hasEyeLash, "Must include eye lash inking");
    QVERIFY2(hasEyeIrisContour, "Must include uncolored iris contour line");
    QVERIFY2(hasEyeCatch, "Must include circular catchlight outline ring");
    QVERIFY2(hasNeckOrClothing, "Must include neck or clothing lines");
}

void KisAiLineartModeTest::testLineartEyeAssembly()
{
    KisAiRigParameterSet rig;
    rig.eyeLeft.aperture = 1.0;
    rig.eyeRight.aperture = 1.0;
    rig.eyeLeft.doubleLid = true;
    rig.eyeRight.doubleLid = true;

    const QVector<KisAiStrokeOperation> eyeOps = KisAiRigLibrary::eyePairLineartOps(rig);
    QVERIFY(eyeOps.size() >= 10);

    for (const KisAiStrokeOperation &op : eyeOps) {
        QCOMPARE(op.kind, KisAiStrokeOperation::Kind::Path);
        QCOMPARE(op.layer, QStringLiteral("Lineart"));
        // All strokes must be dark ink
        QVERIFY(op.brush.color.red() < 60);
        QVERIFY(op.brush.color.green() < 60);
        QVERIFY(op.brush.color.blue() < 60);
    }
}

void KisAiLineartModeTest::testGenerateProgramAutomaticLineartRouting()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("銀髪美少女の線画、塗り絵用");
    spec.canvasSize = QSize(1024, 1024);

    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, spec.canvasSize);
    QVERIFY(!prog.operations.isEmpty());

    int flatsFillCount = 0;
    int lineartCount = 0;
    for (const auto &op : prog.operations) {
        const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        if (lName == QLatin1String("Flats") && op.kind == KisAiStrokeOperation::Kind::Fill) {
            flatsFillCount++;
        }
        if (lName == QLatin1String("Lineart")) {
            lineartCount++;
        }
    }

    QCOMPARE(flatsFillCount, 0); // Automatic routing to lineartProgram ensures zero flats fills
    QVERIFY(lineartCount >= 10);
}

void KisAiLineartModeTest::testLineartRenderingExecution()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("美少女キャラクターの線画");
    spec.style.artStyleId = QStringLiteral("pure_lineart");
    const QSize size(512, 512);

    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, size);
    QVERIFY(!prog.operations.isEmpty());

    // Rasterize strokes to image
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, size);
    QVERIFY(!img.isNull());

    // Verify non-white dark ink pixels are drawn on the white canvas
    int darkPixelCount = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *scanline = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb pixel = scanline[x];
            if (qRed(pixel) < 100 && qGreen(pixel) < 100 && qBlue(pixel) < 100) {
                darkPixelCount++;
            }
        }
    }

    QVERIFY2(darkPixelCount > 500, "Rendered lineart must produce substantive dark ink strokes on canvas");

    // Save visual preview to artifact directory
    img.save(QStringLiteral("C:/Users/mibu0/.gemini/antigravity-ide/brain/6a6d55ca-d3d9-493e-8865-b5ad3c6ee74f/lineart_preview.png"));
}

KISTEST_MAIN(KisAiLineartModeTest)
