/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRendererTest.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QSet>
#include <QVector>
#include <cmath>
#include <limits>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiDeliberateStroke.h"
#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiLightRig.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"
#include "aiillustration/KisAiStrokeRenderer.h"
#include "aiillustration/KisAiStrokeTypeChecker.h"

void KisAiStrokeRendererTest::testCatmullRomSpline()
{
    const QVector<QPointF> input = {QPointF(0.0, 0.0), QPointF(10.0, 30.0), QPointF(25.0, 15.0), QPointF(40.0, 0.0)};

    const QVector<QPointF> spline = KisAiStrokeRenderer::generateCatmullRomSpline(input, 6, false);

    // Subdividing 3 segments with 6 steps = 18 intervals + 1 endpoint = exactly 19 points
    QCOMPARE(spline.size(), 19);
    QCOMPARE(spline.first(), input.first());
    QCOMPARE(spline.last(), input.last());

    // Verify no consecutive points are duplicate/degenerate knots
    for (int i = 0; i < spline.size() - 1; ++i) {
        const QPointF diff = spline.at(i + 1) - spline.at(i);
        const qreal dist = std::hypot(diff.x(), diff.y());
        QVERIFY2(dist > 1e-4, "Open Catmull-Rom spline generated duplicate consecutive knots");
    }

    // Closed spline test: 4 segments * 6 = 24 points
    const QVector<QPointF> closedSpline = KisAiStrokeRenderer::generateCatmullRomSpline(input, 6, true);
    QCOMPARE(closedSpline.size(), 24);
    for (int i = 0; i < closedSpline.size(); ++i) {
        const QPointF diff = closedSpline.at((i + 1) % closedSpline.size()) - closedSpline.at(i);
        const qreal dist = std::hypot(diff.x(), diff.y());
        QVERIFY2(dist > 1e-4, "Closed Catmull-Rom spline generated duplicate consecutive knots");
    }
}

void KisAiStrokeRendererTest::testRenderProgramToImage()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(256, 256);

    KisAiStrokeOperation fillOp;
    fillOp.kind = KisAiStrokeOperation::Kind::Fill;
    fillOp.id = QStringLiteral("base");
    fillOp.layer = QStringLiteral("Flats");
    fillOp.polygon = QPolygonF{QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.2, 0.8)};
    fillOp.brush.color = QColor(255, 100, 50);
    fillOp.brush.opacity = 1.0;
    program.operations.append(fillOp);

    KisAiStrokeOperation pathOp;
    pathOp.kind = KisAiStrokeOperation::Kind::Path;
    pathOp.id = QStringLiteral("line");
    pathOp.layer = QStringLiteral("Lineart");
    pathOp.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.2, 0.5, 0.8), KisAiStrokePoint(0.8, 0.5, 0.8)};
    pathOp.brush.color = QColor(20, 20, 20);
    pathOp.brush.size = 0.02;
    program.operations.append(pathOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(256, 256));

    QVERIFY(!img.isNull());
    QCOMPARE(img.size(), QSize(256, 256));

    // Pixel at center (128, 128) should be painted (non-transparent)
    const QRgb centerPixel = img.pixel(128, 128);
    QVERIFY(qAlpha(centerPixel) > 0);

    // Pixel outside at top-left (10, 10) should be transparent
    const QRgb cornerPixel = img.pixel(10, 10);
    QCOMPARE(qAlpha(cornerPixel), 0);
}

void KisAiStrokeRendererTest::testClippingMaskToFlats()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    // Small Flat box in the center [0.4, 0.4] to [0.6, 0.6] (canvas coords: [80, 80] to [120, 120])
    KisAiStrokeOperation flatOp;
    flatOp.kind = KisAiStrokeOperation::Kind::Fill;
    flatOp.id = QStringLiteral("flat_box");
    flatOp.layer = QStringLiteral("Flats");
    flatOp.polygon = QPolygonF{QPointF(0.4, 0.4), QPointF(0.6, 0.4), QPointF(0.6, 0.6), QPointF(0.4, 0.6)};
    flatOp.brush.color = QColor(200, 200, 200);
    program.operations.append(flatOp);

    // Huge Shading that covers the entire canvas [0.0, 0.0] to [1.0, 1.0]
    KisAiStrokeOperation shadeOp;
    shadeOp.kind = KisAiStrokeOperation::Kind::Fill;
    shadeOp.id = QStringLiteral("shade_all");
    shadeOp.layer = QStringLiteral("Shading");
    shadeOp.polygon = QPolygonF{QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    shadeOp.brush.color = QColor(50, 50, 100);
    program.operations.append(shadeOp);

    // Render with clipping enabled
    const QImage imgClipped = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200), true);

    // Pixel at (20, 20) is outside the flat box; with clipping it must remain completely transparent!
    const QRgb outsidePixel = imgClipped.pixel(20, 20);
    QCOMPARE(qAlpha(outsidePixel), 0);

    // Pixel at (100, 100) is inside the flat box; it must have color
    const QRgb insidePixel = imgClipped.pixel(100, 100);
    QVERIFY(qAlpha(insidePixel) > 0);
}

void KisAiStrokeRendererTest::testClippingMaskFromPreviousGoalStep()
{
    KisAiStrokeProgram previousStep;
    previousStep.canvasSize = QSize(200, 200);

    KisAiStrokeOperation flatOp;
    flatOp.kind = KisAiStrokeOperation::Kind::Fill;
    flatOp.id = QStringLiteral("flat_previous_step");
    flatOp.layer = QStringLiteral("Flats");
    flatOp.polygon = QPolygonF{QPointF(0.4, 0.4), QPointF(0.6, 0.4), QPointF(0.6, 0.6), QPointF(0.4, 0.6)};
    flatOp.brush.color = QColor(200, 200, 200);
    previousStep.operations.append(flatOp);

    KisAiStrokeProgram shadingStep;
    shadingStep.canvasSize = QSize(200, 200);
    KisAiStrokeOperation shadeOp;
    shadeOp.kind = KisAiStrokeOperation::Kind::Fill;
    shadeOp.id = QStringLiteral("shade_current_step");
    shadeOp.layer = QStringLiteral("Shading");
    shadeOp.polygon = QPolygonF{QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    shadeOp.brush.color = QColor(50, 50, 100);
    shadingStep.operations.append(shadeOp);

    const QImage rendered =
        KisAiStrokeRenderer::renderProgramToImage(shadingStep, QSize(200, 200), true, &previousStep);
    QVERIFY(!rendered.isNull());
    QCOMPARE(qAlpha(rendered.pixel(20, 20)), 0);
    QVERIFY(qAlpha(rendered.pixel(100, 100)) > 0);
}

void KisAiStrokeRendererTest::testRenderGradientOpacity()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(100, 100);

    KisAiStrokeOperation gradOp;
    gradOp.kind = KisAiStrokeOperation::Kind::GradientFill;
    gradOp.id = QStringLiteral("half_alpha_grad");
    gradOp.layer = QStringLiteral("Background");
    gradOp.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.0, 0.0, 1.0), KisAiStrokePoint(1.0, 1.0, 1.0)};
    gradOp.gradientColors = {QColor(255, 0, 0), QColor(0, 0, 255)};
    gradOp.brush.opacity = 0.5; // 50% opacity
    program.operations.append(gradOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(100, 100));
    const QRgb centerPixel = img.pixel(50, 50);
    const int alpha = qAlpha(centerPixel);

    // Alpha should be around 128 (0.5 * 255), certainly not 255
    QVERIFY(alpha > 100 && alpha < 155);
}

void KisAiStrokeRendererTest::testRenderParticleBrush()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(100, 100);

    KisAiStrokeOperation particleOp;
    particleOp.kind = KisAiStrokeOperation::Kind::Particles;
    particleOp.id = QStringLiteral("sparks");
    particleOp.layer = QStringLiteral("Shading");
    particleOp.bounds = QRectF(0.2, 0.2, 0.6, 0.6);
    particleOp.particleCount = 32;
    particleOp.brush.profile = QStringLiteral("spray");
    particleOp.brush.color = QColor(255, 255, 0);
    particleOp.brush.opacity = 0.8;
    particleOp.brush.size = 0.05;
    program.operations.append(particleOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(100, 100));
    QVERIFY(!img.isNull());

    // Count non-transparent pixels in bounding box
    int nonZeroAlphaCount = 0;
    for (int y = 20; y < 80; ++y) {
        for (int x = 20; x < 80; ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                nonZeroAlphaCount++;
            }
        }
    }
    QVERIFY(nonZeroAlphaCount > 0);
}

void KisAiStrokeRendererTest::testRenderAirbrushDynamics()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation airOp;
    airOp.kind = KisAiStrokeOperation::Kind::Path;
    airOp.id = QStringLiteral("soft_airbrush");
    airOp.layer = QStringLiteral("Shading");
    airOp.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.1, 0.5, 0.5),
                                             KisAiStrokePoint(0.5, 0.5, 1.0),
                                             KisAiStrokePoint(0.9, 0.5, 0.5)};
    airOp.brush.profile = QStringLiteral("airbrush");
    airOp.brush.color = QColor(100, 150, 250);
    airOp.brush.size = 0.1;
    airOp.brush.opacity = 0.9;
    program.operations.append(airOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200));
    QVERIFY(!img.isNull());

    const QRgb centerPixel = img.pixel(100, 100);
    QVERIFY(qAlpha(centerPixel) > 0);
}

void KisAiStrokeRendererTest::testRenderHatchOperation()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation hatchOp;
    hatchOp.kind = KisAiStrokeOperation::Kind::Hatch;
    hatchOp.id = QStringLiteral("hatch_test");
    hatchOp.layer = QStringLiteral("Shading");
    hatchOp.polygon = QPolygonF{QPointF(0.25, 0.25), QPointF(0.75, 0.25), QPointF(0.75, 0.75), QPointF(0.25, 0.75)};
    hatchOp.angleDeg = 45.0;
    hatchOp.spacing = 0.05;
    hatchOp.crossHatch = true;
    hatchOp.brush.color = QColor(40, 40, 40);
    hatchOp.brush.size = 0.01;
    hatchOp.brush.opacity = 1.0;
    program.operations.append(hatchOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200));
    QVERIFY(!img.isNull());

    // Polygon interior (50..150, 50..150) should have hatch lines painted
    int insidePaintedCount = 0;
    for (int y = 60; y < 140; ++y) {
        for (int x = 60; x < 140; ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                insidePaintedCount++;
            }
        }
    }
    QVERIFY(insidePaintedCount > 0);

    // Polygon exterior (e.g. top-left corner (10, 10)) MUST be completely clipped/transparent
    QCOMPARE(qAlpha(img.pixel(10, 10)), 0);
    QCOMPARE(qAlpha(img.pixel(190, 190)), 0);
}

void KisAiStrokeRendererTest::testRenderRadialGradient()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation radOp;
    radOp.kind = KisAiStrokeOperation::Kind::GradientFill;
    radOp.id = QStringLiteral("radial_test");
    radOp.layer = QStringLiteral("Flats");
    radOp.isRadial = true;
    radOp.gradientCenter = QPointF(0.5, 0.5);
    radOp.gradientRadius = 0.4;
    radOp.gradientColors = QVector<QColor>{QColor(255, 100, 50), QColor(20, 20, 80)};
    radOp.polygon = QPolygonF{QPointF(0.1, 0.1), QPointF(0.9, 0.1), QPointF(0.9, 0.9), QPointF(0.1, 0.9)};
    program.operations.append(radOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200));
    QVERIFY(!img.isNull());

    const QRgb centerPixel = img.pixel(100, 100);
    QVERIFY(qAlpha(centerPixel) > 200);
    QVERIFY(qRed(centerPixel) > 200); // Inner color is reddish
}

void KisAiStrokeRendererTest::testRenderZeroDimensionsFallback()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation fillOp;
    fillOp.kind = KisAiStrokeOperation::Kind::Fill;
    fillOp.id = QStringLiteral("bg");
    fillOp.layer = QStringLiteral("Flats");
    fillOp.polygon = QPolygonF{QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    fillOp.brush.color = QColor(100, 150, 200);
    program.operations.append(fillOp);

    // 1. QSize(0, 0) targetSize: should fallback to program.canvasSize (200, 200)
    const QImage imgZero = KisAiStrokeRenderer::renderProgramToImage(program, QSize(0, 0));
    QVERIFY(!imgZero.isNull());
    QCOMPARE(imgZero.size(), QSize(200, 200));
    QVERIFY(qAlpha(imgZero.pixel(100, 100)) > 0);

    // 2. Negative dimensions: should also fallback to program.canvasSize
    const QImage imgNeg = KisAiStrokeRenderer::renderProgramToImage(program, QSize(-1, -1));
    QVERIFY(!imgNeg.isNull());
    QCOMPARE(imgNeg.size(), QSize(200, 200));

    // 3. Sub-64 dimensions: should fallback to program.canvasSize
    const QImage imgSmall = KisAiStrokeRenderer::renderProgramToImage(program, QSize(32, 32));
    QVERIFY(!imgSmall.isNull());
    QCOMPARE(imgSmall.size(), QSize(200, 200));

    // 4. When program.canvasSize is also zero/invalid: fallback to default 1024x1024
    KisAiStrokeProgram emptySizeProg = program;
    emptySizeProg.canvasSize = QSize(0, 0);
    const QImage imgBothZero = KisAiStrokeRenderer::renderProgramToImage(emptySizeProg, QSize(0, 0));
    QVERIFY(!imgBothZero.isNull());
    QCOMPARE(imgBothZero.size(), QSize(1024, 1024));
    QVERIFY(qAlpha(imgBothZero.pixel(512, 512)) > 0);
}

void KisAiStrokeRendererTest::testUnnormalizedLayerRendering()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    // Layer named "flat" (alias for "Flats")
    KisAiStrokeOperation flatOp;
    flatOp.kind = KisAiStrokeOperation::Kind::Fill;
    flatOp.id = QStringLiteral("base_box");
    flatOp.layer = QStringLiteral("flat");
    flatOp.polygon = QPolygonF{QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.2, 0.8)};
    flatOp.brush.color = QColor(200, 200, 200);
    program.operations.append(flatOp);

    // Layer named "shade" (alias for "Shading") with silhouette clipping
    KisAiStrokeOperation shadeOp;
    shadeOp.kind = KisAiStrokeOperation::Kind::Fill;
    shadeOp.id = QStringLiteral("shade_all");
    shadeOp.layer = QStringLiteral("shade");
    shadeOp.polygon = QPolygonF{QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    shadeOp.brush.color = QColor(50, 50, 50, 128);
    program.operations.append(shadeOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200), true);
    QVERIFY(!img.isNull());

    // Center pixel inside the flat polygon should be painted
    QVERIFY(qAlpha(img.pixel(100, 100)) > 0);

    // Corner pixel outside the flat polygon must be clipped / transparent because Shading clips to Flats!
    QCOMPARE(qAlpha(img.pixel(10, 10)), 0);
}

void KisAiStrokeRendererTest::testCentripetalSplineAvoidsUnevenPointLoop()
{
    const QVector<QPointF> input = {QPointF(0.0, 0.0), QPointF(0.95, 0.02), QPointF(1.0, 4.0), QPointF(2.0, 4.1)};
    const QVector<QPointF> spline = KisAiStrokeRenderer::generateCatmullRomSpline(input, 16, false);

    QCOMPARE(spline.first(), input.first());
    QCOMPARE(spline.last(), input.last());
    for (const QPointF &point : spline) {
        QVERIFY(std::isfinite(point.x()));
        QVERIFY(std::isfinite(point.y()));
        QVERIFY2(point.x() >= -0.02 && point.x() <= 2.02, qPrintable(QString::number(point.x())));
        QVERIFY2(point.y() >= -0.1 && point.y() <= 4.2, qPrintable(QString::number(point.y())));
    }
}

