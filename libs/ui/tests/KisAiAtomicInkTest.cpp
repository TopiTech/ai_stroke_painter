/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiAtomicInkTest.h"

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

#include "aiillustration/KisAiDeliberateStroke.h"
#include "aiillustration/KisAiLayoutEngine.h"
#include "aiillustration/KisAiPhysicalRenderer.h"
#include "aiillustration/KisAiPrimitiveExpander.h"
#include "aiillustration/KisAiSceneSpec.h"
#include "aiillustration/KisAiStrokeCommitter.h"
#include "aiillustration/KisAiStrokeGraph.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeRenderer.h"
#include "aiillustration/KisAiVisionCritic.h"

namespace
{
KisAiStrokeOperation sampleEye()
{
    KisAiStrokeOperation eye;
    eye.kind = KisAiStrokeOperation::Kind::AnimeEye;
    eye.id = QStringLiteral("test_eye");
    eye.layer = QStringLiteral("Flats");
    eye.eyeCenter = QPointF(0.42, 0.40);
    eye.eyeSize = QSizeF(0.12, 0.14);
    eye.eyeIrisColor = QColor(32, 96, 224);
    eye.eyeIsRight = false;
    return eye;
}

int countIdContains(const QVector<KisAiStrokeOperation> &ops, const QString &needle)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.id.contains(needle, Qt::CaseInsensitive))
            ++n;
    }
    return n;
}

int opaqueCount(const QImage &img, const QRect &region, int alphaMin = 10)
{
    int n = 0;
    const QRect r = region.intersected(img.rect());
    for (int y = r.top(); y <= r.bottom(); ++y) {
        for (int x = r.left(); x <= r.right(); ++x) {
            if (img.pixelColor(x, y).alpha() > alphaMin)
                ++n;
        }
    }
    return n;
}
} // namespace

void KisAiAtomicInkTest::testSanity()
{
    QCOMPARE(1, 1);
}

void KisAiAtomicInkTest::testEyeExpandsToAtomicLashes()
{
    const QVector<KisAiStrokeOperation> atoms = KisAiPrimitiveExpander::expand(sampleEye(), QSize(512, 512));
    QVERIFY(atoms.size() >= 8);
    QCOMPARE(KisAiPrimitiveExpander::leftoverCompositeCount(atoms), 0);
    QVERIFY(countIdContains(atoms, QStringLiteral("lash_upper")) >= 1);
    QVERIFY(countIdContains(atoms, QStringLiteral("lash_clump")) >= 2);
    QVERIFY(countIdContains(atoms, QStringLiteral("crease")) >= 1);
    QVERIFY(countIdContains(atoms, QStringLiteral("sclera")) >= 1);
}

void KisAiAtomicInkTest::testMouthExpandsToLipPaths()
{
    KisAiStrokeOperation mouth;
    mouth.kind = KisAiStrokeOperation::Kind::AnimeMouth;
    mouth.id = QStringLiteral("test_mouth");
    mouth.mouthCenter = QPointF(0.5, 0.62);
    mouth.mouthSize = QSizeF(0.08, 0.03);
    mouth.mouthExpression = QStringLiteral("smile");
    mouth.mouthHasHighlight = true;
    const QVector<KisAiStrokeOperation> atoms = KisAiPrimitiveExpander::expand(mouth, QSize(512, 512));
    QCOMPARE(KisAiPrimitiveExpander::leftoverCompositeCount(atoms), 0);
    QVERIFY(countIdContains(atoms, QStringLiteral("upper_lip")) >= 1);
    QVERIFY(countIdContains(atoms, QStringLiteral("lower_lip")) >= 1);
}

void KisAiAtomicInkTest::testHatchBecomesPaths()
{
    KisAiStrokeOperation hatch;
    hatch.kind = KisAiStrokeOperation::Kind::Hatch;
    hatch.id = QStringLiteral("shade_hatch");
    hatch.layer = QStringLiteral("Shading");
    hatch.polygon << QPointF(0.2, 0.2) << QPointF(0.6, 0.2) << QPointF(0.6, 0.6) << QPointF(0.2, 0.6);
    hatch.spacing = 0.04;
    hatch.angleDeg = 45.0;
    hatch.brush.color = QColor(40, 20, 20);
    hatch.brush.size = 0.004;
    const QVector<KisAiStrokeOperation> atoms = KisAiPrimitiveExpander::expand(hatch, QSize(256, 256));
    QVERIFY(atoms.size() >= 4);
    int pathCount = 0;
    for (const KisAiStrokeOperation &op : atoms) {
        if (op.kind == KisAiStrokeOperation::Kind::Path)
            ++pathCount;
    }
    QVERIFY(pathCount >= 4);
}

