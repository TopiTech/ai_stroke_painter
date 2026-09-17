/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPhysicalRendererTest.h"

#include <QColor>
#include <QImage>
#include <QPolygonF>
#include <QSize>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif
#include <QtMath>

#include "aiillustration/KisAiPhysicalRenderer.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeRenderer.h"

using namespace KisAi;

void KisAiPhysicalRendererTest::testSrgbToLinearAndLinearToSrgbRoundtrip()
{
    // [0.0, 1.0] の主要な値で往復テスト
    const QVector<float> testVals = {0.0f, 0.01f, 0.04f, 0.18f, 0.5f, 0.73f, 1.0f};
    for (float v : testVals) {
        const float lin = KisAiPhysicalRenderer::srgbToLinear(v);
        const float roundtrip = KisAiPhysicalRenderer::linearToSrgb(lin);
        QVERIFY2(std::abs(roundtrip - v) < 1e-3f,
                 qPrintable(QString("Roundtrip mismatch: original=%1, got=%2").arg(v).arg(roundtrip)));
    }
}

void KisAiPhysicalRendererTest::testBlendMultiplyValues()
{
    // B(cb, cs) = cb * cs
    QCOMPARE(KisAiPhysicalRenderer::blendMultiply(0.0f, 1.0f), 0.0f);
    QCOMPARE(KisAiPhysicalRenderer::blendMultiply(1.0f, 1.0f), 1.0f);
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendMultiply(0.5f, 0.5f) - 0.25f) < 1e-5f);
}

void KisAiPhysicalRendererTest::testBlendScreenValues()
{
    // B(cb, cs) = cb + cs - cb * cs
    QCOMPARE(KisAiPhysicalRenderer::blendScreen(0.0f, 0.0f), 0.0f);
    QCOMPARE(KisAiPhysicalRenderer::blendScreen(1.0f, 1.0f), 1.0f);
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendScreen(0.5f, 0.5f) - 0.75f) < 1e-5f);
}

void KisAiPhysicalRendererTest::testBlendOverlayValues()
{
    // cb <= 0.5 ? 2*cb*cs : 1 - 2*(1-cb)*(1-cs)
    QCOMPARE(KisAiPhysicalRenderer::blendOverlay(0.0f, 0.5f), 0.0f);
    QCOMPARE(KisAiPhysicalRenderer::blendOverlay(1.0f, 0.5f), 1.0f);
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendOverlay(0.5f, 0.5f) - 0.5f) < 1e-5f);
    // cs=1 on cb=0.25 -> 2*0.25*1 = 0.5
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendOverlay(0.25f, 1.0f) - 0.5f) < 1e-5f);
}

void KisAiPhysicalRendererTest::testBlendSoftLightValues()
{
    // Neutral when cs=0.5 -> B(cb, 0.5) = cb
    const float cb = 0.4f;
    const float out = KisAiPhysicalRenderer::blendSoftLight(cb, 0.5f);
    QVERIFY(std::abs(out - cb) < 1e-4f);

    // Darkening when cs < 0.5
    QVERIFY(KisAiPhysicalRenderer::blendSoftLight(0.5f, 0.2f) < 0.5f);
    // Lightening when cs > 0.5
    QVERIFY(KisAiPhysicalRenderer::blendSoftLight(0.5f, 0.8f) > 0.5f);
}

void KisAiPhysicalRendererTest::testBlendColorDodgeValues()
{
    // cs=0 -> cb
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendColorDodge(0.3f, 0.0f) - 0.3f) < 1e-5f);
    // cb=0 -> 0
    QCOMPARE(KisAiPhysicalRenderer::blendColorDodge(0.0f, 0.8f), 0.0f);
    // cs=1 -> 1
    QCOMPARE(KisAiPhysicalRenderer::blendColorDodge(0.5f, 1.0f), 1.0f);
}