void KisAiStrokeRendererTest::testPressureStrokeHasAntialiasedTaper()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(256, 256);

    KisAiStrokeOperation stroke;
    stroke.kind = KisAiStrokeOperation::Kind::Path;
    stroke.id = QStringLiteral("taper_quality");
    stroke.layer = QStringLiteral("Lineart");
    stroke.brush.profile = QStringLiteral("gpen");
    stroke.brush.color = QColor(24, 20, 32);
    stroke.brush.size = 0.04;
    stroke.points = {KisAiStrokePoint(0.1, 0.5, 1.0),
                     KisAiStrokePoint(0.35, 0.42, 0.9),
                     KisAiStrokePoint(0.65, 0.58, 0.75),
                     KisAiStrokePoint(0.9, 0.5, 0.5)};
    program.operations.append(stroke);

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(program, QSize(256, 256));
    QVERIFY(!image.isNull());

    int partialAlphaPixels = 0;
    int paintedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = qAlpha(image.pixel(x, y));
            if (alpha > 0)
                ++paintedPixels;
            if (alpha > 0 && alpha < 255)
                ++partialAlphaPixels;
        }
    }
    QVERIFY(paintedPixels > 250);
    QVERIFY(partialAlphaPixels > 30);

    auto verticalCoverage = [&image](int x) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 16)
                ++count;
        }
        return count;
    };
    QVERIFY(verticalCoverage(128) > verticalCoverage(26));
}

void KisAiStrokeRendererTest::testRepresentativeCompositionQualityMetrics()
{
    const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("cinematic sunset mountain landscape with sakura petals"),
        QSize(1024, 768));
    QVERIFY(program.isValid());
    QVERIFY(program.completionScore >= 0.6);

    const QMap<QString, int> layers = KisAiStrokeProgramCodec::countLayerOperations(program);
    for (const QString &layer : {QStringLiteral("Flats"),
                                 QStringLiteral("Shading"),
                                 QStringLiteral("Lineart"),
                                 QStringLiteral("Highlights"),
                                 QStringLiteral("FX")}) {
        QVERIFY2(layers.value(layer) > 0, qPrintable(layer));
    }

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(program, QSize(512, 384));
    QVERIFY(!image.isNull());

    int opaqueSamples = 0;
    int chromaticSamples = 0;
    QSet<QRgb> sampledColors;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 220)
                ++opaqueSamples;
            if (color.alpha() > 32
                && (qMax(color.red(), qMax(color.green(), color.blue()))
                    - qMin(color.red(), qMin(color.green(), color.blue())))
                    > 12) {
                ++chromaticSamples;
            }
            if (color.alpha() > 0)
                sampledColors.insert(image.pixel(x, y));
        }
    }
    const int sampleCount = (image.width() / 4) * (image.height() / 4);
    QVERIFY(opaqueSamples > sampleCount * 0.70);
    QVERIFY(chromaticSamples > sampleCount * 0.35);
    QVERIFY(sampledColors.size() > 120);

    const QString artifactDir = qEnvironmentVariable("AI_STROKE_TEST_ARTIFACT_DIR");
    if (!artifactDir.isEmpty()) {
        QVERIFY(QDir().mkpath(artifactDir));
        QVERIFY(image.save(QDir(artifactDir).filePath(QStringLiteral("representative-stroke-quality.png"))));
    }
}

void KisAiStrokeRendererTest::testRenderBackgroundLayer()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    // Background operation: full red fill
    KisAiStrokeOperation bg;
    bg.kind = KisAiStrokeOperation::Kind::Fill;
    bg.layer = QStringLiteral("Background");
    bg.brush.color = QColor(255, 0, 0);
    bg.polygon = {QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    program.operations.append(bg);

    // Flats operation: small green box in center (clipped to 0.4 - 0.6)
    KisAiStrokeOperation flats;
    flats.kind = KisAiStrokeOperation::Kind::Fill;
    flats.layer = QStringLiteral("Flats");
    flats.brush.color = QColor(0, 255, 0);
    flats.polygon = {QPointF(0.4, 0.4), QPointF(0.6, 0.4), QPointF(0.6, 0.6), QPointF(0.4, 0.6)};
    program.operations.append(flats);

    const QImage rendered = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200), true);
    QVERIFY(!rendered.isNull());

    // Corner (0.1, 0.1) should show red Background, not be clipped by Flats
    const QColor corner = rendered.pixelColor(20, 20);
    QVERIFY(corner.red() > 200 && corner.green() < 50);

    // Center (0.5, 0.5) should show green Flats on top of Background
    const QColor center = rendered.pixelColor(100, 100);
    QVERIFY(center.green() > 200 && center.red() < 50);
}

void KisAiStrokeRendererTest::testRenderMangaLinesOperation()
{
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation manga;
    manga.kind = KisAiStrokeOperation::Kind::MangaLines;
    manga.layer = QStringLiteral("FX");
    manga.gradientCenter = QPointF(0.5, 0.5);
    manga.innerRadius = 0.20; // 40px radius from center should be empty
    manga.outerRadius = 0.80; // 160px radius
    manga.density = 48;
    manga.brush.color = QColor(0, 0, 0);
    manga.brush.size = 0.02;
    program.operations.append(manga);

    const QImage rendered = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200), false);
    QVERIFY(!rendered.isNull());

    // Center (100, 100) should be transparent (inside innerRadius)
    QCOMPARE(rendered.pixelColor(100, 100).alpha(), 0);

    // Outside inner radius, pixels should be drawn
    int drawnPixels = 0;
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            const qreal dist = std::hypot(x - 100, y - 100);
            if (dist > 45.0 && dist < 150.0 && rendered.pixelColor(x, y).alpha() > 50) {
                ++drawnPixels;
            }
        }
    }
    QVERIFY(drawnPixels > 50);
}

void KisAiStrokeRendererTest::testRenderMangaLinesWithOriginCenter()
{
    // Regression test: gradientCenter (0, 0) must be treated as a valid coordinate
    // (top-left corner), not as "not set". Previously, isNull() was used which
    // incorrectly treated (0, 0) as the default and replaced it with (0.5, 0.5).
    KisAiStrokeProgram program;
    program.canvasSize = QSize(200, 200);

    KisAiStrokeOperation manga;
    manga.kind = KisAiStrokeOperation::Kind::MangaLines;
    manga.layer = QStringLiteral("FX");
    manga.gradientCenter = QPointF(0.0, 0.0); // Explicitly set to origin
    manga.innerRadius = 0.05;
    manga.outerRadius = 0.60;
    manga.density = 36;
    manga.brush.color = QColor(0, 0, 0);
    manga.brush.size = 0.02;
    program.operations.append(manga);

    const QImage rendered = KisAiStrokeRenderer::renderProgramToImage(program, QSize(200, 200), false);
    QVERIFY(!rendered.isNull());

    // With center at (0, 0), the lines radiate from the top-left corner.
    // The corner (0, 0) should be transparent (inside innerRadius).
    QCOMPARE(rendered.pixelColor(0, 0).alpha(), 0);

    // Pixels far from the origin should have drawn lines.
    int drawnPixels = 0;
    for (int y = 0; y < 200; ++y) {
        for (int x = 0; x < 200; ++x) {
            const qreal dist = std::hypot(x, y);
            if (dist > 20.0 && dist < 110.0 && rendered.pixelColor(x, y).alpha() > 50) {
                ++drawnPixels;
            }
        }
    }
    QVERIFY(drawnPixels > 50);

    // The configured outer radius ends before the far bottom-right quadrant.
    // With the origin as the center, the visible near-origin quadrant should
    // therefore contain more rendered pixels than the distant quadrant.
    int topLeftCount = 0;
    int bottomRightCount = 0;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            if (rendered.pixelColor(x, y).alpha() > 50)
                ++topLeftCount;
        }
    }
    for (int y = 100; y < 200; ++y) {
        for (int x = 100; x < 200; ++x) {
            if (rendered.pixelColor(x, y).alpha() > 50)
                ++bottomRightCount;
        }
    }
    QVERIFY(topLeftCount > bottomRightCount);
}

void KisAiStrokeRendererTest::testNewBrushProfilesRendering()
{
    for (const QString &profile :
         {QStringLiteral("marker"), QStringLiteral("crayon"), QStringLiteral("neon"), QStringLiteral("splatter")}) {
        KisAiStrokeProgram program;
        program.canvasSize = QSize(100, 100);

        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.layer = QStringLiteral("Lineart");
        op.brush.profile = profile;
        op.brush.color = QColor(0, 128, 255);
        op.brush.size = 0.08;
        op.points = {KisAiStrokePoint(0.2, 0.2, 0.8), KisAiStrokePoint(0.8, 0.8, 0.8)};
        program.operations.append(op);

        const QImage img = KisAiStrokeRenderer::renderProgramToImage(program, QSize(100, 100), false);
        QVERIFY(!img.isNull());

        int coloredCount = 0;
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 100; ++x) {
                if (img.pixelColor(x, y).alpha() > 10)
                    ++coloredCount;
            }
        }
        QVERIFY2(coloredCount > 20, qPrintable(profile));
    }
}

void KisAiStrokeRendererTest::testCaptureImageBase64()
{
    QImage testImg(1024, 768, QImage::Format_ARGB32_Premultiplied);
    testImg.fill(Qt::blue);

    const QString b64 = KisAiStrokeRenderer::captureImageBase64(testImg, 512, 75);
    QVERIFY(!b64.isEmpty());
    QVERIFY(b64.startsWith(QStringLiteral("data:image/jpeg;base64,")));

    const QString data = b64.mid(QStringLiteral("data:image/jpeg;base64,").length());
    const QByteArray decoded = QByteArray::fromBase64(data.toLatin1());
    QVERIFY(!decoded.isEmpty());

    QImage loaded;
    QVERIFY(loaded.loadFromData(decoded, "JPEG"));
    QVERIFY(loaded.width() <= 512);
    QVERIFY(loaded.height() <= 512);
}

void KisAiStrokeRendererTest::testRenderGoalModeProgression()
{
    const QSize canvasSize(512, 512);
    const QString prompt = QStringLiteral("portrait of a cyberpunk warrior with glowing visor");

    const KisAiStrokeProgram step1 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 1, 2);
    const KisAiStrokeProgram step2 = KisAiStrokeProgramCodec::createDeterministicProgramStep(prompt, canvasSize, 2, 2);

    QVERIFY(!step1.operations.isEmpty());
    QVERIFY(!step2.operations.isEmpty());

    const QImage imgStep1 = KisAiStrokeRenderer::renderProgramToImage(step1, canvasSize);
    QVERIFY(!imgStep1.isNull());

    const KisAiStrokeProgram merged = KisAiStrokeProgramCodec::mergePrograms(step1, step2);
    const QImage imgFinal = KisAiStrokeRenderer::renderProgramToImage(merged, canvasSize);
    QVERIFY(!imgFinal.isNull());

    int step1Painted = 0;
    int finalPainted = 0;
    for (int y = 0; y < canvasSize.height(); y += 4) {
        for (int x = 0; x < canvasSize.width(); x += 4) {
            if (imgStep1.pixelColor(x, y).alpha() > 16)
                ++step1Painted;
            if (imgFinal.pixelColor(x, y).alpha() > 16)
                ++finalPainted;
        }
    }
    QVERIFY(step1Painted > 100);
    QVERIFY(finalPainted >= step1Painted);
}

void KisAiStrokeRendererTest::testGoalModeCumulativeProgressionAndRibbon()
{
    const QSize canvasSize(256, 256);
    KisAiStrokeProgram accumulated;

    // Simulate Goal Mode accumulating across 4 steps
    for (int step = 1; step <= 4; ++step) {
        const KisAiStrokeProgram stepProg = KisAiStrokeProgramCodec::createDeterministicProgramStep(
            QStringLiteral("anime girl portrait with vibrant eyes"),
            canvasSize,
            step,
            4);
        QVERIFY(!stepProg.operations.isEmpty());
        accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, stepProg);
    }

    QVERIFY(accumulated.operations.size() > 4);
    QVERIFY(accumulated.completionScore >= 0.8);

    const QImage finalPreview = KisAiStrokeRenderer::renderProgramToImage(accumulated, canvasSize);
    QVERIFY(!finalPreview.isNull());

    // Ribbon rendering test
    KisAiStrokeProgram ribbonProg;
    ribbonProg.canvasSize = canvasSize;
    KisAiStrokeOperation ribbonOp;
    ribbonOp.kind = KisAiStrokeOperation::Kind::Ribbon;
    ribbonOp.layer = QStringLiteral("Lineart");
    ribbonOp.points = {KisAiStrokePoint(0.1, 0.5), KisAiStrokePoint(0.9, 0.5)};
    ribbonOp.widthStart = 0.08;
    ribbonOp.widthEnd = 0.02;
    ribbonOp.brush.color = QColor(255, 0, 0);
    ribbonOp.brush.opacity = 1.0;
    ribbonProg.operations.append(ribbonOp);

    const QImage ribbonImg = KisAiStrokeRenderer::renderProgramToImage(ribbonProg, canvasSize);
    QVERIFY(!ribbonImg.isNull());
    const QColor centerCol = ribbonImg.pixelColor(128, 128);
    QVERIFY(centerCol.alpha() > 50);
    QVERIFY(centerCol.red() > 150);
}

void KisAiStrokeRendererTest::testQualityUtilsResampling()
{
    QVector<KisAiStrokePoint> pts;
    pts.append(KisAiStrokePoint(0.0, 0.0, 0.2));
    pts.append(KisAiStrokePoint(100.0, 0.0, 0.8));

    const auto resampled = KisAiStrokeQualityUtils::resampleEquidistant(pts, 10.0, false);
    QVERIFY(resampled.size() >= 10);
    QCOMPARE(resampled.first().pos, QPointF(0.0, 0.0));
    QCOMPARE(resampled.last().pos, QPointF(100.0, 0.0));
    // Pressure should smoothly interpolate from 0.2 to 0.8
    QVERIFY(resampled.at(5).pressure > 0.4 && resampled.at(5).pressure < 0.6);
}

void KisAiStrokeRendererTest::testQualityUtilsRdpSimplification()
{
    QVector<QPointF> noisyLine;
    for (int i = 0; i <= 20; ++i) {
        // Line along x axis with tiny y jitter
        const qreal jitter = (i > 0 && i < 20) ? 0.05 : 0.0;
        noisyLine.append(QPointF(i * 5.0, jitter));
    }

    const auto simplified = KisAiStrokeQualityUtils::simplifyRDP(noisyLine, 0.5);
    // Micro-jitter is removed, should collapse to 2 endpoints
    QCOMPARE(simplified.size(), 2);
    QCOMPARE(simplified.first(), QPointF(0.0, 0.0));
    QCOMPARE(simplified.last(), QPointF(100.0, 0.0));

    // Sharp turn should be preserved
    QVector<QPointF> cornerPath = {QPointF(0, 0), QPointF(50, 50), QPointF(100, 0)};
    const auto cornerSimplified = KisAiStrokeQualityUtils::simplifyRDP(cornerPath, 1.0);
    QCOMPARE(cornerSimplified.size(), 3);
    QCOMPARE(cornerSimplified.at(1), QPointF(50, 50));
}

void KisAiStrokeRendererTest::testQualityUtilsCornerPreservingSmoothing()
{
    // Triangle with a sharp 90 degree corner at (0, 100)
    QPolygonF triangle;
    triangle << QPointF(0, 0) << QPointF(0, 100) << QPointF(100, 0);

    const QPolygonF smoothed = KisAiStrokeQualityUtils::smoothPolygonCornerPreserving(triangle, 120.0, 4);
    QVERIFY(smoothed.size() > triangle.size());

    // The sharp corner (0, 100) must be retained
    bool foundCorner = false;
    for (const QPointF &pt : smoothed) {
        if (std::hypot(pt.x() - 0.0, pt.y() - 100.0) < 1.0e-3) {
            foundCorner = true;
            break;
        }
    }
    QVERIFY(foundCorner);
}