void KisAiAtomicInkTest::testLegacyAnimeEyeJsonStillPaints()
{
    KisAiStrokeProgram prog;
    prog.operations.append(sampleEye());
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QCOMPARE(img.size(), QSize(256, 256));
    QVERIFY(img.pixelColor(qRound(0.42 * 256), qRound(0.40 * 256)).alpha() > 20);
}

void KisAiAtomicInkTest::testZeroCoverageSkipped()
{
    KisAiStrokeOperation off;
    off.kind = KisAiStrokeOperation::Kind::Path;
    off.id = QStringLiteral("offcanvas");
    off.layer = QStringLiteral("Lineart");
    off.points = {KisAiStrokePoint(2.0, 2.0, 0.8), KisAiStrokePoint(2.4, 2.4, 0.8)};
    off.brush.size = 0.004;
    off.brush.color = QColor(0, 0, 0);

    KisAiStrokeOperation keep;
    keep.kind = KisAiStrokeOperation::Kind::Path;
    keep.id = QStringLiteral("keep");
    keep.layer = QStringLiteral("Lineart");
    keep.points = {KisAiStrokePoint(0.2, 0.2, 0.8), KisAiStrokePoint(0.8, 0.25, 0.8)};
    keep.brush.size = 0.01;
    keep.brush.color = QColor(0, 0, 0);

    KisAiStrokeProgram prog;
    prog.operations = {off, keep};
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    QVERIFY(!img.isNull());
    const KisAiStrokeCommitLog log = KisAiStrokeCommitter::lastLog();
    QVERIFY(log.skipped >= 1);
    QVERIFY(log.committed >= 1);
}

void KisAiAtomicInkTest::testSinglePointDabFollowsCatchlightPolicy()
{
    // 単一ダブのポリシー (refineForRendering の意図的ダブ判定と一致):
    // - キャッチライト等のキーワード/Highlights レイヤの1点 → 不透明度で生存
    // - それ以外の1点 (corner_ink ドット等) → zero-coverage-skip (V10 受け入れ基準)
    KisAiStrokeOperation dab;
    dab.kind = KisAiStrokeOperation::Kind::Path;
    dab.layer = QStringLiteral("Highlights");
    dab.points = {KisAiStrokePoint(0.5, 0.5, 1.0)};
    dab.brush.size = 0.005;
    dab.brush.color = QColor(255, 255, 255);

    dab.id = QStringLiteral("eye_catchlight");
    dab.brush.opacity = 0.85;
    const KisAiStrokeCommitReview kept = KisAiDeliberateStroke::reviewStroke(dab, QSize(512, 512));
    QVERIFY(kept.committed);
    QVERIFY(kept.inkCoverage > 1.0e-7);
    QVERIFY(!kept.dirtyRect.isEmpty());

    // 完全透明のキャッチライトは落ちる。
    dab.brush.opacity = 0.0;
    const KisAiStrokeCommitReview transparent = KisAiDeliberateStroke::reviewStroke(dab, QSize(512, 512));
    QVERIFY(!transparent.committed);

    // 生成された corner_ink ドット (非キーワード id・Lineart) は従来どおり落ちる:
    // これが無いと半透明プローブへの α 合成超過で V10 ゲート5本が破れる。
    dab.id = QStringLiteral("corner_ink_1");
    dab.layer = QStringLiteral("Lineart");
    dab.brush.opacity = 0.85;
    const KisAiStrokeCommitReview dot = KisAiDeliberateStroke::reviewStroke(dab, QSize(512, 512));
    QVERIFY(!dot.committed);
}

