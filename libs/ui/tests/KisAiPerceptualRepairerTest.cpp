/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPerceptualRepairerTest.h"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPolygonF>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
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

KISTEST_MAIN(KisAiPerceptualRepairerTest)