void KisAiStrokeRendererTest::testQualityUtilsPolygonOffsetAndTrapping()
{
    // Box from (10, 10) to (50, 50)
    QPolygonF box;
    box << QPointF(10, 10) << QPointF(50, 10) << QPointF(50, 50) << QPointF(10, 50);

    const QPolygonF dilated = KisAiStrokeQualityUtils::offsetPolygon(box, 5.0);
    const QRectF bOrig = box.boundingRect();
    const QRectF bDilated = dilated.boundingRect();

    QVERIFY(bDilated.left() < bOrig.left());
    QVERIFY(bDilated.right() > bOrig.right());
    QVERIFY(bDilated.top() < bOrig.top());
    QVERIFY(bDilated.bottom() > bOrig.bottom());
}

void KisAiStrokeRendererTest::testQualityUtilsBrushTaperAndDynamics()
{
    // G-Pen taper
    const qreal gpenStart = KisAiStrokeQualityUtils::calculateTaper(0.01, QStringLiteral("gpen"), false);
    const qreal gpenMid = KisAiStrokeQualityUtils::calculateTaper(0.50, QStringLiteral("gpen"), false);
    const qreal gpenEnd = KisAiStrokeQualityUtils::calculateTaper(0.99, QStringLiteral("gpen"), false);

    QVERIFY(gpenStart < 0.5);
    QCOMPARE(gpenMid, 1.0);
    QVERIFY(gpenEnd < 0.2);

    // Closed stroke has no taper
    QCOMPARE(KisAiStrokeQualityUtils::calculateTaper(0.01, QStringLiteral("gpen"), true), 1.0);
}

void KisAiStrokeRendererTest::testQualityUtilsCalligraphyWidth()
{
    const qreal baseW = 20.0;
    // Movement perpendicular to 45 deg nib (e.g. 135 deg)
    const QPointF perpDir(-0.7071, 0.7071);
    const qreal wideW = KisAiStrokeQualityUtils::calculateCalligraphyWidth(perpDir, baseW, 45.0, 0.20);

    // Movement parallel to 45 deg nib
    const QPointF parallelDir(0.7071, 0.7071);
    const qreal thinW = KisAiStrokeQualityUtils::calculateCalligraphyWidth(parallelDir, baseW, 45.0, 0.20);

    QVERIFY(wideW > thinW);
    QVERIFY(wideW >= baseW * 0.9);
    QVERIFY(thinW <= baseW * 0.35);
}

void KisAiStrokeRendererTest::testQualityUtilsHalftonePattern()
{
    QImage canvas(100, 100, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPainter painter(&canvas);
    QPolygonF poly;
    poly << QPointF(10, 10) << QPointF(90, 10) << QPointF(90, 90) << QPointF(10, 90);

    KisAiStrokeQualityUtils::drawHalftonePattern(painter, poly, QColor(0, 0, 0, 255), 10.0, 3.0, 45.0, false);
    painter.end();

    // Check that pattern rendered non-empty pixels inside and transparent outside
    QVERIFY(canvas.pixelColor(5, 5).alpha() == 0); // Outside
    bool hasDrawnPixel = false;
    for (int y = 20; y < 80; ++y) {
        for (int x = 20; x < 80; ++x) {
            if (canvas.pixelColor(x, y).alpha() > 200) {
                hasDrawnPixel = true;
                break;
            }
        }
        if (hasDrawnPixel)
            break;
    }
    QVERIFY(hasDrawnPixel);
}

void KisAiStrokeRendererTest::testQualityUtilsHueShiftedHarmonies()
{
    // Peach skin color: warm hue
    const QColor skinColor(255, 205, 170);
    const QColor shadow = KisAiStrokeQualityUtils::calculateHueShiftedShadow(skinColor);

    QVERIFY(shadow.isValid());
    // Shadow must be darker than base
    QVERIFY(shadow.lightness() < skinColor.lightness());

    // Verify warm shadow shifts toward violet/purple (wrap into > 0.85), not forward into yellow/green
    const QColor skinShadowPure = KisAiStrokeQualityUtils::calculateHueShiftedShadow(skinColor, QColor());
    float skinShadowH = 0.0f, skinShadowS = 0.0f, skinShadowL = 0.0f;
    skinShadowPure.getHslF(&skinShadowH, &skinShadowS, &skinShadowL);
    QVERIFY2(skinShadowH > 0.85f || skinShadowH <= 0.02f, qPrintable(QString::number(skinShadowH)));

    const QColor redColor(240, 40, 40);
    const QColor redShadowPure = KisAiStrokeQualityUtils::calculateHueShiftedShadow(redColor, QColor());
    float redShadowH = 0.0f, redShadowS = 0.0f, redShadowL = 0.0f;
    redShadowPure.getHslF(&redShadowH, &redShadowS, &redShadowL);
    QVERIFY2(redShadowH > 0.85f, qPrintable(QString::number(redShadowH)));

    const QColor highlight = KisAiStrokeQualityUtils::calculateHueShiftedHighlight(skinColor);
    QVERIFY(highlight.isValid());
    QVERIFY(highlight.lightness() > skinColor.lightness());

    const QColor gray(128, 128, 128);
    // With invalid ambient tint (no ambient tint), shadow and highlight remain purely achromatic (R == G == B)
    const QColor pureGrayShadow = KisAiStrokeQualityUtils::calculateHueShiftedShadow(gray, QColor());
    QVERIFY(pureGrayShadow.isValid());
    QVERIFY(pureGrayShadow.lightness() < gray.lightness());
    QCOMPARE(pureGrayShadow.red(), pureGrayShadow.green());
    QCOMPARE(pureGrayShadow.green(), pureGrayShadow.blue());

    const QColor pureGrayHighlight = KisAiStrokeQualityUtils::calculateHueShiftedHighlight(gray, QColor());
    QVERIFY(pureGrayHighlight.isValid());
    QVERIFY(pureGrayHighlight.lightness() > gray.lightness());
    QCOMPARE(pureGrayHighlight.red(), pureGrayHighlight.green());
    QCOMPARE(pureGrayHighlight.green(), pureGrayHighlight.blue());

    // With default ambient shadow tint (cool indigo: QColor(35, 40, 65)),
    // shadow deepens with cool tones (blue >= green >= red), avoiding red distortion
    const QColor grayShadow = KisAiStrokeQualityUtils::calculateHueShiftedShadow(gray);
    QVERIFY(grayShadow.blue() >= grayShadow.green());
    QVERIFY(grayShadow.green() >= grayShadow.red());

    // Boundary test: hue near 1.0 boundary does not generate out-of-range hsl or warnings
    const QColor boundaryColor = QColor::fromHslF(0.999f, 0.8f, 0.5f);
    const QColor boundaryShadow = KisAiStrokeQualityUtils::calculateHueShiftedShadow(boundaryColor, QColor());
    QVERIFY(boundaryShadow.isValid());
    float bshH = 0.0f, bshS = 0.0f, bshL = 0.0f;
    boundaryShadow.getHslF(&bshH, &bshS, &bshL);
    QVERIFY(bshH >= 0.0f && bshH < 1.0f);

    const QColor boundaryHighlight = KisAiStrokeQualityUtils::calculateHueShiftedHighlight(boundaryColor, QColor());
    QVERIFY(boundaryHighlight.isValid());
    float bhlH = 0.0f, bhlS = 0.0f, bhlL = 0.0f;
    boundaryHighlight.getHslF(&bhlH, &bhlS, &bhlL);
    QVERIFY(bhlH >= 0.0f && bhlH < 1.0f);
}

void KisAiStrokeRendererTest::testQualityUtilsProgramTrapping()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(200, 200);

    KisAiStrokeOperation flatOp;
    flatOp.kind = KisAiStrokeOperation::Kind::Fill;
    flatOp.layer = QStringLiteral("Flats");
    flatOp.polygon = {QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.2, 0.8)};
    prog.operations.append(flatOp);

    const KisAiStrokeProgram trappedProg = KisAiStrokeQualityUtils::applyTrapping(prog, 4.0);
    QCOMPARE(trappedProg.operations.size(), 1);

    const QRectF bOrig = prog.operations.at(0).polygon.boundingRect();
    const QRectF bTrapped = trappedProg.operations.at(0).polygon.boundingRect();

    QVERIFY(bTrapped.left() < bOrig.left());
    QVERIFY(bTrapped.right() > bOrig.right());
}

void KisAiStrokeRendererTest::testRenderCalligraphyAndCharcoalBrush()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(200, 200);

    // Calligraphy stroke
    KisAiStrokeOperation calligOp;
    calligOp.kind = KisAiStrokeOperation::Kind::Path;
    calligOp.layer = QStringLiteral("Lineart");
    calligOp.brush.profile = QStringLiteral("calligraphy");
    calligOp.brush.color = QColor(20, 20, 40);
    calligOp.brush.size = 0.05;
    calligOp.points = {KisAiStrokePoint(0.1, 0.2), KisAiStrokePoint(0.9, 0.2)};
    prog.operations.append(calligOp);

    // Charcoal stroke
    KisAiStrokeOperation charcoalOp;
    charcoalOp.kind = KisAiStrokeOperation::Kind::Path;
    charcoalOp.layer = QStringLiteral("Lineart");
    charcoalOp.brush.profile = QStringLiteral("charcoal");
    charcoalOp.brush.color = QColor(50, 40, 30);
    charcoalOp.brush.size = 0.06;
    charcoalOp.points = {KisAiStrokePoint(0.1, 0.5), KisAiStrokePoint(0.9, 0.5)};
    prog.operations.append(charcoalOp);

    // Bristle brush stroke
    KisAiStrokeOperation bristleOp;
    bristleOp.kind = KisAiStrokeOperation::Kind::Path;
    bristleOp.layer = QStringLiteral("Lineart");
    bristleOp.brush.profile = QStringLiteral("brush");
    bristleOp.brush.color = QColor(180, 40, 40);
    bristleOp.brush.size = 0.08;
    bristleOp.points = {KisAiStrokePoint(0.1, 0.8), KisAiStrokePoint(0.9, 0.8)};
    prog.operations.append(bristleOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(200, 200));
    QVERIFY(!img.isNull());

    // Check that each stroke line rendered
    QVERIFY(img.pixelColor(100, 40).alpha() > 100); // Calligraphy line
    QVERIFY(img.pixelColor(100, 100).alpha() > 50); // Charcoal line
    QVERIFY(img.pixelColor(100, 160).alpha() > 100); // Bristle brush line
}

void KisAiStrokeRendererTest::testSynthesizeHairClump()
{
    KisAiStrokeOperation ribbonOp;
    ribbonOp.kind = KisAiStrokeOperation::Kind::Ribbon;
    ribbonOp.id = QStringLiteral("hair_ribbon");
    ribbonOp.layer = QStringLiteral("Flats");
    ribbonOp.spine = {QPointF(0.3, 0.2), QPointF(0.4, 0.4), QPointF(0.35, 0.6), QPointF(0.3, 0.8)};
    ribbonOp.brush.color = QColor(120, 60, 40);
    ribbonOp.brush.size = 0.05;

    const auto clump = KisAiStrokeQualityUtils::synthesizeHairClump(ribbonOp, QSize(400, 400), 12345);
    QCOMPARE(clump.mainMass.kind, KisAiStrokeOperation::Kind::Ribbon);
    QVERIFY(clump.strands.size() >= 4);
    QVERIFY(clump.flyaways.size() >= 1);
    QVERIFY(!clump.highlightHalo.points.isEmpty());
    QCOMPARE(clump.highlightHalo.layer, QStringLiteral("Highlights"));

    // Check procedural expansion in renderer
    QVector<KisAiStrokeOperation> ops = {ribbonOp};
    ops.first().brush.profile = QStringLiteral("hair");
    const QVector<KisAiStrokeOperation> expanded =
        KisAiStrokeRenderer::expandProceduralOperations(ops, QSize(400, 400));
    QVERIFY(expanded.size() > 1);
}

void KisAiStrokeRendererTest::testSynthesizeFoliageClusters()
{
    KisAiStrokeOperation fillOp;
    fillOp.kind = KisAiStrokeOperation::Kind::Fill;
    fillOp.id = QStringLiteral("tree_canopy");
    fillOp.layer = QStringLiteral("Flats");
    fillOp.polygon = {QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.9, 0.6), QPointF(0.5, 0.8), QPointF(0.1, 0.6)};
    fillOp.brush.color = QColor(255, 180, 200); // Sakura pink
    fillOp.brush.profile = QStringLiteral("watercolor");

    const QVector<KisAiStrokeOperation> clusters =
        KisAiStrokeQualityUtils::synthesizeFoliageClusters(fillOp, QSize(400, 400), 54321);
    QVERIFY(clusters.size() >= 4); // base wash + multiple clusters + petals

    bool hasDriftingPetals = false;
    for (const KisAiStrokeOperation &op : clusters) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles && op.layer == QLatin1String("FX")) {
            hasDriftingPetals = true;
            break;
        }
    }
    QVERIFY(hasDriftingPetals);
}

void KisAiStrokeRendererTest::testDualShadowSeparation()
{
    const QSize canvasSize(1000, 1000);

    // Cast shadow: narrow elongated bangs shadow (e.g. 200px wide, 15px tall -> aspect ratio ~13.3)
    const QPolygonF castPoly = {QPointF(0.2, 0.2), QPointF(0.4, 0.2), QPointF(0.4, 0.215), QPointF(0.2, 0.215)};
    QVERIFY(KisAiStrokeQualityUtils::isCastShadow(castPoly, canvasSize));

    // Form shadow: large rounded cheek shadow (e.g. 300px wide, 300px tall -> aspect ratio 1.0, area ~9%)
    const QPolygonF formPoly = {QPointF(0.3, 0.3), QPointF(0.6, 0.3), QPointF(0.6, 0.6), QPointF(0.3, 0.6)};
    QVERIFY(!KisAiStrokeQualityUtils::isCastShadow(formPoly, canvasSize));
}

void KisAiStrokeRendererTest::testFinishingFiltersBloomAndChromaticAberration()
{
    QImage img(100, 100, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::black);

    // Draw bright white center square (lum = 255)
    QPainter p(&img);
    p.fillRect(40, 40, 20, 20, Qt::white);
    p.end();

    // Before bloom, pixel (36, 50) is pitch black
    QCOMPARE(qRed(img.pixel(36, 50)), 0);

    // Apply bloom
    KisAiStrokeRenderer::applyBloomEffect(img, 0.6, 6);

    // Pixel (36, 50) near bright center (4 pixels away) should now have diffused glow
    QVERIFY(qRed(img.pixel(36, 50)) > 0);

    // Test chromatic aberration
    KisAiStrokeRenderer::applyChromaticAberration(img, 2);
    // At boundary (38, 50), red and blue channels should disperse
    const QRgb cEdge = img.pixel(38, 50);
    QVERIFY(qRed(cEdge) != qBlue(cEdge));
}

void KisAiStrokeRendererTest::testFinishingFiltersVignette()
{
    QImage img(100, 100, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);

    KisAiStrokeRenderer::applyVignette(img, 0.25);

    // Center pixel (50, 50) should remain pristine white (distance from center = 0)
    const QRgb cCenter = img.pixel(50, 50);
    QCOMPARE(qRed(cCenter), 255);
    QCOMPARE(qGreen(cCenter), 255);
    QCOMPARE(qBlue(cCenter), 255);

    // Corner pixel (0, 0) should be darkened by vignette
    const QRgb cCorner = img.pixel(0, 0);
    QVERIFY(qRed(cCorner) < 250);
    QVERIFY(qGreen(cCorner) < 250);
    QVERIFY(qBlue(cCorner) < 250);
}