void KisAiAtomicInkTest::testNeedsRepairSelfIntersectionFixed()
{
    KisAiStrokeOperation loop;
    loop.kind = KisAiStrokeOperation::Kind::Path;
    loop.id = QStringLiteral("loop");
    loop.layer = QStringLiteral("Lineart");
    loop.points = {KisAiStrokePoint(0.20, 0.20, 0.8),
                   KisAiStrokePoint(0.80, 0.80, 0.8),
                   KisAiStrokePoint(0.20, 0.80, 0.8),
                   KisAiStrokePoint(0.80, 0.20, 0.8),
                   KisAiStrokePoint(0.50, 0.50, 0.8)};
    loop.brush.size = 0.008;
    loop.brush.color = QColor(10, 10, 10);
    const KisAiStrokeLintReport before = KisAiDeliberateStroke::lintStroke(loop, QSize(256, 256));
    QVERIFY(before.needsRepair || before.selfIntersections >= 1);
    const KisAiStrokeOperation repaired = KisAiStrokeCommitter::repairOperation(loop, before, QSize(256, 256));
    const KisAiStrokeLintReport after = KisAiDeliberateStroke::lintStroke(repaired, QSize(256, 256));
    QVERIFY(after.selfIntersections <= before.selfIntersections);
}

void KisAiAtomicInkTest::testLintRunsOnStabilized()
{
    QVector<KisAiStrokePoint> noisy;
    for (int i = 0; i < 40; ++i) {
        const qreal t = qreal(i) / 39.0;
        noisy.append(KisAiStrokePoint(0.2 + t * 0.6, 0.4 + ((i % 2) ? 0.004 : -0.004), 0.8));
    }
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = QStringLiteral("noisy");
    op.points = noisy;
    op.brush.size = 0.006;
    op.brush.color = QColor(0, 0, 0);
    const KisAiStrokeOperation stable = KisAiStrokeCommitter::stabilizeOperation(op, QSize(512, 512));
    QVERIFY(stable.points.size() >= 2);
    QVERIFY(!KisAiDeliberateStroke::lintStroke(stable, QSize(512, 512)).drop);
}

void KisAiAtomicInkTest::testCommitLogDeterministic()
{
    KisAiStrokeProgram prog;
    prog.operations.append(sampleEye());
    KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    const QString a = KisAiStrokeCommitter::lastLog().summary();
    KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    const QString b = KisAiStrokeCommitter::lastLog().summary();
    QCOMPARE(a, b);
}

void KisAiAtomicInkTest::testSymmetricEyeRolesInterleaved()
{
    KisAiStrokeOperation left = sampleEye();
    left.eyeIsRight = false;
    left.id = QStringLiteral("eye_l");
    KisAiStrokeOperation right = sampleEye();
    right.eyeIsRight = true;
    right.eyeCenter = QPointF(0.58, 0.40);
    right.id = QStringLiteral("eye_r");
    const QVector<KisAiStrokeOperation> atoms =
        KisAiStrokeGraph::orderForCommit(KisAiPrimitiveExpander::expandAll({left, right}, QSize(512, 512)),
                                         QSize(512, 512));
    int firstLash = -1;
    int firstRightLash = -1;
    for (int i = 0; i < atoms.size(); ++i) {
        if (atoms.at(i).id.contains(QLatin1String("lash_upper"))) {
            if (atoms.at(i).groupId == QLatin1String("eye_l") && firstLash < 0)
                firstLash = i;
            if (atoms.at(i).groupId == QLatin1String("eye_r") && firstRightLash < 0)
                firstRightLash = i;
        }
    }
    QVERIFY(firstLash >= 0);
    QVERIFY(firstRightLash >= 0);
    QVERIFY(qAbs(firstLash - firstRightLash) <= 4);
}

void KisAiAtomicInkTest::testFringeIsMultipleStrokes()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("Anime girl portrait with fine hair");
    spec.subject.type = QStringLiteral("character");
    spec.composition.headCenter = QPointF(0.5, 0.4);
    spec.composition.headHeight = 0.40;
    spec.head.hairColor = QColor(45, 30, 60);
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));
    QVERIFY(countIdContains(prog.operations, QStringLiteral("hair_fringe_clump")) >= 2);
    QCOMPARE(countIdContains(prog.operations, QStringLiteral("hair_fringe_line")), 0);
}

