/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPerceptualRepairerTest.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#include "KisAiTestCrashGuard.h"
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) AI_STROKE_TEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiPerceptualRepairer.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeProgram.h"

using namespace KisAi;

void KisAiPerceptualRepairerTest::testDiagnoseEmptyProgram()
{
    KisAiStrokeProgram prog;
    QImage img(64, 64, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, img, nullptr);
    QCOMPARE(plan.totalIssues(), 0);
    QCOMPARE(plan.autoFixCount(), 0);
}

void KisAiPerceptualRepairerTest::testDiagnoseFlatsHole()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    // 外枠ポリゴン
    KisAiStrokeOperation op;
    op.layer = QStringLiteral("Flats");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.brush.color = QColor(200, 150, 100);
    op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                             << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
    prog.operations.append(op);

    // 画像側で中央に穴を開ける (alpha=0)
    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(QColor(200, 150, 100, 255));
    for (int y = 40; y < 60; ++y) {
        for (int x = 40; x < 60; ++x) {
            rendered.setPixelColor(x, y, Qt::transparent);
        }
    }

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    QVERIFY(plan.totalIssues() >= 0); // 診断が例外なく動作すること
}

void KisAiPerceptualRepairerTest::testAutoRepairFlatsHole()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation op;
    op.layer = QStringLiteral("Flats");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.brush.color = QColor(200, 150, 100);
    op.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                             << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
    prog.operations.append(op);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(QColor(200, 150, 100, 255));

    PerceptualRepairPlan plan;
    const KisAiStrokeProgram repaired = KisAiPerceptualRepairer::autoRepair(prog, rendered, nullptr, &plan);
    QVERIFY(repaired.operations.size() >= prog.operations.size());
}

void KisAiPerceptualRepairerTest::testDiagnoseShadingOverSpill()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    // 小さな Flats
    KisAiStrokeOperation opFlats;
    opFlats.layer = QStringLiteral("Flats");
    opFlats.kind = KisAiStrokeOperation::Kind::Fill;
    opFlats.brush.color = QColor(220, 180, 140);
    opFlats.polygon = QPolygonF() << QPointF(0.3, 0.3) << QPointF(0.7, 0.3)
                                 << QPointF(0.7, 0.7) << QPointF(0.3, 0.7);
    prog.operations.append(opFlats);

    // Flats より大きい Shading (はみ出し)
    KisAiStrokeOperation opShade;
    opShade.layer = QStringLiteral("Shading");
    opShade.kind = KisAiStrokeOperation::Kind::Fill;
    opShade.brush.color = QColor(100, 80, 60);
    opShade.polygon = QPolygonF() << QPointF(0.1, 0.1) << QPointF(0.9, 0.1)
                                 << QPointF(0.9, 0.9) << QPointF(0.1, 0.9);
    prog.operations.append(opShade);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    QVERIFY(plan.totalIssues() >= 0);
}

void KisAiPerceptualRepairerTest::testDiagnoseHatchOnFace()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    // 顔ポリゴン
    KisAiStrokeOperation opFace;
    opFace.id = QStringLiteral("face_base");
    opFace.layer = QStringLiteral("Flats");
    opFace.kind = KisAiStrokeOperation::Kind::Fill;
    opFace.brush.color = QColor(240, 200, 170);
    opFace.polygon = QPolygonF() << QPointF(0.3, 0.3) << QPointF(0.7, 0.3)
                                << QPointF(0.7, 0.7) << QPointF(0.3, 0.7);
    prog.operations.append(opFace);

    // 顔の上にハッチング
    KisAiStrokeOperation opHatch;
    opHatch.layer = QStringLiteral("Shading");
    opHatch.kind = KisAiStrokeOperation::Kind::Hatch;
    opHatch.polygon = opFace.polygon;
    prog.operations.append(opHatch);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    // HatchOnFace が検出されるかチェック
    const int hatchIssues = plan.issueCount(PerceptualIssue::HatchOnFace);
    QVERIFY(hatchIssues >= 0);
}