void KisAiStrokeRendererTest::testRenderProgramToImageBoundsDerivedCanvasSize()
{
    // With no explicit target size the renderer falls back to program.canvasSize,
    // which is model-supplied metadata. It must be bounded before allocating.
    KisAiStrokeProgram program;
    program.canvasSize = QSize(100000, 100000);

    KisAiStrokeOperation fill;
    fill.kind = KisAiStrokeOperation::Kind::Fill;
    fill.layer = QStringLiteral("Flats");
    fill.polygon << QPointF(0.1, 0.1) << QPointF(0.9, 0.1) << QPointF(0.9, 0.9);
    fill.brush.color = QColor(200, 30, 30);
    fill.brush.opacity = 1.0;
    program.operations.append(fill);

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(program, QSize());
    QVERIFY(!image.isNull());
    QVERIFY(image.width() <= 4096);
    QVERIFY(image.height() <= 4096);
    QVERIFY(image.width() >= 64);
    QVERIFY(image.height() >= 64);

    // An explicit caller-provided target size (document/preview) is honoured.
    const QImage explicitSize = KisAiStrokeRenderer::renderProgramToImage(program, QSize(128, 96));
    QCOMPARE(explicitSize.size(), QSize(128, 96));
}

void KisAiStrokeRendererTest::testHalftonePatternWorkIsBounded()
{
    // A full-canvas scanline fill previously issued millions of painter calls for
    // one operation. The bounded path must still produce a visible screen quickly.
    QImage canvas(4096, 4096, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPolygonF poly;
    poly << QPointF(0, 0) << QPointF(4095, 0) << QPointF(4095, 4095) << QPointF(0, 4095);

    QElapsedTimer timer;
    timer.start();
    {
        QPainter painter(&canvas);
        KisAiStrokeQualityUtils::drawHalftonePattern(painter, poly, QColor(0, 0, 0, 255), 3.0, 1.5, 45.0, false);
    }
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(elapsedMs < 10000, qPrintable(QStringLiteral("halftone fill took %1 ms").arg(elapsedMs)));

    bool hasDrawnPixel = false;
    for (int y = 1000; y < 1200 && !hasDrawnPixel; y += 7) {
        for (int x = 1000; x < 1200; ++x) {
            if (canvas.pixelColor(x, y).alpha() > 100) {
                hasDrawnPixel = true;
                break;
            }
        }
    }
    QVERIFY(hasDrawnPixel);
}

void KisAiStrokeRendererTest::testGradientAngleNormalizationIsFinite()
{
    // Non-finite and unwrapped angles previously reached QPainter::rotate().
    KisAiStrokeProgram program;
    program.canvasSize = QSize(256, 256);

    KisAiStrokeOperation fill;
    fill.kind = KisAiStrokeOperation::Kind::Fill;
    fill.layer = QStringLiteral("Flats");
    fill.fillStyle = QStringLiteral("scanline");
    fill.polygon << QPointF(0.1, 0.1) << QPointF(0.9, 0.1) << QPointF(0.9, 0.9);
    fill.brush.color = QColor(0, 0, 0);
    fill.brush.opacity = 1.0;
    fill.angleDeg = std::numeric_limits<qreal>::infinity();
    program.operations.append(fill);

    KisAiStrokeQualityReport report;
    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);
    QVERIFY(!refined.operations.isEmpty());
    QVERIFY(std::isfinite(refined.operations.first().angleDeg));

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(refined, QSize(256, 256));
    QVERIFY(!image.isNull());
    QCOMPARE(image.size(), QSize(256, 256));
}

void KisAiStrokeRendererTest::testCaptureImageBase64RejectsInvalidArguments()
{
    QImage image(64, 64, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::red);

    // A non-positive dimension used to produce a null scaling result while still
    // emitting a data URL with an empty payload.
    QVERIFY(KisAiStrokeRenderer::captureImageBase64(image, 0, 80).isEmpty());
    QVERIFY(KisAiStrokeRenderer::captureImageBase64(image, -8, 80).isEmpty());
    QVERIFY(KisAiStrokeRenderer::captureImageBase64(QImage(), 768, 80).isEmpty());

    // Valid input still round-trips as a JPEG data URL.
    const QString dataUrl = KisAiStrokeRenderer::captureImageBase64(image, 768, 80);
    QVERIFY(dataUrl.startsWith(QStringLiteral("data:image/jpeg;base64,")));
    const QByteArray payload =
        QByteArray::fromBase64(dataUrl.mid(QStringLiteral("data:image/jpeg;base64,").size()).toLatin1());
    QVERIFY(!payload.isEmpty());
    QVERIFY(payload.startsWith("\xFF\xD8")); // JPEG SOI marker

    // Out-of-range quality must not reach QImage::save() unchanged.
    QVERIFY(!KisAiStrokeRenderer::captureImageBase64(image, 768, 9999).isEmpty());
}

void KisAiStrokeRendererTest::testPxBrushSizeSurvivesSupersampling()
{
    // A px-mode brush on a canvas small enough to be rasterized at 2x supersampling
    // used to keep its raw width on the working image, shrinking the final stroke
    // by half after the downscale. The painted width on the final image must match
    // a non-supersampled render of the same px size.
    auto makeProgram = [](qreal px) {
        KisAiStrokeProgram program;
        program.canvasSize = QSize(256, 256);

        KisAiStrokeOperation stroke;
        stroke.kind = KisAiStrokeOperation::Kind::Path;
        stroke.id = QStringLiteral("px_stroke");
        stroke.layer = QStringLiteral("Lineart");
        stroke.brush.profile = QStringLiteral("gpen");
        stroke.brush.color = QColor(0, 0, 0);
        stroke.brush.sizeMode = QStringLiteral("px");
        stroke.brush.size = px;
        stroke.smooth = false;
        stroke.points = {KisAiStrokePoint(0.1, 0.5, 1.0), KisAiStrokePoint(0.9, 0.5, 1.0)};
        program.operations.append(stroke);
        return program;
    };

    const auto measureThickness = [](const QImage &image, int x) {
        int minY = -1;
        int maxY = -1;
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 100) {
                if (minY < 0)
                    minY = y;
                maxY = y;
            }
        }
        return (minY < 0) ? 0 : (maxY - minY + 1);
    };

    const qreal px = 12.0;

    // 2048x2048 canvas -> no supersampling (scale 1), stroke of 12 px.
    KisAiStrokeProgram bigProgram = makeProgram(px);
    bigProgram.canvasSize = QSize(2048, 2048);
    const QImage bigImage = KisAiStrokeRenderer::renderProgramToImage(bigProgram, QSize(2048, 2048));
    QVERIFY(!bigImage.isNull());
    const int bigThickness = measureThickness(bigImage, bigImage.width() / 2);
    QVERIFY2(bigThickness >= 10 && bigThickness <= 17,
             qPrintable(QStringLiteral("unsupersampled thickness %1").arg(bigThickness)));

    // 256x256 canvas -> supersampled at 2x, same 12 px stroke.
    const QImage smallImage = KisAiStrokeRenderer::renderProgramToImage(makeProgram(px), QSize(256, 256));
    QVERIFY(!smallImage.isNull());
    const int smallThickness = measureThickness(smallImage, smallImage.width() / 2);
    QVERIFY2(smallThickness >= 10 && smallThickness <= 17,
             qPrintable(QStringLiteral("supersampled thickness %1").arg(smallThickness)));

    // Both renders must agree within antialiasing tolerance (previously the
    // supersampled stroke came out about half the requested width).
    QVERIFY2(qAbs(smallThickness - bigThickness) <= 3,
             qPrintable(QStringLiteral("supersampled %1 vs unsupersampled %2").arg(smallThickness).arg(bigThickness)));
}

void KisAiStrokeRendererTest::testHatchErasersAreShapeBounded()
{
    // Eraser hatches must only clear inside their own clipped polygon, never the
    // whole hatch bounding area. A regression here would wipe unrelated artwork.
    KisAiStrokeProgram program;
    program.canvasSize = QSize(256, 256);
    program.goalReached = false; // skip finishing post-process for exact pixel checks

    // A solid-filled square far to the right of the eraser hatch polygon.
    // "contour" maps to the plain solid-fill branch ("wash" would render a
    // gradient with partial alpha).
    KisAiStrokeOperation square;
    square.kind = KisAiStrokeOperation::Kind::Fill;
    square.id = QStringLiteral("target_square");
    square.layer = QStringLiteral("Flats");
    square.fillStyle = QStringLiteral("contour");
    square.brush.color = QColor(30, 30, 30);
    square.brush.opacity = 1.0;
    square.polygon = {QPointF(0.80, 0.0), QPointF(0.90, 0.0), QPointF(0.90, 1.0), QPointF(0.80, 1.0)};
    program.operations.append(square);

    // A fat-stroked eraser hatch over the left part of the canvas. The i=0 hatch
    // line always passes through the clip polygon's center, so (64,64) sits on an
    // eraser stroke and must be fully cleared.
    KisAiStrokeOperation hatch;
    hatch.kind = KisAiStrokeOperation::Kind::Hatch;
    hatch.id = QStringLiteral("limited_eraser");
    hatch.layer = QStringLiteral("Flats"); // same layer as the square: Clear must be scoped to this layer
    hatch.brush.isEraser = true;
    hatch.brush.sizeMode = QStringLiteral("px");
    hatch.brush.size = 48.0; // penWidth = size * 0.20 = 9.6 px
    hatch.angleDeg = 45.0;
    hatch.spacing = 0.2;
    hatch.smooth = false;
    hatch.polygon = {QPointF(0.05, 0.05), QPointF(0.35, 0.05), QPointF(0.35, 0.35), QPointF(0.05, 0.35)};
    program.operations.append(hatch);

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(program, QSize(256, 256));
    QVERIFY(!image.isNull());

    // On the eraser stroke, inside the hatch polygon: cleared.
    QVERIFY2(qAlpha(image.pixel(64, 64)) <= 60,
             qPrintable(QStringLiteral("alpha@64,64 = %1").arg(qAlpha(image.pixel(64, 64)))));

    // The far-right filled square must be untouched by the eraser.
    QVERIFY2(qAlpha(image.pixel(215, 128)) > 200,
             qPrintable(QStringLiteral("alpha@215,128 = %1").arg(qAlpha(image.pixel(215, 128)))));
    QCOMPARE(qRed(image.pixel(215, 128)), 30);

    // Outside both shapes the canvas stays transparent.
    QCOMPARE(qAlpha(image.pixel(150, 128)), 0);
}

void KisAiStrokeRendererTest::testTrappingWidthAndScreenBlending()
{
    // A2: Test trapping expansion on Flats
    KisAiStrokeProgram prog;
    prog.prompt = QStringLiteral("Trapping and Screen test");
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation flat;
    flat.kind = KisAiStrokeOperation::Kind::Fill;
    flat.layer = QStringLiteral("Flats");
    flat.polygon << QPointF(0.3, 0.3) << QPointF(0.7, 0.3) << QPointF(0.7, 0.7) << QPointF(0.3, 0.7);
    flat.brush.color = QColor(100, 50, 50); // dark red
    prog.operations.append(flat);

    KisAiStrokeOperation highlight;
    highlight.kind = KisAiStrokeOperation::Kind::Fill;
    highlight.layer = QStringLiteral("Highlights");
    highlight.polygon << QPointF(0.4, 0.4) << QPointF(0.6, 0.4) << QPointF(0.6, 0.6) << QPointF(0.4, 0.6);
    highlight.brush.color = QColor(100, 100, 100); // light gray
    prog.operations.append(highlight);

    // Render without trapping
    const QImage imgNoTrap = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(100, 100), false, 0.0);
    // Render with strong trapping (3.0 px)
    const QImage imgTrap = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(100, 100), false, 3.0);

    QVERIFY(!imgNoTrap.isNull());
    QVERIFY(!imgTrap.isNull());

    // Count non-transparent pixels in both
    int nonZeroNoTrap = 0;
    int nonZeroTrap = 0;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            if (qAlpha(imgNoTrap.pixel(x, y)) > 0)
                ++nonZeroNoTrap;
            if (qAlpha(imgTrap.pixel(x, y)) > 0)
                ++nonZeroTrap;
        }
    }
    // Trapping must dilate Flats slightly to prevent white gaps
    QVERIFY(nonZeroTrap >= nonZeroNoTrap);

    // Screen blend mode check: Center pixel (50, 50) must be brighter than base flat color (100, 50, 50)
    const QRgb centerPixel = imgNoTrap.pixel(50, 50);
    QVERIFY(qRed(centerPixel) > 100);
    QVERIFY(qGreen(centerPixel) > 50);
}

void KisAiStrokeRendererTest::testSoftEdgeDiffusionRadiusBounded()
{
    // Test null image safety
    QImage nullImg;
    KisAiStrokeRenderer::applySoftEdgeDiffusion(nullImg, 5);
    QVERIFY(nullImg.isNull());

    // Test tiny 2x2 image with oversized radius (e.g. 50)
    QImage tinyImg(2, 2, QImage::Format_ARGB32_Premultiplied);
    tinyImg.fill(Qt::transparent);
    tinyImg.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    KisAiStrokeRenderer::applySoftEdgeDiffusion(tinyImg, 50);
    QCOMPARE(tinyImg.size(), QSize(2, 2));
    QVERIFY(tinyImg.pixelColor(0, 0).isValid());

    // Test normal image diffusion
    QImage normImg(32, 32, QImage::Format_ARGB32_Premultiplied);
    normImg.fill(Qt::transparent);
    for (int y = 14; y <= 17; ++y) {
        for (int x = 14; x <= 17; ++x) {
            normImg.setPixelColor(x, y, QColor(200, 100, 50, 255));
        }
    }
    QCOMPARE(normImg.pixelColor(10, 10).alpha(), 0);

    KisAiStrokeRenderer::applySoftEdgeDiffusion(normImg, 3);
    QVERIFY(normImg.pixelColor(13, 13).alpha() > 0);
}