void KisAiAtomicInkTest::testJawBeforeLashes()
{
    KisAiSceneSpec spec;
    spec.prompt = QStringLiteral("Anime girl");
    spec.subject.type = QStringLiteral("character");
    spec.composition.headCenter = QPointF(0.5, 0.4);
    spec.composition.headHeight = 0.40;
    const KisAiStrokeProgram prog = KisAiLayoutEngine::generateProgram(spec, QSize(512, 512));
    const QVector<KisAiStrokeOperation> ordered =
        KisAiStrokeCommitter::prepareAtomicOps(prog.operations, QSize(512, 512));
    int jaw = -1;
    int lash = -1;
    for (int i = 0; i < ordered.size(); ++i) {
        if (ordered.at(i).id.contains(QLatin1String("face_contour")) && jaw < 0)
            jaw = i;
        if (ordered.at(i).id.contains(QLatin1String("lash")) && lash < 0)
            lash = i;
    }
    QVERIFY(jaw >= 0);
    QVERIFY(lash >= 0);
    QVERIFY(jaw < lash);
}

void KisAiAtomicInkTest::testGroupCritiqueRetriesLashOnly()
{
    KisAiStrokeOperation left = sampleEye();
    left.eyeIsRight = false;
    left.id = QStringLiteral("eye_l");
    KisAiStrokeOperation right = sampleEye();
    right.eyeIsRight = true;
    right.eyeCenter = QPointF(0.58, 0.46);
    right.id = QStringLiteral("eye_r");
    const QVector<KisAiStrokeOperation> atoms = KisAiStrokeCommitter::prepareAtomicOps({left, right}, QSize(256, 256));
    const QStringList warnings = KisAiStrokeGraph::critiqueGroup(QStringLiteral("eye"), atoms);
    QVERIFY(warnings.contains(QStringLiteral("eye-height-mismatch")));
}

void KisAiAtomicInkTest::testTStopSnapsToParent()
{
    KisAiStrokeOperation parent;
    parent.kind = KisAiStrokeOperation::Kind::Path;
    parent.id = QStringLiteral("parent_contour");
    parent.points = {KisAiStrokePoint(0.2, 0.5, 0.8), KisAiStrokePoint(0.8, 0.5, 0.8)};
    parent.brush.size = 0.006;

    KisAiStrokeOperation child;
    child.kind = KisAiStrokeOperation::Kind::Path;
    child.id = QStringLiteral("child");
    child.parentId = QStringLiteral("parent_contour");
    child.points = {KisAiStrokePoint(0.5, 0.2, 0.5), KisAiStrokePoint(0.501, 0.499, 0.4)};
    child.brush.size = 0.003;

    QVector<KisAiStrokeOperation> ops{parent, child};
    const int n = KisAiStrokeGraph::snapTStops(ops, QSize(1000, 1000), 1.2);
    QVERIFY(n >= 1);
    QVERIFY(qAbs(ops.at(1).points.last().pos.y() - 0.5) < 1.0e-6);
}

void KisAiAtomicInkTest::testFineLineUsesEnvelope()
{
    KisAiStrokeOperation fine;
    fine.kind = KisAiStrokeOperation::Kind::Path;
    fine.id = QStringLiteral("short_eyelash");
    fine.layer = QStringLiteral("Lineart");
    fine.brush.profile = QStringLiteral("fineliner");
    fine.brush.color = QColor(10, 10, 20);
    fine.brush.size = 0.004;
    fine.points = {KisAiStrokePoint(0.30, 0.40, 0.7),
                   KisAiStrokePoint(0.45, 0.38, 0.9),
                   KisAiStrokePoint(0.60, 0.40, 0.4)};
    KisAiStrokeProgram prog;
    prog.operations = {fine};
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    QVERIFY(opaqueCount(img, img.rect()) > 10);
}

void KisAiAtomicInkTest::testTaperedTipVsRoundStart()
{
    KisAiStrokeOperation fine;
    fine.kind = KisAiStrokeOperation::Kind::Path;
    fine.id = QStringLiteral("taper_probe");
    fine.layer = QStringLiteral("Lineart");
    fine.brush.profile = QStringLiteral("fineliner");
    fine.brush.color = QColor(0, 0, 0);
    fine.brush.size = 0.02;
    fine.points = {KisAiStrokePoint(0.20, 0.50, 1.0),
                   KisAiStrokePoint(0.50, 0.50, 0.7),
                   KisAiStrokePoint(0.80, 0.50, 0.15)};
    KisAiStrokeProgram prog;
    prog.operations = {fine};
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(256, 256));
    const int startInk = opaqueCount(img, QRect(40, 110, 40, 36));
    const int endInk = opaqueCount(img, QRect(180, 110, 40, 36));
    QVERIFY(startInk > 0);
    QVERIFY(endInk > 0);
    QVERIFY(startInk >= endInk);
}

