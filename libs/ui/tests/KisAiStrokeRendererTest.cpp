/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRendererTest.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
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

#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeRenderer.h"
#include "aiillustration/KisAiStrokeQualityUtils.h"

void KisAiStrokeRendererTest::testCatmullRomSpline()
{
    const QVector<QPointF> input = {
        QPointF(0.0, 0.0),
        QPointF(10.0, 30.0),
        QPointF(25.0, 15.0),
        QPointF(40.0, 0.0)
    };

    const QVector<QPointF> spline = KisAiStrokeRenderer::generateCatmullRomSpline(input, 6, false);

    // Subdividing 3 segments with 6 steps = 18 + 1 = 19 points
    QVERIFY(spline.size() > input.size());
    QCOMPARE(spline.first(), input.first());
    QCOMPARE(spline.last(), input.last());
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
    pathOp.points = QVector<KisAiStrokePoint>{
        KisAiStrokePoint(0.2, 0.5, 0.8),
        KisAiStrokePoint(0.8, 0.5, 0.8)
    };
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
    gradOp.points = QVector<KisAiStrokePoint>{
        KisAiStrokePoint(0.0, 0.0, 1.0),
        KisAiStrokePoint(1.0, 1.0, 1.0)
    };
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
    airOp.points = QVector<KisAiStrokePoint>{
        KisAiStrokePoint(0.1, 0.5, 0.5),
        KisAiStrokePoint(0.5, 0.5, 1.0),
        KisAiStrokePoint(0.9, 0.5, 0.5)
    };
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
    hatchOp.polygon = QPolygonF{
        QPointF(0.25, 0.25),
        QPointF(0.75, 0.25),
        QPointF(0.75, 0.75),
        QPointF(0.25, 0.75)
    };
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
    radOp.gradientColors = QVector<QColor>{
        QColor(255, 100, 50),
        QColor(20, 20, 80)
    };
    radOp.polygon = QPolygonF{
        QPointF(0.1, 0.1),
        QPointF(0.9, 0.1),
        QPointF(0.9, 0.9),
        QPointF(0.1, 0.9)
    };
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
    const QVector<QPointF> input = {
        QPointF(0.0, 0.0),
        QPointF(0.95, 0.02),
        QPointF(1.0, 4.0),
        QPointF(2.0, 4.1)
    };
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
    stroke.points = {
        KisAiStrokePoint(0.1, 0.5, 1.0),
        KisAiStrokePoint(0.35, 0.42, 0.9),
        KisAiStrokePoint(0.65, 0.58, 0.75),
        KisAiStrokePoint(0.9, 0.5, 0.5)
    };
    program.operations.append(stroke);

    const QImage image = KisAiStrokeRenderer::renderProgramToImage(program, QSize(256, 256));
    QVERIFY(!image.isNull());

    int partialAlphaPixels = 0;
    int paintedPixels = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = qAlpha(image.pixel(x, y));
            if (alpha > 0) ++paintedPixels;
            if (alpha > 0 && alpha < 255) ++partialAlphaPixels;
        }
    }
    QVERIFY(paintedPixels > 250);
    QVERIFY(partialAlphaPixels > 30);

    auto verticalCoverage = [&image](int x) {
        int count = 0;
        for (int y = 0; y < image.height(); ++y) {
            if (qAlpha(image.pixel(x, y)) > 16) ++count;
        }
        return count;
    };
    QVERIFY(verticalCoverage(128) > verticalCoverage(26));
}

void KisAiStrokeRendererTest::testRepresentativeCompositionQualityMetrics()
{
    const KisAiStrokeProgram program = KisAiStrokeProgramCodec::createDeterministicProgram(
        QStringLiteral("cinematic sunset mountain landscape with sakura petals"),
        QSize(1024, 768)
    );
    QVERIFY(program.isValid());
    QVERIFY(program.completionScore >= 0.6);

    const QMap<QString, int> layers = KisAiStrokeProgramCodec::countLayerOperations(program);
    for (const QString &layer : {QStringLiteral("Flats"), QStringLiteral("Shading"), QStringLiteral("Lineart"), QStringLiteral("Highlights"), QStringLiteral("FX")}) {
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
            if (color.alpha() > 220) ++opaqueSamples;
            if (color.alpha() > 32 && (qMax(color.red(), qMax(color.green(), color.blue()))
                    - qMin(color.red(), qMin(color.green(), color.blue()))) > 12) {
                ++chromaticSamples;
            }
            if (color.alpha() > 0) sampledColors.insert(image.pixel(x, y));
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
            if (rendered.pixelColor(x, y).alpha() > 50) ++topLeftCount;
        }
    }
    for (int y = 100; y < 200; ++y) {
        for (int x = 100; x < 200; ++x) {
            if (rendered.pixelColor(x, y).alpha() > 50) ++bottomRightCount;
        }
    }
    QVERIFY(topLeftCount > bottomRightCount);
}

void KisAiStrokeRendererTest::testNewBrushProfilesRendering()
{
    for (const QString &profile : {QStringLiteral("marker"), QStringLiteral("crayon"), QStringLiteral("neon"), QStringLiteral("splatter")}) {
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
                if (img.pixelColor(x, y).alpha() > 10) ++coloredCount;
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
            if (imgStep1.pixelColor(x, y).alpha() > 16) ++step1Painted;
            if (imgFinal.pixelColor(x, y).alpha() > 16) ++finalPainted;
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
            QStringLiteral("anime girl portrait with vibrant eyes"), canvasSize, step, 4);
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
        if (hasDrawnPixel) break;
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
    QVERIFY(img.pixelColor(100, 40).alpha() > 100);  // Calligraphy line
    QVERIFY(img.pixelColor(100, 100).alpha() > 50);  // Charcoal line
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
    const QVector<KisAiStrokeOperation> expanded = KisAiStrokeRenderer::expandProceduralOperations(ops, QSize(400, 400));
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

    const QVector<KisAiStrokeOperation> clusters = KisAiStrokeQualityUtils::synthesizeFoliageClusters(fillOp, QSize(400, 400), 54321);
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
    const QByteArray payload = QByteArray::fromBase64(dataUrl.mid(QStringLiteral("data:image/jpeg;base64,").size()).toLatin1());
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
                if (minY < 0) minY = y;
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
            if (qAlpha(imgNoTrap.pixel(x, y)) > 0) ++nonZeroNoTrap;
            if (qAlpha(imgTrap.pixel(x, y)) > 0) ++nonZeroTrap;
        }
    }
    // Trapping must dilate Flats slightly to prevent white gaps
    QVERIFY(nonZeroTrap >= nonZeroNoTrap);

    // Screen blend mode check: Center pixel (50, 50) must be brighter than base flat color (100, 50, 50)
    const QRgb centerPixel = imgNoTrap.pixel(50, 50);
    QVERIFY(qRed(centerPixel) > 100);
    QVERIFY(qGreen(centerPixel) > 50);
}

KISTEST_MAIN(KisAiStrokeRendererTest)