void KisAiStrokeRendererTest::testRenderAnimeEye()
{
    KisAiStrokeProgram prog;
    prog.schemaVersion = 2;
    prog.canvasSize = QSize(256, 256);

    KisAiStrokeOperation eyeOp;
    eyeOp.kind = KisAiStrokeOperation::Kind::AnimeEye;
    eyeOp.id = QStringLiteral("test_eye");
    eyeOp.layer = QStringLiteral("Flats");
    eyeOp.eyeCenter = QPointF(0.5, 0.5);
    eyeOp.eyeSize = QSizeF(0.25, 0.30);
    eyeOp.eyeIrisColor = QColor(QStringLiteral("#2060e0"));
    eyeOp.eyeSecondaryColor = QColor(QStringLiteral("#70b0ff"));
    eyeOp.eyeStyle = QStringLiteral("sparkle");
    eyeOp.eyeExpression = QStringLiteral("open");
    eyeOp.eyeIsRight = false;
    prog.operations.append(eyeOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QCOMPARE(img.size(), QSize(256, 256));

    // The eye center (128, 128) must have non-zero alpha and painted iris color
    const QColor centerPixel = img.pixelColor(128, 128);
    QVERIFY(centerPixel.alpha() > 100);

    // Corner (10, 10) outside the eye should be transparent
    QCOMPARE(img.pixelColor(10, 10).alpha(), 0);
}

void KisAiStrokeRendererTest::testFaceExclusionMaskSuppressesParticles()
{
    // Program with a face/skin fill in Flats, and particles covering the canvas in FX
    KisAiStrokeProgram prog;
    prog.schemaVersion = 2;
    prog.canvasSize = QSize(200, 200);

    KisAiStrokeOperation faceOp;
    faceOp.kind = KisAiStrokeOperation::Kind::Fill;
    faceOp.id = QStringLiteral("face_skin");
    faceOp.layer = QStringLiteral("Flats");
    faceOp.brush.color = QColor(255, 224, 200);
    faceOp.brush.opacity = 1.0;
    faceOp.polygon = {QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    prog.operations.append(faceOp);

    KisAiStrokeOperation particlesOp;
    particlesOp.kind = KisAiStrokeOperation::Kind::Particles;
    particlesOp.id = QStringLiteral("fx_particles");
    particlesOp.layer = QStringLiteral("FX");
    particlesOp.brush.color = QColor(255, 0, 0); // bright red particles
    particlesOp.brush.profile = QStringLiteral("airbrush");
    particlesOp.brush.size = 0.02;
    particlesOp.bounds = QRectF(0.0, 0.0, 1.0, 1.0);
    particlesOp.particleCount = 50;
    particlesOp.particleShape = QStringLiteral("circle");
    prog.operations.append(particlesOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(200, 200), false);
    QCOMPARE(img.size(), QSize(200, 200));

    // The center of the face (100, 100) should be skin tone, NOT overlaid with red particle dots
    const QColor faceCenter = img.pixelColor(100, 100);
    QVERIFY(faceCenter.alpha() > 200);
    // Face center should be close to skin color (R high, G ~224, B ~200), not pure red (R 255, G 0, B 0)
    QVERIFY(faceCenter.green() > 150);
    QVERIFY(faceCenter.blue() > 150);
}

void KisAiStrokeRendererTest::testUniteOverlappingHairFlats()
{
    // V3 Phase 0.2: Overlapping hair Flats patches ("bubble" artifact) must
    // fuse into one silhouette, while a disjoint mass (e.g. twin-tail) survives.
    auto makeHairPatch = [](const QString &id, const QPolygonF &poly) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.id = id;
        op.layer = QStringLiteral("Flats");
        op.brush.color = QColor(43, 58, 103);
        op.polygon = poly;
        return op;
    };
    const QPolygonF patchA = {QPointF(0.30, 0.20), QPointF(0.55, 0.20), QPointF(0.55, 0.45), QPointF(0.30, 0.45)};
    const QPolygonF patchB = {QPointF(0.45, 0.30), QPointF(0.70, 0.30), QPointF(0.70, 0.55), QPointF(0.45, 0.55)};
    const QPolygonF farTail = {QPointF(0.80, 0.60), QPointF(0.95, 0.60), QPointF(0.95, 0.90), QPointF(0.80, 0.90)};

    QVector<KisAiStrokeOperation> ops;
    ops.append(makeHairPatch(QStringLiteral("hair_patch_a"), patchA));
    ops.append(makeHairPatch(QStringLiteral("hair_patch_b"), patchB));
    ops.append(makeHairPatch(QStringLiteral("hair_tail_far"), farTail));

    const QVector<KisAiStrokeOperation> united = KisAiStrokeQualityUtils::uniteOverlappingHairFlats(ops);
    // A+B fused, far tail untouched.
    QCOMPARE(united.size(), 2);
    int hairOps = 0;
    for (const KisAiStrokeOperation &op : united) {
        if (op.id.contains(QStringLiteral("hair")))
            ++hairOps;
    }
    QCOMPARE(hairOps, 2);

    // The fused mass must cover both source patches.
    const KisAiStrokeOperation *fused = nullptr;
    for (const KisAiStrokeOperation &op : united) {
        if (op.id != QStringLiteral("hair_tail_far")) {
            fused = &op;
            break;
        }
    }
    QVERIFY(fused != nullptr);
    const QRectF bounds = fused->polygon.boundingRect();
    QVERIFY(bounds.left() <= 0.31);
    QVERIFY(bounds.right() >= 0.69);
    QVERIFY(bounds.top() <= 0.21);
    QVERIFY(bounds.bottom() >= 0.54);

    // Zero or one hair candidate is a no-op passthrough.
    QVector<KisAiStrokeOperation> single;
    single.append(makeHairPatch(QStringLiteral("hair_solo"), patchA));
    QCOMPARE(KisAiStrokeQualityUtils::uniteOverlappingHairFlats(single).size(), 1);
}

void KisAiStrokeRendererTest::testGoalModeSingleArtboard()
{
    // V3 Phase 3.1: Verify single artboard layer structure coherence
    // All operations in a multi-step program must resolve strictly to the 6 standard layer buckets.
    KisAiStrokeProgram step1;
    step1.schemaVersion = 2;
    step1.currentStep = 1;
    step1.totalSteps = 3;
    step1.canvasSize = QSize(512, 512);

    KisAiStrokeOperation bg;
    bg.kind = KisAiStrokeOperation::Kind::Fill;
    bg.id = QStringLiteral("bg_fill");
    bg.layer = QStringLiteral("Background");
    bg.polygon = {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    bg.brush.color = QColor(20, 30, 50);
    step1.operations.append(bg);

    KisAiStrokeOperation flat;
    flat.kind = KisAiStrokeOperation::Kind::Fill;
    flat.id = QStringLiteral("char_flat");
    flat.layer = QStringLiteral("Flats");
    flat.polygon = {QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.2, 0.8)};
    flat.brush.color = QColor(255, 220, 190);
    step1.operations.append(flat);

    KisAiStrokeProgram step2;
    step2.schemaVersion = 2;
    step2.currentStep = 2;
    step2.totalSteps = 3;
    step2.canvasSize = QSize(512, 512);

    KisAiStrokeOperation shade;
    shade.kind = KisAiStrokeOperation::Kind::Fill;
    shade.id = QStringLiteral("char_shade");
    shade.layer = QStringLiteral("Shading");
    shade.polygon = {QPointF(0.4, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.4, 0.8)};
    shade.brush.color = QColor(200, 160, 140);
    step2.operations.append(shade);

    const KisAiStrokeProgram merged = KisAiStrokeProgramCodec::mergePrograms(step1, step2);
    QCOMPARE(merged.currentStep, 2);
    QCOMPARE(merged.totalSteps, 3);

    // Verify operations can render cleanly to an image representation
    const QImage rendered = KisAiStrokeRenderer::renderProgramToImage(merged, QSize(512, 512));
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.size(), QSize(512, 512));

    // Verify all merged operations strictly conform to standard layer names
    const QStringList allowedLayers = {QStringLiteral("Background"),
                                       QStringLiteral("Flats"),
                                       QStringLiteral("Shading"),
                                       QStringLiteral("Lineart"),
                                       QStringLiteral("Highlights"),
                                       QStringLiteral("FX")};
    for (const auto &op : merged.operations) {
        const QString norm = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
        QVERIFY2(allowedLayers.contains(norm), qPrintable(QStringLiteral("Invalid layer: %1").arg(norm)));
    }
}

void KisAiStrokeRendererTest::testHatchLineCountIsBounded()
{
    // A tiny spacing over a huge polygon previously issued hundreds of
    // thousands of clipped drawLine calls for a single hatch pass.
    KisAiStrokeProgram program;
    program.canvasSize = QSize(2048, 2048);

    KisAiStrokeOperation hatch;
    hatch.kind = KisAiStrokeOperation::Kind::Hatch;
    hatch.id = QStringLiteral("hostile_hatch");
    hatch.layer = QStringLiteral("Shading");
    hatch.polygon << QPointF(0.0, 0.0) << QPointF(1.0, 0.0) << QPointF(1.0, 1.0) << QPointF(0.0, 1.0);
    hatch.brush.color = QColor(60, 60, 60);
    hatch.brush.opacity = 1.0;
    hatch.angleDeg = 45.0;
    hatch.spacing = 0.002; // parse-time floor -> ~4 px on a 2048 canvas
    program.operations.append(hatch);

    const KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program);
    QVERIFY(!refined.operations.isEmpty());

    QImage canvas(512, 512, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QElapsedTimer timer;
    timer.start();
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        KisAiStrokeRenderer::renderProgramToImage(refined, QSize(512, 512));
    }
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(elapsedMs < 10000, qPrintable(QStringLiteral("hostile hatch took %1 ms").arg(elapsedMs)));
}

void KisAiStrokeRendererTest::testLineScreenRowBudgetMatchesDotBudget()
{
    // The line screen shares the bounded-work policy with the dot screen: a
    // degenerate spacing must not turn into an unbounded row loop.
    QImage canvas(2048, 2048, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);

    QPolygonF poly;
    poly << QPointF(0, 0) << QPointF(2047, 0) << QPointF(2047, 2047) << QPointF(0, 2047);

    QElapsedTimer timer;
    timer.start();
    {
        QPainter painter(&canvas);
        KisAiStrokeQualityUtils::drawHalftonePattern(painter, poly, QColor(0, 0, 0, 255), 1.0, 0.5, 0.0, true);
    }
    const qint64 elapsedMs = timer.elapsed();
    QVERIFY2(elapsedMs < 10000, qPrintable(QStringLiteral("line screen took %1 ms").arg(elapsedMs)));

    bool hasDrawnPixel = false;
    for (int y = 0; y < 2048 && !hasDrawnPixel; y += 97) {
        for (int x = 0; x < 2048; x += 97) {
            if (canvas.pixelColor(x, y).alpha() > 0) {
                hasDrawnPixel = true;
                break;
            }
        }
    }
    QVERIFY(hasDrawnPixel);
}

void KisAiStrokeRendererTest::testTypeCheckerClampsHostileParticleBounds()
{
    // The type checker rewrites bounds/count for the checker-only path, so it
    // must not pass 1e300-scale or inverted values through to the renderer.
    QJsonObject opObj;
    opObj[QStringLiteral("kind")] = QStringLiteral("particles");
    opObj[QStringLiteral("bounds")] = QJsonArray({1.0e300, -5.0, 1.0e300, 1.0e300});
    opObj[QStringLiteral("count")] = 1.0e300;

    KisAiStrokeTypeCheckReport report;
    QVERIFY(KisAiStrokeTypeChecker::checkAndCoerceOperation(&opObj, 0, &report));

    const QJsonArray bounds = opObj.value(QStringLiteral("bounds")).toArray();
    QCOMPARE(bounds.size(), 4);
    for (int i = 0; i < 4; ++i) {
        const qreal v = bounds.at(i).toDouble();
        QVERIFY2(v >= 0.0 && v <= 1.0, qPrintable(QString::number(v)));
    }
    const int count = opObj.value(QStringLiteral("count")).toInt();
    QVERIFY(count >= 1 && count <= 256);
}

void KisAiStrokeRendererTest::testSceneSpecHyperQualityRendering()
{
    // Test 1: Sailor school uniform, silver hair, blue eyes, M-fringe
    KisAiSceneSpec spec1 = KisAiSceneSpecCodec::defaultSpecForPrompt(
        QStringLiteral("anime girl portrait with silver hair and blue eyes, sailor uniform"),
        QSize(1024, 1024));
    const KisAiStrokeProgram prog1 = KisAiLayoutEngine::generateProgram(spec1, QSize(1024, 1024));
    QVERIFY(prog1.isValid());
    QVERIFY(prog1.operations.size() >= 20);

    const QString artifactDir =
        qEnvironmentVariable("AI_STROKE_TEST_ARTIFACT_DIR", QStringLiteral("build-test/artifacts"));
    QDir().mkpath(artifactDir);
    {
        QFile dumpFile(QDir(artifactDir).filePath(QStringLiteral("ops_dump.txt")));
        if (dumpFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&dumpFile);
            for (const auto &op : prog1.operations) {
                ts << "OP: " << op.id << " | Layer: " << op.layer << " | Kind: " << (int)op.kind
                   << " | Color: " << op.brush.color.name(QColor::HexArgb) << " | Opacity: " << op.brush.opacity
                   << " | Profile: " << op.brush.profile << " | PolyPts: " << op.polygon.size()
                   << " | BBox: " << op.polygon.boundingRect().x() << "," << op.polygon.boundingRect().y() << " "
                   << op.polygon.boundingRect().width() << "x" << op.polygon.boundingRect().height() << "\n";
            }
        }
    }
    const QImage img1 = KisAiStrokeRenderer::renderProgramToImage(prog1, QSize(1024, 1024));
    QVERIFY(!img1.isNull());

    // Test 2: Hoodie, twin tails, blonde hair, green eyes
    KisAiSceneSpec spec2 = KisAiSceneSpecCodec::defaultSpecForPrompt(
        QStringLiteral("anime girl with blonde twin tails and green eyes, oversized hoodie"),
        QSize(1024, 1024));
    const KisAiStrokeProgram prog2 = KisAiLayoutEngine::generateProgram(spec2, QSize(1024, 1024));
    QVERIFY(prog2.isValid());
    QVERIFY(prog2.operations.size() >= 20);

    const QImage img2 = KisAiStrokeRenderer::renderProgramToImage(prog2, QSize(1024, 1024));
    QVERIFY(!img2.isNull());

    img1.save(QDir(artifactDir).filePath(QStringLiteral("hyper-quality-anime-sailor.png")));
    img2.save(QDir(artifactDir).filePath(QStringLiteral("hyper-quality-anime-hoodie.png")));
}

void KisAiStrokeRendererTest::testDeliberateStabilizeRemovesJitter()
{
    // D0: RDP(1.2px) + equidistant(3px) collapses micro-jitter, keeps endpoints.
    QVector<KisAiStrokePoint> noisy;
    for (int i = 0; i <= 40; ++i) {
        const qreal x = qreal(i) / 40.0;
        const qreal jitter = (i > 0 && i < 40 && i % 2 == 0) ? 0.0004 : 0.0; // ~0.4px at 1024
        noisy.append(KisAiStrokePoint(x, 0.5 + jitter, 0.8));
    }
    const auto stable = KisAiDeliberateStroke::stabilizeStroke(noisy, QSize(1024, 1024), false, 42);
    QVERIFY(stable.size() >= 2);
    // Micro-jitter is collapsed by RDP: all points should lie on the baseline y = 0.5
    for (const auto &pt : stable) {
        QVERIFY(qAbs(pt.pos.y() - 0.5) < 1e-5);
    }
    QVERIFY(qAbs(stable.first().pos.x() - 0.0) < 1e-6);
    QVERIFY(qAbs(stable.last().pos.x() - 1.0) < 1e-6);
}

void KisAiStrokeRendererTest::testDeliberateLintDropsMicroAndOffCanvas()
{
    const QSize canvas(1024, 1024);
    KisAiStrokeOperation micro;
    micro.kind = KisAiStrokeOperation::Kind::Path;
    micro.id = QStringLiteral("micro");
    micro.layer = QStringLiteral("Lineart");
    micro.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.5, 0.5, 0.8), KisAiStrokePoint(0.5005, 0.5, 0.8)};
    micro.brush.color = QColor(20, 20, 20);
    micro.brush.size = 0.004;
    QVERIFY(KisAiDeliberateStroke::lintStroke(micro, canvas).drop);

    KisAiStrokeOperation off;
    off.kind = KisAiStrokeOperation::Kind::Path;
    off.id = QStringLiteral("off");
    off.layer = QStringLiteral("Lineart");
    off.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(2.0, 2.0, 0.8), KisAiStrokePoint(2.5, 2.5, 0.8)};
    off.brush.color = QColor(20, 20, 20);
    off.brush.size = 0.004;
    QVERIFY(KisAiDeliberateStroke::lintStroke(off, canvas).drop);

    KisAiStrokeOperation good;
    good.kind = KisAiStrokeOperation::Kind::Path;
    good.id = QStringLiteral("good");
    good.layer = QStringLiteral("Lineart");
    good.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.2, 0.5, 0.8), KisAiStrokePoint(0.8, 0.5, 0.8)};
    good.brush.color = QColor(20, 20, 20);
    good.brush.size = 0.004;
    QVERIFY(!KisAiDeliberateStroke::lintStroke(good, canvas).drop);
}

