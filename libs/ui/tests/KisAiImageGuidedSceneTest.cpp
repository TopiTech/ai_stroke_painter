/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiImageGuidedSceneTest.h"

#include "KisAiTestCrashGuard.h"
#include <QPainter>
#include <QtTest>

#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) AI_STROKE_TEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiImageGuidedScene.h"
#include "aiillustration/KisAiStrokeRenderer.h"

namespace
{
KisAiImageGuidedScene sceneWithRectangle(const QSize &sourceSize = QSize(100, 50),
                                         const QSize &canvasSize = QSize(200, 200))
{
    KisAiImageGuidedScene scene;
    scene.source = QImage(sourceSize, QImage::Format_ARGB32_Premultiplied);
    scene.source.fill(QColor(170, 140, 120));
    scene.canvasSize = canvasSize;
    KisAiGuidedRegion region;
    region.id = QStringLiteral("item");
    region.confidence = 0.99;
    region.outlineApproved = true;
    region.mask = QImage(sourceSize, QImage::Format_Grayscale8);
    region.mask.fill(0);
    QPainter painter(&region.mask);
    painter.setPen(Qt::NoPen);
    painter.fillRect(QRect(20, 10, 60, 30), Qt::white);
    painter.end();
    scene.regions.append(region);
    return scene;
}
} // namespace

void KisAiImageGuidedSceneTest::testImagePlacementMatchesCenteredKeepAspectRatio()
{
    const auto scene = sceneWithRectangle();
    QCOMPARE(scene.imageRectPx(), QRectF(0, 50, 200, 100));
    QVERIFY(scene.isValid());
}

void KisAiImageGuidedSceneTest::testOddLetterboxPlacementMatchesImageLayer()
{
    const auto scene = sceneWithRectangle(QSize(100, 50), QSize(201, 200));
    QCOMPARE(scene.imageRectPx(), QRectF(0, 50, 201, 100));
    const auto vertical = sceneWithRectangle(QSize(50, 100), QSize(200, 201));
    QCOMPARE(vertical.imageRectPx(), QRectF(50, 0, 100, 201));
}

void KisAiImageGuidedSceneTest::testNoMaskNeverInventsGeometry()
{
    auto scene = sceneWithRectangle();
    scene.regions.clear();
    const QImage original = scene.source.copy();
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
    QCOMPARE(scene.source, original);
    scene.regions = sceneWithRectangle().regions;
    scene.regions[0].outlineApproved = false;
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
}

void KisAiImageGuidedSceneTest::testLowConfidenceAndInvalidMaskNeverInventsGeometry()
{
    auto scene = sceneWithRectangle();
    scene.regions[0].confidence = 0.94;
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
    scene.regions[0].confidence = 0.99;
    scene.regions[0].mask = QImage(QSize(50, 50), QImage::Format_Grayscale8);
    scene.regions[0].mask.fill(255);
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
    scene.regions[0].mask = QImage(QSize(100, 50), QImage::Format_ARGB32);
    scene.regions[0].mask.fill(Qt::white);
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
    scene = sceneWithRectangle();
    scene.regions[0].id = QStringLiteral("../unsafe");
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
}

void KisAiImageGuidedSceneTest::testRectangleBoundaryInNonSquareCanvas()
{
    const auto scene = sceneWithRectangle();
    const auto program = KisAiImageGuidedContour::buildLineart(scene);
    QCOMPARE(program.operations.size(), 1);
    const auto &op = program.operations.first();
    QCOMPARE(op.layer, QStringLiteral("Lineart"));
    QCOMPARE(op.kind, KisAiStrokeOperation::Kind::Path);
    QVERIFY(op.closed);
    qreal minX = 1.0, minY = 1.0, maxX = 0.0, maxY = 0.0;
    for (const auto &pt : op.points) {
        minX = qMin(minX, pt.pos.x());
        minY = qMin(minY, pt.pos.y());
        maxX = qMax(maxX, pt.pos.x());
        maxY = qMax(maxY, pt.pos.y());
    }
    QVERIFY(qAbs(minX - 0.2) < 0.01);
    QVERIFY(qAbs(maxX - 0.8) < 0.01);
    QVERIFY(qAbs(minY - 0.35) < 0.01);
    QVERIFY(qAbs(maxY - 0.65) < 0.01);
    const QImage lineart = KisAiStrokeRenderer::renderProgramToImage(program, scene.canvasSize);
    QVERIFY(!lineart.isNull());
    QVERIFY(lineart.pixelColor(40, 70).alpha() > 0);
}

void KisAiImageGuidedSceneTest::testHolesAreNotContours()
{
    auto scene = sceneWithRectangle();
    QPainter painter(&scene.regions[0].mask);
    painter.fillRect(QRect(40, 20, 20, 10), Qt::black);
    painter.end();
    const auto program = KisAiImageGuidedContour::buildLineart(scene);
    QCOMPARE(program.operations.size(), 1);
}

void KisAiImageGuidedSceneTest::testAlphaMaskAndBoundaryAtImageEdge()
{
    auto scene = sceneWithRectangle();
    scene.regions[0].mask = QImage(scene.source.size(), QImage::Format_Alpha8);
    scene.regions[0].mask.fill(0);
    for (int y = 15; y < 45; ++y) {
        uchar *row = scene.regions[0].mask.scanLine(y);
        for (int x = 0; x < 40; ++x)
            row[x] = 255;
    }
    const auto program = KisAiImageGuidedContour::buildLineart(scene);
    QCOMPARE(program.operations.size(), 1);
    qreal minX = 1.0;
    for (const auto &point : program.operations.first().points)
        minX = qMin(minX, point.pos.x());
    QVERIFY(minX < 0.01);
}

void KisAiImageGuidedSceneTest::testTextureEdgesDoNotBecomeStrokes()
{
    auto scene = sceneWithRectangle();
    QPainter painter(&scene.source);
    painter.fillRect(QRect(40, 0, 15, 50), QColor(20, 220, 20));
    painter.end();
    const auto program = KisAiImageGuidedContour::buildLineart(scene);
    QCOMPARE(program.operations.size(), 1);
}

void KisAiImageGuidedSceneTest::testDisconnectedMaskNeedsSplit()
{
    auto scene = sceneWithRectangle();
    QPainter painter(&scene.regions[0].mask);
    painter.fillRect(QRect(0, 0, 12, 12), Qt::white);
    painter.end();
    QVERIFY(KisAiImageGuidedContour::buildLineart(scene).operations.isEmpty());
}

void KisAiImageGuidedSceneTest::testRepeatIsDeterministic()
{
    const auto scene = sceneWithRectangle();
    const auto a = KisAiImageGuidedContour::buildLineart(scene);
    const auto b = KisAiImageGuidedContour::buildLineart(scene);
    QCOMPARE(a.operations.size(), b.operations.size());
    QCOMPARE(a.operations.first().points.size(), b.operations.first().points.size());
    for (int i = 0; i < a.operations.first().points.size(); ++i)
        QCOMPARE(a.operations.first().points.at(i).pos, b.operations.first().points.at(i).pos);
}

KISTEST_MAIN(KisAiImageGuidedSceneTest)
