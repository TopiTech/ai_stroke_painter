/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRendererTest.h"

#include <QColor>
#include <QDir>
#include <QImage>
#include <QPointF>
#include <QSet>
#include <QVector>
#include <cmath>
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

KISTEST_MAIN(KisAiStrokeRendererTest)