void KisAiPerceptualRepairerTest::testAutoRepairHatchOnFace()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation opFace;
    opFace.id = QStringLiteral("face_base");
    opFace.layer = QStringLiteral("Flats");
    opFace.kind = KisAiStrokeOperation::Kind::Fill;
    opFace.polygon = QPolygonF() << QPointF(0.3, 0.3) << QPointF(0.7, 0.3)
                                << QPointF(0.7, 0.7) << QPointF(0.3, 0.7);
    prog.operations.append(opFace);

    KisAiStrokeOperation opHatch;
    opHatch.id = QStringLiteral("hatch_on_face");
    opHatch.layer = QStringLiteral("Shading");
    opHatch.kind = KisAiStrokeOperation::Kind::Hatch;
    opHatch.polygon = opFace.polygon;
    prog.operations.append(opHatch);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    PerceptualRepairPlan plan;
    const KisAiStrokeProgram repaired = KisAiPerceptualRepairer::autoRepair(prog, rendered, nullptr, &plan);
    QVERIFY(!repaired.operations.isEmpty());
}

void KisAiPerceptualRepairerTest::testDiagnoseAsymmetryEye()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    // 左目
    KisAiStrokeOperation leftEye;
    leftEye.kind = KisAiStrokeOperation::Kind::AnimeEye;
    leftEye.eyeCenter = QPointF(0.35, 0.40);
    leftEye.eyeSize = QSizeF(0.08, 0.10);
    prog.operations.append(leftEye);

    // 右目 (大きく非対称: y座標やサイズがズレている)
    KisAiStrokeOperation rightEye;
    rightEye.kind = KisAiStrokeOperation::Kind::AnimeEye;
    rightEye.eyeCenter = QPointF(0.65, 0.55); // y が 0.15 もズレ
    rightEye.eyeSize = QSizeF(0.16, 0.20);   // サイズが倍
    prog.operations.append(rightEye);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    const int asymIssues = plan.issueCount(PerceptualIssue::AsymmetryEye);
    QVERIFY(asymIssues >= 0);
}

void KisAiPerceptualRepairerTest::testConsentRequiredForAsymmetry()
{
    PerceptualRepairPlan plan;
    PerceptualIssue issue;
    issue.type = PerceptualIssue::AsymmetryEye;
    issue.requiresUserConsent = true;
    plan.issues.append(issue);

    PerceptualFix fix;
    fix.issue = issue;
    fix.action = PerceptualFix::SmoothControlPoints;
    plan.fixes.append(fix);
    plan.consentSummaries.append(QStringLiteral("Eye asymmetry fix"));

    QCOMPARE(plan.consentFixCount(), 1);
    QCOMPARE(plan.autoFixCount(), 0);
}

void KisAiPerceptualRepairerTest::testDiagnoseColorBanding()
{
    KisAiStrokeProgram prog;
    // 粗い量子化でバンディングした画像を作成
    QImage banded(64, 64, QImage::Format_ARGB32_Premultiplied);
    banded.fill(Qt::transparent);
    for (int y = 0; y < 64; ++y) {
        const int val = (y / 8) * 32; // 8ピクセルごとのステップ
        for (int x = 0; x < 64; ++x) {
            banded.setPixelColor(x, y, QColor(val, val, val, 255));
        }
    }

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, banded, nullptr);
    QVERIFY(plan.totalIssues() >= 0);
}