void KisAiPhysicalRendererTest::testBlendLinearBurnValues()
{
    // max(0, cb + cs - 1)
    QCOMPARE(KisAiPhysicalRenderer::blendLinearBurn(0.3f, 0.4f), 0.0f);
    QVERIFY(std::abs(KisAiPhysicalRenderer::blendLinearBurn(0.7f, 0.6f) - 0.3f) < 1e-5f);
}

void KisAiPhysicalRendererTest::testBlendPixelOpaqueOverOpaque()
{
    // 赤 (1,0,0,1) の上に青 (0,0,1,1) を normal 合成 -> 青 (0,0,1,1)
    float dR = 1.0f, dG = 0.0f, dB = 0.0f, dA = 1.0f;
    KisAiPhysicalRenderer::blendPixel(QStringLiteral("normal"), 0.0f, 0.0f, 1.0f, 1.0f, dR, dG, dB, dA);
    QCOMPARE(dA, 1.0f);
    QCOMPARE(dR, 0.0f);
    QCOMPARE(dG, 0.0f);
    QCOMPARE(dB, 1.0f);
}

void KisAiPhysicalRendererTest::testBlendPixelTransparentSrc()
{
    // 透明ソース (a=0) は dst を変更しない
    float dR = 0.5f, dG = 0.6f, dB = 0.7f, dA = 0.8f;
    KisAiPhysicalRenderer::blendPixel(QStringLiteral("normal"), 1.0f, 1.0f, 1.0f, 0.0f, dR, dG, dB, dA);
    QCOMPARE(dR, 0.5f);
    QCOMPARE(dG, 0.6f);
    QCOMPARE(dB, 0.7f);
    QCOMPARE(dA, 0.8f);
}

void KisAiPhysicalRendererTest::testBlendPixelTransparentDst()
{
    // 透明バックドロップ (a=0) に半透明ソース (0.4, 0.2, 0.1, 0.5) を合成 -> ソース値
    float dR = 0.0f, dG = 0.0f, dB = 0.0f, dA = 0.0f;
    KisAiPhysicalRenderer::blendPixel(QStringLiteral("normal"), 0.4f, 0.2f, 0.1f, 0.5f, dR, dG, dB, dA);
    QCOMPARE(dA, 0.5f);
    QCOMPARE(dR, 0.4f);
    QCOMPARE(dG, 0.2f);
    QCOMPARE(dB, 0.1f);
}

void KisAiPhysicalRendererTest::testBlendPixelPartialOpacity()
{
    // 不透明白 (1,1,1,1) を opacity 0.5 で透明 dst (0,0,0,0) に合成 -> アルファ 0.5
    float dR = 0.0f, dG = 0.0f, dB = 0.0f, dA = 0.0f;
    KisAiPhysicalRenderer::blendPixel(QStringLiteral("normal"), 1.0f, 1.0f, 1.0f, 1.0f, dR, dG, dB, dA, 0.5f);
    QCOMPARE(dA, 0.5f);
    QCOMPARE(dR, 0.5f);
}

void KisAiPhysicalRendererTest::testHdrFormatSupported()
{
    QVERIFY(KisAiPhysicalRenderer::isHdrFormatSupported());
}

void KisAiPhysicalRendererTest::testToLinearHdrAndToSrgbLdrRoundtrip()
{
    // 32x32 の市松模様画像を作成
    QImage src(32, 32, QImage::Format_ARGB32_Premultiplied);
    src.fill(Qt::transparent);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            if ((x + y) % 2 == 0) {
                src.setPixelColor(x, y, QColor(200, 100, 50, 255));
            } else {
                src.setPixelColor(x, y, QColor(30, 120, 220, 180));
            }
        }
    }

    const QImage hdr = KisAiPhysicalRenderer::toLinearHdr(src);
    QVERIFY(!hdr.isNull());
    QCOMPARE(hdr.size(), src.size());

    const QImage ldr = KisAiPhysicalRenderer::toSrgbLdr(hdr);
    QVERIFY(!ldr.isNull());
    QCOMPARE(ldr.size(), src.size());

    // 元のピクセル色との差分が微小 (1〜2 階調以内) であることを確認
    for (int y = 0; y < 32; y += 4) {
        for (int x = 0; x < 32; x += 4) {
            const QColor cOrig = src.pixelColor(x, y);
            const QColor cConv = ldr.pixelColor(x, y);
            QVERIFY(std::abs(cOrig.red() - cConv.red()) <= 3);
            QVERIFY(std::abs(cOrig.green() - cConv.green()) <= 3);
            QVERIFY(std::abs(cOrig.blue() - cConv.blue()) <= 3);
            QVERIFY(std::abs(cOrig.alpha() - cConv.alpha()) <= 3);
        }
    }
}