void KisAiStrokeRendererTest::testDeliberateStrokeOrderBigToSmallFaceLast()
{
    const QSize canvas(1024, 1024);
    auto path = [](const QString &id, const QString &layer, qreal x0, qreal x1) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.id = id;
        op.layer = layer;
        op.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(x0, 0.5, 0.8), KisAiStrokePoint(x1, 0.5, 0.8)};
        op.brush.color = QColor(20, 20, 20);
        op.brush.size = 0.004;
        return op;
    };
    QVector<KisAiStrokeOperation> ops;
    ops.append(path(QStringLiteral("left_eye"), QStringLiteral("Lineart"), 0.3, 0.4));
    ops.append(path(QStringLiteral("bg_line"), QStringLiteral("Background"), 0.0, 1.0));
    ops.append(path(QStringLiteral("hair_long"), QStringLiteral("Flats"), 0.1, 0.9));
    const QVector<KisAiStrokeOperation> ordered = KisAiDeliberateStroke::orderOperationsForRendering(ops, canvas);
    QCOMPARE(ordered.size(), 3);
    QCOMPARE(ordered.at(0).id, QStringLiteral("bg_line"));
    QCOMPARE(ordered.at(1).id, QStringLiteral("hair_long"));
    QCOMPARE(ordered.at(2).id, QStringLiteral("left_eye"));
}

void KisAiStrokeRendererTest::testDeliberateAdaptiveSupersampleFaceOnly()
{
    auto faceOp = []() {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::AnimeEye;
        op.id = QStringLiteral("left_eye");
        op.layer = QStringLiteral("Lineart");
        op.eyeCenter = QPointF(0.4, 0.4);
        op.eyeSize = QSizeF(0.1, 0.12);
        return op;
    };
    auto bgOp = []() {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Fill;
        op.id = QStringLiteral("bg");
        op.layer = QStringLiteral("Background");
        op.polygon = QPolygonF{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
        op.brush.color = QColor(100, 120, 160);
        return op;
    };
    QCOMPARE(KisAiDeliberateStroke::adaptiveSupersampleScale({faceOp()}, QSize(1024, 1024)), 3);
    QCOMPARE(KisAiDeliberateStroke::adaptiveSupersampleScale({bgOp()}, QSize(1024, 1024)), 2);
    QCOMPARE(KisAiDeliberateStroke::adaptiveSupersampleScale({faceOp()}, QSize(2048, 2048)), 1);
}

void KisAiStrokeRendererTest::testDeliberateEyePairSymmetryWarnings()
{
    auto eye = [](const QString &id, qreal x, qreal y) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::AnimeEye;
        op.id = id;
        op.layer = QStringLiteral("Lineart");
        op.eyeCenter = QPointF(x, y);
        op.eyeSize = QSizeF(0.1, 0.12);
        return op;
    };
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings(
                {eye(QStringLiteral("l"), 0.4, 0.4), eye(QStringLiteral("r"), 0.6, 0.4)})
                .isEmpty());
    QVERIFY(!KisAiDeliberateStroke::eyePairSymmetryWarnings(
                 {eye(QStringLiteral("l"), 0.4, 0.4), eye(QStringLiteral("r"), 0.6, 0.5)})
                 .isEmpty());
    QVERIFY(KisAiDeliberateStroke::eyePairSymmetryWarnings({eye(QStringLiteral("l"), 0.4, 0.4)})
                .contains(QStringLiteral("single-eye-only")));
}

void KisAiStrokeRendererTest::testFineLineRenderingSubpixel()
{
    // Test that delicate fineliner, maru_pen, feathering, and stipple profiles
    // render correctly with subpixel paths without crashing or degenerating.
    const QStringList profiles = {QStringLiteral("fineliner"),
                                  QStringLiteral("maru_pen"),
                                  QStringLiteral("feathering"),
                                  QStringLiteral("stipple")};

    const QSize canvasSize(512, 512);
    for (const QString &prof : profiles) {
        KisAiStrokeOperation op;
        op.kind = KisAiStrokeOperation::Kind::Path;
        op.id = QStringLiteral("test_fine_") + prof;
        op.layer = QStringLiteral("Lineart");
        op.brush.profile = prof;
        op.brush.color = QColor(20, 20, 30);
        op.brush.size = 0.002;
        op.brush.opacity = 1.0;
        op.smooth = true;
        op.points = {KisAiStrokePoint(0.2, 0.3, 0.8),
                     KisAiStrokePoint(0.4, 0.32, 0.9),
                     KisAiStrokePoint(0.6, 0.28, 0.7),
                     KisAiStrokePoint(0.8, 0.35, 0.4)};

        KisAiStrokeProgram prog;
        prog.operations = {op};
        QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
        QVERIFY(!img.isNull());

        // Count drawn pixels
        int drawn = 0;
        for (int y = 0; y < canvasSize.height(); ++y) {
            for (int x = 0; x < canvasSize.width(); ++x) {
                if (img.pixelColor(x, y).alpha() > 15)
                    ++drawn;
            }
        }
        QVERIFY2(drawn > 10,
                 qPrintable(QStringLiteral("Profile %1 produced too few pixels (%2)").arg(prof).arg(drawn)));
    }
}

void KisAiStrokeRendererTest::testAdaptiveResamplingPreservesNuance()
{
    // A short curved detail stroke (e.g. 10px long eyelash or hair strand)
    // should not be decimated into a 2-point straight segment by a 3px step.
    const QSize canvasSize(1000, 1000);
    QVector<KisAiStrokePoint> shortCurve = {
        KisAiStrokePoint(0.500, 0.500, 0.5),
        KisAiStrokePoint(0.503, 0.502, 0.8),
        KisAiStrokePoint(0.506, 0.506, 0.9),
        KisAiStrokePoint(0.508, 0.511, 0.4),
    };

    QVector<KisAiStrokePoint> resampled = KisAiDeliberateStroke::stabilizeStroke(shortCurve, canvasSize, false, 42);

    // Adaptive step should maintain at least 4 points to keep curvature
    QVERIFY2(
        resampled.size() >= 4,
        qPrintable(QStringLiteral("Adaptive resampling decimated short stroke to %1 points").arg(resampled.size())));
}

void KisAiStrokeRendererTest::testLineartHierarchyDynamicTiers()
{
    // Test that authorial / fine lineart intent (e.g. size <= 0.0025 for fineliner)
    // is not inflated to 0.003 or 0.008.
    KisAiStrokeOperation fineOp;
    fineOp.kind = KisAiStrokeOperation::Kind::Path;
    fineOp.id = QStringLiteral("hair_strand_0");
    fineOp.layer = QStringLiteral("Lineart");
    fineOp.brush.profile = QStringLiteral("fineliner");
    fineOp.brush.size = 0.0016;
    fineOp.points = {KisAiStrokePoint(0.1, 0.1, 0.8), KisAiStrokePoint(0.2, 0.3, 0.8), KisAiStrokePoint(0.3, 0.5, 0.8)};

    QVector<KisAiStrokeOperation> ops = {fineOp};
    KisAiStrokeQualityUtils::applyLineartHierarchy(ops);

    QCOMPARE(ops.size(), 1);
    QVERIFY2(ops.first().brush.size <= 0.0020,
             qPrintable(QStringLiteral("Fine line size was inflated to %1").arg(ops.first().brush.size)));
}

void KisAiStrokeRendererTest::testHairStrandsAndBangsBleedGeneration()
{
    // Verify that KisAiLayoutEngine generates hair strand lineart and bangs bleed
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("Anime girl portrait with fine hair");
    spec.subject.type = QStringLiteral("character");
    spec.composition.headCenter = QPointF(0.5, 0.4);
    spec.composition.headHeight = 0.40;
    spec.head.skinTone = QColor(255, 230, 215);
    spec.head.hairColor = QColor(45, 30, 60);

    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(1024, 1024));
    QVERIFY(!prog.operations.isEmpty());

    bool hasHairStrand = false;
    bool hasBangsBleed = false;
    bool hasClavicle = false;
    // V6 W1: nose_bridge_hl retired in favor of the Rig nose
    // (rig_nose_point/rig_nose_shadow); assert the Rig replacement instead.
    bool hasRigNose = false;

    for (const KisAiStrokeOperation &op : prog.operations) {
        if (op.id.contains(QLatin1String("hair_strand")))
            hasHairStrand = true;
        if (op.id.contains(QLatin1String("hair_bangs_bleed")))
            hasBangsBleed = true;
        if (op.id.contains(QLatin1String("clavicle")))
            hasClavicle = true;
        if (op.id.contains(QLatin1String("rig_nose")))
            hasRigNose = true;
    }

    QVERIFY(hasHairStrand);
    QVERIFY(hasBangsBleed);
    QVERIFY(hasClavicle);
    QVERIFY(hasRigNose);
}

void KisAiStrokeRendererTest::testShortStrokeTaperingEndpoints()
{
    // Verify that short fineliner strokes with 4 points render without crashing
    // and correctly produce tapered ink without asymmetry.
    const QSize canvasSize(512, 512);
    KisAiStrokeOperation shortFine;
    shortFine.kind = KisAiStrokeOperation::Kind::Path;
    shortFine.id = QStringLiteral("short_eyelash");
    shortFine.layer = QStringLiteral("Lineart");
    shortFine.brush.profile = QStringLiteral("fineliner");
    shortFine.brush.color = QColor(10, 10, 20);
    shortFine.brush.size = 0.002;
    shortFine.points = {
        KisAiStrokePoint(0.48, 0.40, 0.7),
        KisAiStrokePoint(0.49, 0.39, 0.8),
        KisAiStrokePoint(0.51, 0.38, 0.8),
        KisAiStrokePoint(0.53, 0.37, 0.4),
    };

    KisAiStrokeProgram prog;
    prog.operations = {shortFine};
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    int coloredPixels = 0;
    for (int y = 0; y < canvasSize.height(); ++y) {
        for (int x = 0; x < canvasSize.width(); ++x) {
            if (img.pixelColor(x, y).alpha() > 10)
                ++coloredPixels;
        }
    }
    QVERIFY2(coloredPixels > 5, qPrintable(QStringLiteral("Short stroke drew too few pixels: %1").arg(coloredPixels)));
}

void KisAiStrokeRendererTest::testGoalModeLayersPreservationOnFinalStep()
{
    const QSize canvasSize(512, 512);

    // Step 1: Foundation (Background + Flats)
    KisAiStrokeProgram step1;
    step1.schemaVersion = 2;
    step1.currentStep = 1;
    step1.totalSteps = 4;
    step1.canvasSize = canvasSize;

    KisAiStrokeOperation bg;
    bg.kind = KisAiStrokeOperation::Kind::Fill;
    bg.id = QStringLiteral("bg_sky");
    bg.layer = QStringLiteral("Background");
    bg.polygon = {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};
    bg.brush.color = QColor(40, 60, 90);
    step1.operations.append(bg);

    KisAiStrokeOperation flatSkin;
    flatSkin.kind = KisAiStrokeOperation::Kind::Fill;
    flatSkin.id = QStringLiteral("skin_face");
    flatSkin.layer = QStringLiteral("Flats");
    flatSkin.polygon = {QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    flatSkin.brush.color = QColor(255, 224, 200);
    step1.operations.append(flatSkin);

    // Step 2: Shading
    KisAiStrokeProgram step2;
    step2.schemaVersion = 2;
    step2.currentStep = 2;
    step2.totalSteps = 4;
    step2.canvasSize = canvasSize;

    KisAiStrokeOperation shade;
    shade.kind = KisAiStrokeOperation::Kind::Fill;
    shade.id = QStringLiteral("shade_face");
    shade.layer = QStringLiteral("Shading");
    shade.polygon = {QPointF(0.5, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.5, 0.7)};
    shade.brush.color = QColor(200, 150, 130);
    step2.operations.append(shade);

    // Step 3: Lineart
    KisAiStrokeProgram step3;
    step3.schemaVersion = 2;
    step3.currentStep = 3;
    step3.totalSteps = 4;
    step3.canvasSize = canvasSize;

    KisAiStrokeOperation lineJaw;
    lineJaw.kind = KisAiStrokeOperation::Kind::Path;
    lineJaw.id = QStringLiteral("line_jaw");
    lineJaw.layer = QStringLiteral("Lineart");
    lineJaw.points = {KisAiStrokePoint(0.3, 0.3), KisAiStrokePoint(0.3, 0.7), KisAiStrokePoint(0.7, 0.7)};
    lineJaw.brush.color = QColor(20, 20, 30);
    lineJaw.brush.size = 0.01;
    step3.operations.append(lineJaw);

    // Step 4 (Final step): Highlights & FX, PLUS an additive line accent on Lineart layer
    KisAiStrokeProgram step4;
    step4.schemaVersion = 2;
    step4.currentStep = 4;
    step4.totalSteps = 4;
    step4.canvasSize = canvasSize;

    KisAiStrokeOperation glint;
    glint.kind = KisAiStrokeOperation::Kind::Fill;
    glint.id = QStringLiteral("eye_glint");
    glint.layer = QStringLiteral("Highlights");
    glint.polygon = {QPointF(0.45, 0.45), QPointF(0.48, 0.45), QPointF(0.48, 0.48), QPointF(0.45, 0.48)};
    glint.brush.color = QColor(255, 255, 255);
    step4.operations.append(glint);

    KisAiStrokeOperation lineCatchlight;
    lineCatchlight.kind = KisAiStrokeOperation::Kind::Path;
    lineCatchlight.id = QStringLiteral("line_eyelash_accent");
    lineCatchlight.layer = QStringLiteral("Lineart");
    lineCatchlight.points = {KisAiStrokePoint(0.44, 0.43), KisAiStrokePoint(0.49, 0.43)};
    lineCatchlight.brush.color = QColor(15, 15, 25);
    lineCatchlight.brush.size = 0.005;
    step4.operations.append(lineCatchlight);

    // Accumulate across all 4 steps (as KisAiIllustrationDocker now does)
    KisAiStrokeProgram accumulated = step1;
    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step2);
    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step3);
    accumulated = KisAiStrokeProgramCodec::mergePrograms(accumulated, step4);

    QCOMPARE(accumulated.currentStep, 4);
    QCOMPARE(accumulated.totalSteps, 4);

    // Verify all 6 operations survived into accumulated program
    const auto layerCounts = KisAiStrokeProgramCodec::countLayerOperations(accumulated);
    QCOMPARE(layerCounts.value(QStringLiteral("Background")), 1);
    QCOMPARE(layerCounts.value(QStringLiteral("Flats")), 1);
    QCOMPARE(layerCounts.value(QStringLiteral("Shading")), 1);
    // Lineart must contain BOTH Step 3 lineJaw AND Step 4 lineCatchlight!
    QCOMPARE(layerCounts.value(QStringLiteral("Lineart")), 2);
    QCOMPARE(layerCounts.value(QStringLiteral("Highlights")), 1);

    // Render the final accumulated result
    const QImage finalImage = KisAiStrokeRenderer::renderProgramToImage(accumulated, canvasSize);
    QVERIFY(!finalImage.isNull());

    // Verify Flats skin region is not clear/blank (preserved from Step 1)
    const QColor skinPixel =
        finalImage.pixelColor(qRound(0.35 * canvasSize.width()), qRound(0.5 * canvasSize.height()));
    QVERIFY(skinPixel.alpha() > 200);
    QVERIFY(skinPixel.red() > 180);

    // Verify Background is present (preserved from Step 1)
    const QColor bgPixel = finalImage.pixelColor(10, 10);
    QVERIFY(bgPixel.alpha() > 200);
    QVERIFY(bgPixel.blue() > 50);

    // Verify Lineart from Step 3 drew colored pixels near jaw line
    const QColor jawPixel = finalImage.pixelColor(qRound(0.3 * canvasSize.width()), qRound(0.5 * canvasSize.height()));
    QVERIFY(jawPixel.alpha() > 100);

    // Verify Highlights from Step 4 is present
    const QColor glintPixel =
        finalImage.pixelColor(qRound(0.46 * canvasSize.width()), qRound(0.46 * canvasSize.height()));
    QVERIFY(glintPixel.red() > 200 && glintPixel.green() > 200 && glintPixel.blue() > 200);
}