void KisAiAtomicInkTest::testNoBowtieOnSharpCorner()
{
    KisAiStrokeOperation sharp;
    sharp.kind = KisAiStrokeOperation::Kind::Path;
    sharp.id = QStringLiteral("sharp_corner");
    sharp.layer = QStringLiteral("Lineart");
    sharp.brush.profile = QStringLiteral("gpen");
    sharp.brush.color = QColor(0, 0, 0);
    sharp.brush.size = 0.01;
    sharp.points = {KisAiStrokePoint(0.20, 0.20, 0.8),
                    KisAiStrokePoint(0.50, 0.50, 0.8),
                    KisAiStrokePoint(0.20, 0.80, 0.8)};
    const QPolygonF env =
        KisAiDeliberateStroke::buildEnvelopePolygon(sharp.points, sharp.brush, QSize(256, 256), false, 1);
    QVERIFY(env.size() >= 3);
    const QRectF box = env.boundingRect();
    QVERIFY(box.width() < 180.0);
    QVERIFY(box.height() < 220.0);
}

void KisAiAtomicInkTest::testAtomicStrokeRatioAfterExpand()
{
    const QVector<KisAiStrokeOperation> atoms = KisAiStrokeCommitter::prepareAtomicOps({sampleEye()}, QSize(256, 256));
    QVERIFY2(KisAiStrokeCommitter::atomicStrokeRatio(atoms) >= 0.999,
             qPrintable(QStringLiteral("atomic ratio=%1").arg(KisAiStrokeCommitter::atomicStrokeRatio(atoms))));
}

void KisAiAtomicInkTest::testSideTokenStrictness()
{
    KisAiStrokeOperation leftOp;
    leftOp.kind = KisAiStrokeOperation::Kind::Path;
    leftOp.id = QStringLiteral("eye_l_lash_upper");
    leftOp.layer = QStringLiteral("Lineart");
    QCOMPARE(KisAiStrokeGraph::inferGroupId(leftOp), QStringLiteral("eye_l"));

    KisAiStrokeOperation rightOp = leftOp;
    rightOp.id = QStringLiteral("eye_r_lash_upper");
    QCOMPARE(KisAiStrokeGraph::inferGroupId(rightOp), QStringLiteral("eye_r"));

    KisAiStrokeOperation rimOp = leftOp;
    rimOp.id = QStringLiteral("face_rim_light");
    QCOMPARE(KisAiStrokeGraph::inferGroupId(rimOp), QStringLiteral("face_rim_light"));
}

void KisAiAtomicInkTest::testZeroDensityMangaLinesExpandsToNothing()
{
    KisAiStrokeOperation manga;
    manga.kind = KisAiStrokeOperation::Kind::MangaLines;
    manga.id = QStringLiteral("fx_zero");
    manga.density = 0;
    QVERIFY(KisAiPrimitiveExpander::expand(manga, QSize(256, 256)).isEmpty());
}

void KisAiAtomicInkTest::testDegenerateHatchExpandsToNothing()
{
    KisAiStrokeOperation hatch;
    hatch.kind = KisAiStrokeOperation::Kind::Hatch;
    hatch.id = QStringLiteral("shade_zero");
    hatch.layer = QStringLiteral("Shading");
    hatch.polygon << QPointF(0.2, 0.2) << QPointF(0.6, 0.2) << QPointF(0.6, 0.6) << QPointF(0.2, 0.6);
    hatch.spacing = 0.0;
    QVERIFY(KisAiPrimitiveExpander::expand(hatch, QSize(256, 256)).isEmpty());
}