void KisAiPhysicalRendererTest::testCompositeLayerNormal()
{
    QImage dst(64, 64, QImage::Format_ARGB32_Premultiplied);
    dst.fill(QColor(100, 100, 100, 255));

    KisAiLayerImage lyr;
    lyr.name = QStringLiteral("TestLayer");
    lyr.blendMode = QStringLiteral("normal");
    QImage lyrImg(64, 64, QImage::Format_ARGB32_Premultiplied);
    lyrImg.fill(QColor(200, 200, 200, 255));
    lyr.image = lyrImg;

    KisAiPhysicalRenderer::compositeLayer(dst, lyr);
    const QImage ldr = (dst.format() == QImage::Format_RGBA32FPx4_Premultiplied)
        ? KisAiPhysicalRenderer::toSrgbLdr(dst) : dst;
    const QColor result = ldr.pixelColor(32, 32);
    QVERIFY(result.red() >= 195);
}

void KisAiPhysicalRendererTest::testCompositeLayerMultiply()
{
    // 白 (255, 255, 255) の上に 50% 灰色 (128, 128, 128) を乗算 -> 約 128
    QImage dst(32, 32, QImage::Format_ARGB32_Premultiplied);
    dst.fill(QColor(255, 255, 255, 255));

    KisAiLayerImage lyr;
    lyr.name = QStringLiteral("Shading");
    lyr.blendMode = QStringLiteral("multiply");
    QImage lyrImg(32, 32, QImage::Format_ARGB32_Premultiplied);
    lyrImg.fill(QColor(128, 128, 128, 255));
    lyr.image = lyrImg;

    KisAiPhysicalRenderer::compositeLayer(dst, lyr);
    const QImage ldr = (dst.format() == QImage::Format_RGBA32FPx4_Premultiplied)
        ? KisAiPhysicalRenderer::toSrgbLdr(dst) : dst;
    const QColor result = ldr.pixelColor(16, 16);
    QVERIFY(std::abs(result.red() - 128) <= 5);
}

void KisAiPhysicalRendererTest::testCompositeLayerWithClipMask()
{
    // dst は透明
    QImage dst(32, 32, QImage::Format_ARGB32_Premultiplied);
    dst.fill(Qt::transparent);

    // ソースレイヤーは全面赤
    KisAiLayerImage lyr;
    lyr.name = QStringLiteral("Shading");
    lyr.blendMode = QStringLiteral("normal");
    QImage lyrImg(32, 32, QImage::Format_ARGB32_Premultiplied);
    lyrImg.fill(QColor(255, 0, 0, 255));
    lyr.image = lyrImg;

    // クリップマスク: 左半分だけ不透明、右半分は透明
    QImage mask(32, 32, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 16; ++x) {
            mask.setPixelColor(x, y, QColor(0, 0, 0, 255));
        }
    }

    KisAiPhysicalRenderer::compositeLayer(dst, lyr, &mask);
    const QImage ldr = (dst.format() == QImage::Format_RGBA32FPx4_Premultiplied)
        ? KisAiPhysicalRenderer::toSrgbLdr(dst) : dst;

    // 左半分 (x=8) は赤、右半分 (x=24) は透明
    QVERIFY(ldr.pixelColor(8, 16).alpha() >= 250);
    QVERIFY(ldr.pixelColor(24, 16).alpha() <= 5);
}