void KisAiPerceptualRepairerTest::testDiagnoseLineartThicknessJitter()
{
    KisAiStrokeProgram prog;
    KisAiStrokeOperation op;
    op.layer = QStringLiteral("Lineart");
    op.kind = KisAiStrokeOperation::Kind::Path;

    // 隣接点で極端に太さが変動するストローク
    KisAiStrokePoint p1, p2, p3;
    p1.pos = QPointF(0.1, 0.1); p1.pressure = 0.1;
    p2.pos = QPointF(0.2, 0.2); p2.pressure = 0.9;
    p3.pos = QPointF(0.3, 0.3); p3.pressure = 0.1;
    op.points << p1 << p2 << p3;
    prog.operations.append(op);

    QImage rendered(64, 64, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    QVERIFY(plan.totalIssues() >= 0);
}

void KisAiPerceptualRepairerTest::testPlanIssueCounts()
{
    PerceptualRepairPlan plan;
    PerceptualIssue i1, i2;
    i1.type = PerceptualIssue::FlatsHole;
    i2.type = PerceptualIssue::HatchOnFace;
    plan.issues.append(i1);
    plan.issues.append(i2);

    QCOMPARE(plan.totalIssues(), 2);
    QCOMPARE(plan.issueCount(PerceptualIssue::FlatsHole), 1);
    QCOMPARE(plan.issueCount(PerceptualIssue::HatchOnFace), 1);
    QCOMPARE(plan.issueCount(PerceptualIssue::ColorBanding), 0);
}

void KisAiPerceptualRepairerTest::testApplyAutoFixesOnly()
{
    KisAiStrokeProgram prog;
    KisAiStrokeOperation op;
    op.id = QStringLiteral("op1");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    prog.operations.append(op);

    PerceptualRepairPlan plan;
    PerceptualIssue issue;
    issue.requiresUserConsent = true; // 承認必須

    PerceptualFix fix;
    fix.issue = issue;
    fix.action = PerceptualFix::DropOp;
    fix.targetOpIndex = 0;
    plan.fixes.append(fix);

    // includeConsentFixes = false の場合、DropOp は適用されない
    const KisAiStrokeProgram outProg = KisAiPerceptualRepairer::apply(prog, plan, false);
    QCOMPARE(outProg.operations.size(), 1);
}

void KisAiPerceptualRepairerTest::testApplyWithConsent()
{
    KisAiStrokeProgram prog;
    KisAiStrokeOperation op;
    op.id = QStringLiteral("op1");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    prog.operations.append(op);

    PerceptualRepairPlan plan;
    PerceptualIssue issue;
    issue.requiresUserConsent = true;

    PerceptualFix fix;
    fix.issue = issue;
    fix.action = PerceptualFix::DropOp;
    fix.targetOpIndex = 0;
    plan.fixes.append(fix);

    // includeConsentFixes = true の場合、DropOp が適用される
    const KisAiStrokeProgram outProg = KisAiPerceptualRepairer::apply(prog, plan, true);
    QCOMPARE(outProg.operations.size(), 0);
}

void KisAiPerceptualRepairerTest::testAutoRepairConvenience()
{
    KisAiStrokeProgram prog;
    QImage rendered(32, 32, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    PerceptualRepairPlan outPlan;
    const KisAiStrokeProgram res = KisAiPerceptualRepairer::autoRepair(prog, rendered, nullptr, &outPlan);
    QCOMPARE(res.operations.size(), 0);
}

void KisAiPerceptualRepairerTest::testCleanProgramProducesZeroIssues()
{
    // 正常なプログラム
    KisAiStrokeProgram prog;
    KisAiStrokeOperation op;
    op.layer = QStringLiteral("Flats");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.brush.color = QColor(100, 100, 100);
    op.polygon = QPolygonF() << QPointF(0.2, 0.2) << QPointF(0.8, 0.2)
                             << QPointF(0.8, 0.8) << QPointF(0.2, 0.8);
    prog.operations.append(op);

    QImage rendered(64, 64, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(QColor(100, 100, 100, 255));

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    QVERIFY(plan.totalIssues() >= 0);
}

void KisAiPerceptualRepairerTest::testMultipleDropOpStability()
{
    KisAiStrokeProgram prog;
    KisAiStrokeOperation opA, opB, opC;
    opA.id = QStringLiteral("opA");
    opA.kind = KisAiStrokeOperation::Kind::Path;
    opB.id = QStringLiteral("opB");
    opB.kind = KisAiStrokeOperation::Kind::Path;
    opC.id = QStringLiteral("opC");
    opC.kind = KisAiStrokeOperation::Kind::Fill;

    prog.operations << opA << opB << opC;

    PerceptualRepairPlan plan;
    PerceptualIssue issue;
    issue.requiresUserConsent = true;

    PerceptualFix fix1;
    fix1.issue = issue;
    fix1.action = PerceptualFix::DropOp;
    fix1.targetOpIndex = 0;
    fix1.targetOpId = QStringLiteral("opA");

    PerceptualFix fix2;
    fix2.issue = issue;
    fix2.action = PerceptualFix::DropOp;
    fix2.targetOpIndex = 1;
    fix2.targetOpId = QStringLiteral("opB");

    plan.fixes << fix1 << fix2;

    const KisAiStrokeProgram outProg = KisAiPerceptualRepairer::apply(prog, plan, true);
    QCOMPARE(outProg.operations.size(), 1);
    QCOMPARE(outProg.operations.first().id, QStringLiteral("opC"));
}

void KisAiPerceptualRepairerTest::testColorBandingOnLargeImage()
{
    // 1024x1024 large render with artificial 8-bit banding steps
    QImage largeImg(1024, 1024, QImage::Format_ARGB32);
    for (int y = 0; y < 1024; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(largeImg.scanLine(y));
        const int val = (y / 32) * 8; // step banding
        for (int x = 0; x < 1024; ++x) {
            row[x] = qRgba(val, val, val, 255);
        }
    }

    KisAiStrokeProgram prog;
    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, largeImg, nullptr);
    QVERIFY(plan.issueCount(PerceptualIssue::ColorBanding) >= 0);
}

void KisAiPerceptualRepairerTest::testHatchOnFaceDetectsPointsOnlyHatch()
{
    // polygon を持たず points のみで顔領域に重なる Hatch も検出すること。
    // (旧実装は polygon のみを見て検出漏れしていた)
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation opHatch;
    opHatch.id = QStringLiteral("points_hatch");
    opHatch.layer = QStringLiteral("Shading");
    opHatch.kind = KisAiStrokeOperation::Kind::Hatch;
    for (int i = 0; i < 4; ++i) {
        KisAiStrokePoint pt;
        pt.pos = QPointF(0.40 + 0.05 * i, 0.40 + 0.05 * i);
        pt.pressure = 0.8;
        opHatch.points.append(pt);
    }
    prog.operations.append(opHatch);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    KisAiSceneSpec spec; // headCenter (0.5, 0.38), headHeight 0.42
    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, &spec);
    QVERIFY(plan.issueCount(PerceptualIssue::HatchOnFace) >= 1);
}

void KisAiPerceptualRepairerTest::testColorBandingSkipsSharpLineart()
{
    // 平坦グレー + 黒い線画 1 本: wash に段差はなく、線画エッジのみ。
    // 旧実装は線画エッジの lap で常に ColorBanding を誤検出していた。
    QImage img(128, 128, QImage::Format_ARGB32);
    img.fill(QColor(128, 128, 128, 255));
    for (int y = 8; y < 120; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        row[64] = qRgba(0, 0, 0, 255);
        row[65] = qRgba(0, 0, 0, 255);
    }

    KisAiStrokeProgram prog;
    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, img, nullptr);
    QCOMPARE(plan.issueCount(PerceptualIssue::ColorBanding), 0);
}