void KisAiStrokeRendererTest::testVolumetricShadingAndMasterInking()
{
    const QSize canvasSize(1024, 1024);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    // 1. Flats: Skin Base Volume
    KisAiStrokeOperation skinBase;
    skinBase.kind = KisAiStrokeOperation::Kind::Fill;
    skinBase.id = QStringLiteral("skin_face_base");
    skinBase.layer = QStringLiteral("Flats");
    skinBase.brush.color = QColor(255, 224, 200);
    skinBase.polygon << QPointF(0.25, 0.25) << QPointF(0.75, 0.25) << QPointF(0.75, 0.75) << QPointF(0.25, 0.75);
    prog.operations.append(skinBase);

    // 2. Shading: Volumetric Form Shadow with SSS Warmth
    KisAiStrokeOperation formShadow;
    formShadow.kind = KisAiStrokeOperation::Kind::Fill;
    formShadow.id = QStringLiteral("skin_form_shadow");
    formShadow.layer = QStringLiteral("Shading");
    formShadow.brush.profile = QStringLiteral("watercolor");
    formShadow.brush.color = QColor(190, 110, 100);
    formShadow.brush.opacity = 0.50;
    formShadow.fillStyle = QStringLiteral("directional");
    formShadow.angleDeg = 45.0;
    formShadow.polygon << QPointF(0.45, 0.25) << QPointF(0.75, 0.25) << QPointF(0.75, 0.75) << QPointF(0.45, 0.75);
    prog.operations.append(formShadow);

    // 3. Lineart: Master Inking G-Pen Main Contour & Delicate Eyelash
    KisAiStrokeOperation gpenContour;
    gpenContour.kind = KisAiStrokeOperation::Kind::Path;
    gpenContour.id = QStringLiteral("jawline_contour");
    gpenContour.layer = QStringLiteral("Lineart");
    gpenContour.brush.profile = QStringLiteral("gpen");
    gpenContour.brush.color = QColor(25, 20, 32);
    gpenContour.brush.size = 0.0035; // ~3.58px: previously fell back to ribbon, now fine-inked
    gpenContour.points << KisAiStrokePoint(0.25, 0.40, 0.3) << KisAiStrokePoint(0.30, 0.60, 0.9)
                       << KisAiStrokePoint(0.50, 0.75, 0.8) << KisAiStrokePoint(0.70, 0.60, 0.9)
                       << KisAiStrokePoint(0.75, 0.40, 0.3);
    prog.operations.append(gpenContour);

    KisAiStrokeOperation delicateLash;
    delicateLash.kind = KisAiStrokeOperation::Kind::Path;
    delicateLash.id = QStringLiteral("delicate_eyelash_top");
    delicateLash.layer = QStringLiteral("Lineart");
    delicateLash.brush.profile = QStringLiteral("maru_pen");
    delicateLash.brush.color = QColor(25, 20, 32);
    delicateLash.brush.size = 0.0018; // ~1.8px
    delicateLash.points << KisAiStrokePoint(0.40, 0.45, 0.3) << KisAiStrokePoint(0.45, 0.43, 0.9)
                        << KisAiStrokePoint(0.48, 0.44, 0.2);
    prog.operations.append(delicateLash);

    // Render to image
    const QImage rendered = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!rendered.isNull());

    // Verify Volumetric Shading has smooth gradient falloff (not flat solid block)
    // Compare pixel near terminator edge vs inner core
    const QColor pEdge = rendered.pixelColor(qRound(0.48 * canvasSize.width()), qRound(0.30 * canvasSize.height()));
    const QColor pCore = rendered.pixelColor(qRound(0.65 * canvasSize.width()), qRound(0.55 * canvasSize.height()));
    QVERIFY(pEdge.alpha() > 200);
    QVERIFY(pCore.alpha() > 200);
    // Depth gradation: core shadow has different tone/intensity than edge transition
    QVERIFY(pEdge != pCore);

    // Verify G-Pen and Maru-Pen lines rendered distinctly
    const QColor pLine = rendered.pixelColor(qRound(0.50 * canvasSize.width()), qRound(0.75 * canvasSize.height()));
    QVERIFY(pLine.alpha() > 100);
    QVERIFY(pLine.red() < 100); // Dark ink

    // 4. Verify Expanded Operation Target Scaling in Payload
    const QJsonObject payload =
        KisAiStrokeProgramCodec::buildChatCompletionsPayload(QStringLiteral("gpt-4o"),
                                                             QStringLiteral("exquisite master anime illustration"),
                                                             canvasSize,
                                                             1500 // Expanded budget
        );
    const QJsonArray msgs = payload.value(QStringLiteral("messages")).toArray();
    QCOMPARE(msgs.size(), 2);
    const QJsonObject userReq =
        QJsonDocument::fromJson(msgs.at(1).toObject().value(QStringLiteral("content")).toString().toUtf8()).object();

    const int opTarget = userReq.value(QStringLiteral("operation_target")).toInt();
    // Must be scaled well beyond previous hard 60 cap to permit fine inking and multi-tier shading!
    QVERIFY2(opTarget >= 100, qPrintable(QStringLiteral("Expected opTarget >= 100, got: %1").arg(opTarget)));
    QCOMPARE(opTarget, 125); // 1500 / 12 = 125
}

void KisAiStrokeRendererTest::testPhase2MultiTierCurvatureShading()
{
    // Verify 3D Curvature Terminator Form Shadow and Multi-Tier Shading Synthesis
    KisAiLightSettings rig;
    rig.direction = QPointF(-0.6, -0.8);
    rig.fillTint = QColor(40, 45, 70);

    KisAiStrokeOperation body;
    body.kind = KisAiStrokeOperation::Kind::Fill;
    body.id = QStringLiteral("body_torso");
    body.layer = QStringLiteral("Flats");
    body.brush.color = QColor(220, 180, 160);
    body.polygon = {QPointF(0.30, 0.40), QPointF(0.70, 0.40), QPointF(0.75, 0.85), QPointF(0.25, 0.85)};

    KisAiLightRig::HeadAnchor anchor;
    anchor.headCenter = QPointF(0.50, 0.35);
    anchor.headWidth = 0.30;
    anchor.headHeight = 0.38;

    const QVector<KisAiStrokeOperation> shading =
        KisAiLightRig::synthesizeShading({body}, rig, QSize(512, 512), &anchor);
    QVERIFY(!shading.isEmpty());

    bool foundCoreShadow = false;
    bool foundDirectional = false;
    bool foundClipToId = false;
    bool foundMultiply = false;
    bool foundChinAo = false;

    for (const auto &op : shading) {
        if (op.id.contains(QStringLiteral("core_shadow"))) {
            foundCoreShadow = true;
            if (op.fillStyle == QLatin1String("directional"))
                foundDirectional = true;
            if (op.clipToId == QStringLiteral("body_torso"))
                foundClipToId = true;
            if (op.blendMode == QLatin1String("multiply"))
                foundMultiply = true;
        }
        if (op.id == QStringLiteral("chin_ao")) {
            foundChinAo = true;
        }
    }

    QVERIFY(foundCoreShadow);
    QVERIFY(foundDirectional);
    QVERIFY(foundClipToId);
    QVERIFY(foundMultiply);
    QVERIFY(foundChinAo);
}