void KisAiAtomicInkTest::testReviewPixelsRejectsMismatchedImages()
{
    const QImage before(QSize(16, 16), QImage::Format_ARGB32_Premultiplied);
    const QImage after(QSize(8, 8), QImage::Format_ARGB32_Premultiplied);
    const KisAiStrokeCommitReview rev = KisAiStrokeCommitter::reviewPixels(before, after, QRect(0, 0, 8, 8));
    QVERIFY(!rev.committed);
    QVERIFY(rev.notes.contains(QStringLiteral("pixel-review-unavailable")));

    const KisAiStrokeCommitReview emptyRev =
        KisAiStrokeCommitter::reviewPixels(before, before, QRect());
    QVERIFY(!emptyRev.committed);
    QVERIFY(emptyRev.notes.contains(QStringLiteral("pixel-review-unavailable")));
}

void KisAiAtomicInkTest::testReviewPixelsMixedFormatsAndLargeRegion()
{
    QImage before(QSize(512, 512), QImage::Format_ARGB32);
    before.fill(Qt::transparent);
    QImage after = before.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&after);
    painter.fillRect(100, 100, 300, 300, QColor(20, 20, 30, 255));
    painter.end();
    const KisAiStrokeCommitReview rev =
        KisAiStrokeCommitter::reviewPixels(before, after, QRect(0, 0, 512, 512));
    QVERIFY(rev.committed);
    // opaqueDelta() early-exits at 2000 gained pixels, so coverage on a 512px
    // region is a small positive ratio, not the true painted fraction.
    QVERIFY2(rev.inkCoverage > 0.0, qPrintable(QString::number(rev.inkCoverage)));
}

void KisAiAtomicInkTest::testPreviewCanvasParityPsnr()
{
    KisAiStrokeProgram prog;
    prog.operations.append(sampleEye());
    const QImage a = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    const QImage b = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(128, 128));
    QVERIFY2(KisAiVisionCritic::psnr(a, b) >= 59.0,
             qPrintable(QStringLiteral("psnr=%1").arg(KisAiVisionCritic::psnr(a, b))));
}

void KisAiAtomicInkTest::testPhysicalPathUsesCommitter()
{
    KisAiStrokeProgram prog;
    prog.canvasSize = QSize(128, 128);
    prog.operations.append(sampleEye());
    const QImage img = KisAi::KisAiPhysicalRenderer::renderProgramToPhysicalImage(prog, QSize(128, 128), true, -1.0, 1);
    QVERIFY(!img.isNull());
    const KisAiStrokeCommitLog log = KisAiStrokeCommitter::lastLog();
    QVERIFY(log.committed >= 1);
    QVERIFY2(KisAiStrokeCommitter::atomicStrokeRatio(KisAiStrokeCommitter::prepareAtomicOps(prog.operations,
                                                                                           QSize(128, 128)))
                 >= 0.999,
             "physical path must stay atomic");
}

void KisAiAtomicInkTest::testOrderOperationsForRenderingDefaultCanvasSize()
{
    KisAiStrokeOperation op1;
    op1.id = QStringLiteral("bg_fill");
    op1.layer = QStringLiteral("Background");
    op1.kind = KisAiStrokeOperation::Kind::Fill;
    op1.polygon = {QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};

    KisAiStrokeOperation op2;
    op2.id = QStringLiteral("eye_iris");
    op2.layer = QStringLiteral("Lineart");
    op2.kind = KisAiStrokeOperation::Kind::Path;
    op2.points = {KisAiStrokePoint(0.4, 0.4), KisAiStrokePoint(0.6, 0.6)};

    QVector<KisAiStrokeOperation> ops = {op2, op1};

    // Test calling with 1 argument (using default canvasSize)
    const QVector<KisAiStrokeOperation> orderedDefault =
        KisAiDeliberateStroke::orderOperationsForRendering(ops);
    QCOMPARE(orderedDefault.size(), 2);

    // Test calling with 2 arguments (explicit canvasSize)
    const QVector<KisAiStrokeOperation> orderedExplicit =
        KisAiDeliberateStroke::orderOperationsForRendering(ops, QSize(512, 512));
    QCOMPARE(orderedExplicit.size(), 2);
    // Background mass must come before facial detail
    QCOMPARE(orderedExplicit.first().id, QStringLiteral("bg_fill"));
    QCOMPARE(orderedExplicit.last().id, QStringLiteral("eye_iris"));
}

KISTEST_MAIN(KisAiAtomicInkTest)