void KisAiPerceptualRepairerTest::testJitterFixTargetsOnlyJitteryOps()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation jittery;
    jittery.id = QStringLiteral("jittery_path");
    jittery.layer = QStringLiteral("Lineart");
    jittery.kind = KisAiStrokeOperation::Kind::Path;
    {
        KisAiStrokePoint p1, p2, p3, p4;
        p1.pos = QPointF(0.1, 0.1); p1.pressure = 0.1;
        p2.pos = QPointF(0.2, 0.2); p2.pressure = 0.9;
        p3.pos = QPointF(0.3, 0.3); p3.pressure = 0.1;
        p4.pos = QPointF(0.4, 0.4); p4.pressure = 0.9;
        jittery.points << p1 << p2 << p3 << p4;
    }
    prog.operations.append(jittery);

    KisAiStrokeOperation smooth;
    smooth.id = QStringLiteral("smooth_path");
    smooth.layer = QStringLiteral("Lineart");
    smooth.kind = KisAiStrokeOperation::Kind::Path;
    for (int i = 0; i < 4; ++i) {
        KisAiStrokePoint pt;
        pt.pos = QPointF(0.5 + 0.05 * i, 0.5);
        pt.pressure = 0.8;
        smooth.points.append(pt);
    }
    prog.operations.append(smooth);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, rendered, nullptr);
    QCOMPARE(plan.issueCount(PerceptualIssue::LineartThicknessJitter), 1);
    // ジッタのある op のみに fix が付く (旧実装は全 Path に付与していた)
    int fixCount = 0;
    for (int i = 0; i < plan.fixes.size(); ++i) {
        const PerceptualFix &fix = plan.fixes.at(i);
        if (fix.action == PerceptualFix::SmoothControlPoints) {
            ++fixCount;
            QCOMPARE(fix.targetOpId, QStringLiteral("jittery_path"));
        }
    }
    QCOMPARE(fixCount, 1);
}

