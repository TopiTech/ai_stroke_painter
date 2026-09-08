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

KISTEST_MAIN(KisAiStrokeRendererTest)