void KisAiPhysicalRendererTest::testCompositeGraphEvaluate()
{
    KisAiCompositeGraph graph;
    graph.size = QSize(64, 64);

    KisAiLayerImage flats;
    flats.name = QStringLiteral("Flats");
    flats.blendMode = QStringLiteral("normal");
    QImage fImg(64, 64, QImage::Format_ARGB32_Premultiplied);
    fImg.fill(QColor(0, 200, 100, 255));
    flats.image = fImg;
    graph.layers.append(flats);

    KisAiLayerImage lineart;
    lineart.name = QStringLiteral("Lineart");
    lineart.blendMode = QStringLiteral("normal");
    QImage lImg(64, 64, QImage::Format_ARGB32_Premultiplied);
    lImg.fill(Qt::transparent);
    lImg.setPixelColor(32, 32, QColor(0, 0, 0, 255));
    lineart.image = lImg;
    graph.layers.append(lineart);

    const QImage evaluated = graph.evaluate();
    QVERIFY(!evaluated.isNull());
    QCOMPARE(evaluated.size(), QSize(64, 64));
    // (32, 32) は黒、それ以外は緑
    QCOMPARE(evaluated.pixelColor(32, 32), QColor(0, 0, 0, 255));
    QVERIFY(evaluated.pixelColor(0, 0).green() >= 195);
}

void KisAiPhysicalRendererTest::testCompositeGraphFindLayer()
{
    KisAiCompositeGraph graph;
    KisAiLayerImage l1;
    l1.name = QStringLiteral("Flats");
    graph.layers.append(l1);

    QCOMPARE(graph.findLayerIndex(QStringLiteral("flats")), 0);
    QCOMPARE(graph.findLayerIndex(QStringLiteral("NonExistent")), -1);
    QVERIFY(graph.findLayer(QStringLiteral("FLATS")) != nullptr);
    QVERIFY(graph.findLayer(QStringLiteral("unknown")) == nullptr);
}

void KisAiPhysicalRendererTest::testDownsampleBox()
{
    QImage highRes(128, 128, QImage::Format_ARGB32_Premultiplied);
    highRes.fill(QColor(100, 150, 200, 255));

    const QImage lowRes = KisAiPhysicalRenderer::downsampleBox(highRes, QSize(32, 32));
    QVERIFY(!lowRes.isNull());
    QCOMPARE(lowRes.size(), QSize(32, 32));
    QCOMPARE(lowRes.pixelColor(16, 16), highRes.pixelColor(64, 64));
}

void KisAiPhysicalRendererTest::testRenderProgramToPhysicalImage()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(128, 128);

    KisAiStrokeOperation opFlats;
    opFlats.layer = QStringLiteral("Flats");
    opFlats.kind = KisAiStrokeOperation::Kind::Fill;
    opFlats.brush.color = QColor(220, 180, 140);
    opFlats.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2)
                                 << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
    prog.operations.append(opFlats);

    KisAiStrokeOperation opShading;
    opShading.layer = QStringLiteral("Shading");
    opShading.kind = KisAiStrokeOperation::Kind::Fill;
    opShading.brush.color = QColor(100, 80, 60);
    opShading.polygon = QPolygonF() << QPointF(0.4, 0.4) << QPointF(0.8, 0.4)
                                   << QPointF(0.8, 0.8) << QPointF(0.4, 0.8);
    prog.operations.append(opShading);

    const QImage rendered = KisAiPhysicalRenderer::renderProgramToPhysicalImage(prog, QSize(128, 128), true, -1.0, 2);
    QVERIFY(!rendered.isNull());
    QCOMPARE(rendered.size(), QSize(128, 128));

    // 中央付近 (64, 64) は Shading が Flats に乗算合成されている
    const QColor centerCol = rendered.pixelColor(64, 64);
    QVERIFY(centerCol.alpha() > 200);

    // 外側 (10, 10) は透明
    const QColor cornerCol = rendered.pixelColor(10, 10);
    QVERIFY(cornerCol.alpha() < 10);
}

KISTEST_MAIN(KisAiPhysicalRendererTest)