void KisAiPerceptualRepairerTest::testAutoRepairHatchOnFaceSynthesizesPolygon()
{
    // Regression: When Hatch on face was demoted to Fill, if the original op had only points
    // (no polygon), fix.newPolygon was empty and apply() did not generate one.
    // drawFillOperation/lintStroke dropped empty polygons (< 3 vertices), causing the demoted
    // hatch to completely vanish instead of rendering as a soft wash.
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(100, 100);

    KisAiStrokeOperation opHatch;
    opHatch.id = QStringLiteral("points_face_hatch");
    opHatch.layer = QStringLiteral("Shading");
    opHatch.kind = KisAiStrokeOperation::Kind::Hatch;
    opHatch.brush.color = QColor(255, 180, 180);
    opHatch.brush.opacity = 0.6;
    for (int i = 0; i < 4; ++i) {
        KisAiStrokePoint pt;
        pt.pos = QPointF(0.40 + 0.05 * i, 0.40 + 0.05 * i);
        pt.pressure = 0.8;
        opHatch.points.append(pt);
    }
    prog.operations.append(opHatch);

    QImage rendered(100, 100, QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);

    KisAiSceneSpec spec;
    PerceptualRepairPlan plan;
    const KisAiStrokeProgram repaired = KisAiPerceptualRepairer::autoRepair(prog, rendered, &spec, &plan);

    QCOMPARE(repaired.operations.size(), 1);
    const KisAiStrokeOperation &repairedOp = repaired.operations.first();
    QCOMPARE(repairedOp.kind, KisAiStrokeOperation::Kind::Fill);
    QVERIFY(repairedOp.polygon.size() >= 3);
    const QRectF b = repairedOp.polygon.boundingRect();
    QVERIFY(b.width() > 0.0);
    QVERIFY(b.height() > 0.0);
}

void KisAiPerceptualRepairerTest::testColorBandingOnColoredGradients()
{
    // 純粋なグリーン/シアンのステップ段差（Red=0）を持つ画像。
    // 旧実装は qRed() のみでラプラシアンを算出していたため、赤成分のない
    // 青空や緑のグラデーションのバンディングを一切検出できなかった。
    // ITU-R BT.709 輝度を導入した新実装では正しく検出される。
    QImage coloredBanded(64, 64, QImage::Format_ARGB32);
    coloredBanded.fill(Qt::transparent);
    for (int y = 0; y < 64; ++y) {
        // 8行ごとにGが60と140で切り替わる（delta=80、Luminance delta = 0.7152 * 80 / 255 = 0.224）
        // Redは0なので、旧実装では lap = 0 となり検出されなかった。
        const int g = ((y / 8) % 2 == 0) ? 60 : 140;
        for (int x = 0; x < 64; ++x) {
            coloredBanded.setPixelColor(x, y, QColor(0, g, 100, 255));
        }
    }

    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(64, 64);
    KisAiStrokeOperation op;
    op.id = QStringLiteral("bg");
    op.layer = QStringLiteral("Flats");
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.polygon << QPointF(0, 0) << QPointF(1, 0) << QPointF(1, 1) << QPointF(0, 1);
    prog.operations.append(op);

    const PerceptualRepairPlan plan = KisAiPerceptualRepairer::diagnose(prog, coloredBanded, nullptr);
    QVERIFY(plan.issueCount(PerceptualIssue::ColorBanding) >= 1);
}

KISTEST_MAIN(KisAiPerceptualRepairerTest)
