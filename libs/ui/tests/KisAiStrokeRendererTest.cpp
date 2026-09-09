/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeRendererTest.h"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QVector>
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

KISTEST_MAIN(KisAiStrokeRendererTest)