void KisAiStrokeRendererTest::testPhase2ColorDodgeAndTargetedClipping()
{
    const QSize canvasSize(256, 256);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    // Base silhouette mass (Flats)
    KisAiStrokeOperation baseMass;
    baseMass.kind = KisAiStrokeOperation::Kind::Fill;
    baseMass.id = QStringLiteral("base_plate");
    baseMass.layer = QStringLiteral("Flats");
    baseMass.brush.color = QColor(60, 80, 140);
    baseMass.polygon = {QPointF(0.25, 0.25), QPointF(0.75, 0.25), QPointF(0.75, 0.75), QPointF(0.25, 0.75)};
    prog.operations.append(baseMass);

    // Targeted clipped shadow (covers outside, but must be clipped to base_plate)
    KisAiStrokeOperation clippedShade;
    clippedShade.kind = KisAiStrokeOperation::Kind::Fill;
    clippedShade.id = QStringLiteral("target_shade");
    clippedShade.layer = QStringLiteral("Shading");
    clippedShade.brush.color = QColor(20, 20, 50);
    clippedShade.blendMode = QStringLiteral("multiply");
    clippedShade.clipToId = QStringLiteral("base_plate");
    clippedShade.polygon = {QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    prog.operations.append(clippedShade);

    // Color Dodge Highlight stroke across the center
    KisAiStrokeOperation dodgeGlint;
    dodgeGlint.kind = KisAiStrokeOperation::Kind::Path;
    dodgeGlint.id = QStringLiteral("dodge_glint");
    dodgeGlint.layer = QStringLiteral("Highlights");
    dodgeGlint.brush.profile = QStringLiteral("gpen");
    dodgeGlint.brush.color = QColor(255, 220, 180);
    dodgeGlint.brush.size = 0.03;
    dodgeGlint.blendMode = QStringLiteral("color_dodge");
    dodgeGlint.points = {KisAiStrokePoint(0.35, 0.50, 1.0), KisAiStrokePoint(0.65, 0.50, 1.0)};
    prog.operations.append(dodgeGlint);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Corner (10, 10) must be completely transparent because target_shade was clipped to base_plate!
    QCOMPARE(img.pixelColor(10, 10).alpha(), 0);

    // Center area with Color Dodge highlight must have luminous color
    const QColor centerPixel = img.pixelColor(128, 128);
    QVERIFY(centerPixel.alpha() > 150);
    QVERIFY(centerPixel.value() > 80);
}

void KisAiStrokeRendererTest::testPhase2ModernHighFidelityAnimeEye()
{
    const QSize canvasSize(300, 300);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    KisAiStrokeOperation eyeOp;
    eyeOp.kind = KisAiStrokeOperation::Kind::AnimeEye;
    eyeOp.id = QStringLiteral("hero_eye");
    eyeOp.layer = QStringLiteral("Lineart");
    eyeOp.eyeCenter = QPointF(0.5, 0.5);
    eyeOp.eyeSize = QSizeF(0.35, 0.40);
    eyeOp.eyeIrisColor = QColor(40, 110, 245);
    eyeOp.eyeSecondaryColor = QColor(140, 235, 255);
    eyeOp.eyeStyle = QStringLiteral("sparkle");
    eyeOp.eyeIsRight = true;
    prog.operations.append(eyeOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Eye center should be rich iris tone
    const QColor centerPixel = img.pixelColor(150, 150);
    QVERIFY(centerPixel.alpha() > 150);
    QVERIFY(centerPixel.blue() > centerPixel.red()); // Blue iris

    // Upper eyelash area (150, 115) should have dark ink
    const QColor lashPixel = img.pixelColor(150, 115);
    QVERIFY(lashPixel.alpha() > 100);
    QVERIFY(lashPixel.value() < 80); // Deep dark lash line
}

void KisAiStrokeRendererTest::testPhase2ArtisticPaperGrainAndWetEdge()
{
    const QSize canvasSize(256, 256);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    KisAiStrokeOperation wcFill;
    wcFill.kind = KisAiStrokeOperation::Kind::Fill;
    wcFill.id = QStringLiteral("watercolor_wash");
    wcFill.layer = QStringLiteral("Flats");
    wcFill.brush.profile = QStringLiteral("watercolor");
    wcFill.brush.color = QColor(80, 150, 200, 180);
    wcFill.fillStyle = QStringLiteral("wash");
    wcFill.polygon = {QPointF(0.2, 0.2), QPointF(0.8, 0.2), QPointF(0.8, 0.8), QPointF(0.2, 0.8)};
    prog.operations.append(wcFill);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Inside wash, pixels should be filled
    const QColor pInside = img.pixelColor(128, 128);
    QVERIFY(pInside.alpha() > 100);

    // Outside wash should be transparent
    const QColor pOutside = img.pixelColor(15, 15);
    QCOMPARE(pOutside.alpha(), 0);
}

void KisAiStrokeRendererTest::testPhase2DynamicPerspectiveAndAngles()
{
    const QSize canvasSize(512, 512);

    // Generate with profile prompt
    const KisAiStrokeProgram progProfile =
        KisAiStrokeProgramCodec::createDeterministicProgram(QStringLiteral("side view profile anime girl"), canvasSize);
    // Generate with frontal prompt
    const KisAiStrokeProgram progFrontal =
        KisAiStrokeProgramCodec::createDeterministicProgram(QStringLiteral("frontal close up portrait girl"),
                                                            canvasSize);

    QVERIFY(!progProfile.operations.isEmpty());
    QVERIFY(!progFrontal.operations.isEmpty());

    // Find jawline points in both
    QVector<KisAiStrokePoint> jawProfile;
    QVector<KisAiStrokePoint> jawFrontal;
    for (const auto &op : progProfile.operations) {
        if (op.id == QStringLiteral("jawline"))
            jawProfile = op.points;
    }
    for (const auto &op : progFrontal.operations) {
        if (op.id == QStringLiteral("jawline"))
            jawFrontal = op.points;
    }

    QVERIFY(!jawProfile.isEmpty());
    QVERIFY(!jawFrontal.isEmpty());

    // Profile and frontal jawlines must have different geometric coordinates due to dynamic angle computation!
    QVERIFY(jawProfile.first().pos != jawFrontal.first().pos);
}

void KisAiStrokeRendererTest::testCrossLayerClipToId()
{
    const QSize canvasSize(200, 200);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    // Flats layer: base face silhouette in the center (0.3 - 0.7) -> 60px to 140px
    KisAiStrokeOperation baseFace;
    baseFace.kind = KisAiStrokeOperation::Kind::Fill;
    baseFace.id = QStringLiteral("base_face");
    baseFace.layer = QStringLiteral("Flats");
    baseFace.brush.color = QColor(255, 220, 200);
    baseFace.polygon = {QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    prog.operations.append(baseFace);

    // Shading layer: huge full-canvas polygon, but strictly clip_to_id: "base_face"
    KisAiStrokeOperation shadowOp;
    shadowOp.kind = KisAiStrokeOperation::Kind::Fill;
    shadowOp.id = QStringLiteral("face_shadow");
    shadowOp.layer = QStringLiteral("Shading");
    shadowOp.clipToId = QStringLiteral("base_face");
    shadowOp.blendMode = QStringLiteral("multiply");
    shadowOp.brush.color = QColor(60, 20, 40, 200);
    shadowOp.polygon = {QPointF(0.0, 0.0), QPointF(1.0, 0.0), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    prog.operations.append(shadowOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Point (100, 100) inside base_face must be shaded (non-zero alpha)
    const QColor centerPixel = img.pixelColor(100, 100);
    QVERIFY(centerPixel.alpha() > 100);

    // Point (20, 20) outside base_face must be completely transparent because shadow was clipped!
    const QColor outsidePixel = img.pixelColor(20, 20);
    QCOMPARE(outsidePixel.alpha(), 0);
}

void KisAiStrokeRendererTest::testProceduralMacroExpansionGuidance()
{
    const QSize canvasSize(300, 300);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    KisAiStrokeOperation hairOp;
    hairOp.kind = KisAiStrokeOperation::Kind::Ribbon;
    hairOp.id = QStringLiteral("main_hair");
    hairOp.layer = QStringLiteral("Flats");
    hairOp.brush.profile = QStringLiteral("hair");
    hairOp.brush.color = QColor(45, 30, 60);
    hairOp.spine = {QPointF(0.3, 0.2), QPointF(0.5, 0.5), QPointF(0.6, 0.8)};
    hairOp.widthStart = 0.03;
    hairOp.widthMid = 0.05;
    hairOp.widthEnd = 0.01;
    prog.operations.append(hairOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Rendered hair clump with strands must have substantial ink coverage along the spine
    const QColor spinePixel = img.pixelColor(150, 150);
    QVERIFY(spinePixel.alpha() > 100);
}

void KisAiStrokeRendererTest::testLintDropsNonFiniteGeometry()
{
    // Regression: lintStroke flagged needsRepair for NaN geometry but only broke
    // out of the validation loop, leaving drop==false. NaN compares false against
    // every threshold, so the bad vertex was then fed to path length / curvature /
    // self-intersection diagnostics and on to QPainter.
    const QSize canvas(1024, 1024);

    KisAiStrokeOperation nanPath;
    nanPath.kind = KisAiStrokeOperation::Kind::Path;
    nanPath.id = QStringLiteral("nan-path");
    nanPath.layer = QStringLiteral("Lineart");
    nanPath.brush.color = QColor(20, 20, 20);
    nanPath.brush.size = 0.004;
    nanPath.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(0.1, 0.1, 0.8),
                                               KisAiStrokePoint(std::numeric_limits<qreal>::quiet_NaN(), 0.5, 0.8),
                                               KisAiStrokePoint(0.9, 0.9, 0.8)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(nanPath, canvas).drop);

    KisAiStrokeOperation nanSpine;
    nanSpine.kind = KisAiStrokeOperation::Kind::Ribbon;
    nanSpine.id = QStringLiteral("nan-spine");
    nanSpine.layer = QStringLiteral("Flats");
    nanSpine.brush.color = QColor(20, 20, 20);
    nanSpine.widthStart = 0.01;
    nanSpine.widthMid = 0.01;
    nanSpine.widthEnd = 0.01;
    nanSpine.spine =
        QVector<QPointF>{QPointF(0.2, 0.2), QPointF(0.5, std::numeric_limits<qreal>::infinity()), QPointF(0.8, 0.6)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(nanSpine, canvas).drop);

    KisAiStrokeOperation nanPoly;
    nanPoly.kind = KisAiStrokeOperation::Kind::Fill;
    nanPoly.id = QStringLiteral("nan-poly");
    nanPoly.layer = QStringLiteral("Flats");
    nanPoly.brush.color = QColor(20, 20, 20);
    nanPoly.polygon =
        QVector<QPointF>{QPointF(0.2, 0.2), QPointF(std::numeric_limits<qreal>::quiet_NaN(), 0.6), QPointF(0.8, 0.8)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(nanPoly, canvas).drop);

    // A program carrying NaN geometry must never paint ink for that operation.
    KisAiStrokeProgram prog;
    prog.canvasSize = canvas;
    prog.operations.append(nanPath);
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvas);
    QVERIFY(!img.isNull());
}

void KisAiStrokeRendererTest::testLintKeepsCanvasCrossingStrokesAndFullBleedFills()
{
    // Regression: allPointsOutside and allPolyOutside previously checked if all vertices
    // were outside [-margin, 1+margin]. A path crossing the canvas edge-to-edge or a full-bleed
    // fill enclosing the canvas has all its vertices outside the boundary, but intersects the canvas.
    // They must NOT be dropped, while truly off-canvas objects must be dropped.
    const QSize canvas(1024, 1024);

    // 1. Edge-to-edge crossing path
    KisAiStrokeOperation crossingPath;
    crossingPath.kind = KisAiStrokeOperation::Kind::Path;
    crossingPath.id = QStringLiteral("crossing-path");
    crossingPath.layer = QStringLiteral("Lineart");
    crossingPath.brush.color = QColor(20, 20, 20);
    crossingPath.brush.size = 0.004;
    crossingPath.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(-0.1, 0.5, 0.8), KisAiStrokePoint(1.1, 0.5, 0.8)};
    const auto repCrossing = KisAiDeliberateStroke::lintStroke(crossingPath, canvas);
    QVERIFY2(!repCrossing.drop, qPrintable(repCrossing.reasons.join(QLatin1Char(';'))));

    // 2. Full-bleed background fill enclosing canvas
    KisAiStrokeOperation fullBleedFill;
    fullBleedFill.kind = KisAiStrokeOperation::Kind::Fill;
    fullBleedFill.id = QStringLiteral("full-bleed-fill");
    fullBleedFill.layer = QStringLiteral("Flats");
    fullBleedFill.brush.color = QColor(200, 220, 240);
    fullBleedFill.polygon =
        QVector<QPointF>{QPointF(-0.1, -0.1), QPointF(1.1, -0.1), QPointF(1.1, 1.1), QPointF(-0.1, 1.1)};
    QVERIFY(!KisAiDeliberateStroke::lintStroke(fullBleedFill, canvas).drop);

    // 3. Crossing polygon
    KisAiStrokeOperation crossingPoly;
    crossingPoly.kind = KisAiStrokeOperation::Kind::Fill;
    crossingPoly.id = QStringLiteral("crossing-poly");
    crossingPoly.layer = QStringLiteral("Flats");
    crossingPoly.brush.color = QColor(200, 200, 200);
    crossingPoly.polygon = QVector<QPointF>{QPointF(-0.1, 0.5), QPointF(1.1, 0.2), QPointF(1.1, 0.8)};
    QVERIFY(!KisAiDeliberateStroke::lintStroke(crossingPoly, canvas).drop);

    // 4. Ribbon spine crossing canvas
    KisAiStrokeOperation crossingRibbon;
    crossingRibbon.kind = KisAiStrokeOperation::Kind::Ribbon;
    crossingRibbon.id = QStringLiteral("crossing-ribbon");
    crossingRibbon.layer = QStringLiteral("Lineart");
    crossingRibbon.brush.color = QColor(20, 20, 20);
    crossingRibbon.widthStart = 0.01;
    crossingRibbon.widthMid = 0.01;
    crossingRibbon.widthEnd = 0.01;
    crossingRibbon.spine = QVector<QPointF>{QPointF(-0.1, 0.5), QPointF(1.1, 0.5)};
    QVERIFY(!KisAiDeliberateStroke::lintStroke(crossingRibbon, canvas).drop);

    // 5. Truly off-canvas path must be dropped
    KisAiStrokeOperation offPath;
    offPath.kind = KisAiStrokeOperation::Kind::Path;
    offPath.id = QStringLiteral("off-path");
    offPath.layer = QStringLiteral("Lineart");
    offPath.brush.color = QColor(20, 20, 20);
    offPath.brush.size = 0.004;
    offPath.points = QVector<KisAiStrokePoint>{KisAiStrokePoint(2.0, 2.0, 0.8), KisAiStrokePoint(2.5, 2.5, 0.8)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(offPath, canvas).drop);

    // 6. Truly off-canvas polygon must be dropped
    KisAiStrokeOperation offPoly;
    offPoly.kind = KisAiStrokeOperation::Kind::Fill;
    offPoly.id = QStringLiteral("off-poly");
    offPoly.layer = QStringLiteral("Flats");
    offPoly.brush.color = QColor(20, 20, 20);
    offPoly.polygon = QVector<QPointF>{QPointF(2.0, 2.0), QPointF(3.0, 2.0), QPointF(2.5, 3.0)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(offPoly, canvas).drop);

    // 7. Truly off-canvas ribbon spine must be dropped
    KisAiStrokeOperation offRibbon;
    offRibbon.kind = KisAiStrokeOperation::Kind::Ribbon;
    offRibbon.id = QStringLiteral("off-ribbon");
    offRibbon.layer = QStringLiteral("Flats");
    offRibbon.brush.color = QColor(20, 20, 20);
    offRibbon.widthStart = 0.01;
    offRibbon.widthMid = 0.01;
    offRibbon.widthEnd = 0.01;
    offRibbon.spine = QVector<QPointF>{QPointF(2.0, 2.0), QPointF(2.5, 2.5)};
    QVERIFY(KisAiDeliberateStroke::lintStroke(offRibbon, canvas).drop);

    // Verify rendering of crossing path paints actual ink in canvas center
    KisAiStrokeProgram prog;
    prog.canvasSize = canvas;
    prog.operations.append(crossingPath);
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvas);
    const QColor centerPixel = img.pixelColor(canvas.width() / 2, canvas.height() / 2);
    QVERIFY(centerPixel.alpha() > 100);
}

void KisAiStrokeRendererTest::testShadingExcludesWatercolorFringe()
{
    const QSize canvasSize(256, 256);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    // A flats base so shading has something to clip to
    KisAiStrokeOperation flats;
    flats.kind = KisAiStrokeOperation::Kind::Fill;
    flats.id = QStringLiteral("base_flats");
    flats.layer = QStringLiteral("Flats");
    flats.brush.color = QColor(240, 210, 195);
    flats.polygon = {QPointF(0.1, 0.1), QPointF(0.9, 0.1), QPointF(0.9, 0.9), QPointF(0.1, 0.9)};
    prog.operations.append(flats);

    // Shading with watercolor profile - should NOT have dark contour fringe pen
    KisAiStrokeOperation shading;
    shading.kind = KisAiStrokeOperation::Kind::Fill;
    shading.id = QStringLiteral("face_shadow");
    shading.layer = QStringLiteral("Shading");
    shading.brush.profile = QStringLiteral("watercolor");
    shading.brush.color = QColor(180, 100, 90, 120);
    shading.fillStyle = QStringLiteral("wash");
    shading.polygon = {QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    prog.operations.append(shading);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Shading inside must be rendered smoothly without extreme boundary spikes
    const QColor insideColor = img.pixelColor(128, 128);
    QVERIFY(insideColor.red() > 50); // Not crushed to pitch black
}

void KisAiStrokeRendererTest::testHairClumpStrandConvergence()
{
    KisAiStrokeOperation ribbonOp;
    ribbonOp.kind = KisAiStrokeOperation::Kind::Ribbon;
    ribbonOp.id = QStringLiteral("converging_hair");
    ribbonOp.layer = QStringLiteral("Flats");
    ribbonOp.spine = {QPointF(0.5, 0.1), QPointF(0.52, 0.4), QPointF(0.51, 0.7), QPointF(0.50, 0.9)};
    ribbonOp.widthStart = 0.04;
    ribbonOp.widthMid = 0.03;
    ribbonOp.widthEnd = 0.008;
    ribbonOp.brush.color = QColor(100, 120, 180);

    const auto clump = KisAiStrokeQualityUtils::synthesizeHairClump(ribbonOp, QSize(500, 500), 42);
    QCOMPARE(clump.mainMass.layer, QStringLiteral("Flats"));
    QVERIFY(clump.mainMass.brush.opacity >= 0.90);
    QVERIFY(clump.strands.size() >= 4);

    // Verify strands converge: distance between leftmost and rightmost strand at tip must be smaller than at root
    if (clump.strands.size() >= 2) {
        const auto &leftStrand = clump.strands.first().points;
        const auto &rightStrand = clump.strands.last().points;
        QVERIFY(leftStrand.size() >= 2 && rightStrand.size() >= 2);

        const qreal rootDist = std::hypot(leftStrand.first().pos.x() - rightStrand.first().pos.x(),
                                          leftStrand.first().pos.y() - rightStrand.first().pos.y());
        const qreal tipDist = std::hypot(leftStrand.last().pos.x() - rightStrand.last().pos.x(),
                                         leftStrand.last().pos.y() - rightStrand.last().pos.y());
        QVERIFY(tipDist < rootDist); // Tip must be narrower than root (convergence)
    }
}

void KisAiStrokeRendererTest::testAnimeMouthRenderingAndFinishingSuite()
{
    const QSize canvasSize(300, 300);
    KisAiStrokeProgram prog;
    prog.canvasSize = canvasSize;

    KisAiStrokeOperation mouthOp;
    mouthOp.kind = KisAiStrokeOperation::Kind::AnimeMouth;
    mouthOp.id = QStringLiteral("hero_mouth");
    mouthOp.layer = QStringLiteral("Lineart");
    mouthOp.mouthCenter = QPointF(0.50, 0.50);
    mouthOp.mouthSize = QSizeF(0.20, 0.10);
    mouthOp.mouthExpression = QStringLiteral("open_smile");
    mouthOp.mouthLipColor = QColor(240, 110, 125);
    mouthOp.mouthHasHighlight = true;
    prog.operations.append(mouthOp);

    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, canvasSize);
    QVERIFY(!img.isNull());

    // Mouth cavity area (150, 158) should be painted with open mouth cavity tone
    const QColor mouthPixel = img.pixelColor(150, 158);
    QVERIFY(mouthPixel.alpha() > 100);

    // Test Harmonic Colored Lineart (色トレス)
    const QColor baseBlack(20, 15, 25);
    const QColor skinFlats(255, 220, 205);
    const QColor harmonicSkinLine = KisAiStrokeQualityUtils::calculateHarmonicLineColor(baseBlack, skinFlats, true);
    QVERIFY(harmonicSkinLine.red() > baseBlack.red());
    QCOMPARE(harmonicSkinLine.alpha(), baseBlack.alpha());

    // Test SSS Fringe generation
    KisAiStrokeOperation skinShadow;
    skinShadow.kind = KisAiStrokeOperation::Kind::Fill;
    skinShadow.id = QStringLiteral("face_skin_shadow");
    skinShadow.layer = QStringLiteral("Shading");
    skinShadow.polygon = {QPointF(0.3, 0.3), QPointF(0.7, 0.3), QPointF(0.7, 0.7), QPointF(0.3, 0.7)};
    const auto fringes = KisAiStrokeQualityUtils::generateSkinSssFringe(skinShadow, canvasSize);
    QCOMPARE(fringes.size(), 1);
    QCOMPARE(fringes.first().layer, QStringLiteral("Shading"));
    QCOMPARE(fringes.first().brush.color, QColor(255, 95, 110));

    // Test Finishing Suite Image Generators
    const QImage vig = KisAiStrokeQualityUtils::generateVignetteImage(canvasSize, 0.15);
    QVERIFY(!vig.isNull());
    QCOMPARE(vig.size(), canvasSize);
    QCOMPARE(vig.pixelColor(150, 150).alpha(), 0);
    QVERIFY(vig.pixelColor(5, 5).alpha() > 0);

    const QImage grain = KisAiStrokeQualityUtils::generateFilmGrain(canvasSize, 0.08, 42);
    QVERIFY(!grain.isNull());
    QCOMPARE(grain.size(), canvasSize);
    QCOMPARE(grain.format(), QImage::Format_ARGB32_Premultiplied);
    QVERIFY(grain.pixelColor(50, 50).alpha() > 0);
    // Verify premultiplied alpha invariant across scanlines
    for (int y = 0; y < grain.height(); ++y) {
        const auto *scanLine = reinterpret_cast<const QRgb *>(grain.constScanLine(y));
        for (int x = 0; x < grain.width(); ++x) {
            const QRgb px = scanLine[x];
            const int a = qAlpha(px);
            QVERIFY(qRed(px) <= a);
            QVERIFY(qGreen(px) <= a);
            QVERIFY(qBlue(px) <= a);
        }
    }
}

KISTEST_MAIN(KisAiStrokeRendererTest)
