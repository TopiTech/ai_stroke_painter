/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiCoverageRasterTest.h"

#include <QImage>
#include <QtMath>
#ifndef AI_STROKE_STANDALONE
#include <testui.h>
#else
#include <QTest>
#ifndef KISTEST_MAIN
#define KISTEST_MAIN(TestClass) QTEST_MAIN(TestClass)
#endif
#endif

#include "aiillustration/KisAiStrokeCoverageRaster.h"
#include "aiillustration/KisAiStrokeProgram.h"
#include "aiillustration/KisAiStrokeRenderer.h"

using KisAiStrokeCoverageRaster::StrokeSample;

namespace
{
constexpr int kCanvas = 256;

KisAiStrokeOperation pathOp(const QString &id,
                            const QString &profile,
                            const QVector<KisAiStrokePoint> &points,
                            qreal opacity,
                            bool closed = false,
                            qreal size = 0.03)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = id;
    op.layer = QStringLiteral("Lineart");
    op.brush.profile = profile;
    op.brush.color = QColor(20, 20, 30);
    op.brush.opacity = opacity;
    op.brush.size = size;
    op.points = points;
    op.closed = closed;
    op.smooth = true;
    return op;
}

QImage renderOp(const KisAiStrokeOperation &op, int canvas = kCanvas)
{
    KisAiStrokeProgram prog;
    prog.operations = {op};
    return KisAiStrokeRenderer::renderProgramToImage(prog, QSize(canvas, canvas));
}

int maxAlpha(const QImage &img)
{
    int m = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            m = qMax(m, img.pixelColor(x, y).alpha());
        }
    }
    return m;
}

int alphaAt(const QImage &img, const QPointF &pt)
{
    const int x = qBound(0, qRound(pt.x()), img.width() - 1);
    const int y = qBound(0, qRound(pt.y()), img.height() - 1);
    return img.pixelColor(x, y).alpha();
}

int alphaNear(const QImage &img, const QPointF &pt, int radius = 2)
{
    int m = 0;
    const int cx = qRound(pt.x());
    const int cy = qRound(pt.y());
    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            if (x >= 0 && y >= 0 && x < img.width() && y < img.height()) {
                m = qMax(m, img.pixelColor(x, y).alpha());
            }
        }
    }
    return m;
}
} // namespace

void KisAiCoverageRasterTest::testSanity()
{
    QCOMPARE(1, 1);
}

void KisAiCoverageRasterTest::testNoAlphaBuildupOnSelfOverlap()
{
    // Closed ring: every join disk overlaps its neighbours — the classic
    // beading setup. At opacity 0.5 a single-composite stroke tops out at 0.5.
    QVector<KisAiStrokePoint> ring;
    const QPointF ringCenter(0.5, 0.5);
    for (int i = 0; i < 16; ++i) {
        const qreal a = 2.0 * M_PI * i / 16;
        ring.append(KisAiStrokePoint(ringCenter.x() + std::cos(a) * 0.3, ringCenter.y() + std::sin(a) * 0.3, 0.8));
    }
    const QImage ringImg = renderOp(pathOp(QStringLiteral("ring_probe"), QStringLiteral("gpen"), ring, 0.5, true));
    QVERIFY(!ringImg.isNull());
    const int ringMax = maxAlpha(ringImg);
    QVERIFY2(ringMax <= 140, qPrintable(QStringLiteral("ring max alpha=%1 (beading)").arg(ringMax)));

    // Dense S-curve: join disks overlap the body along the whole run.
    const QVector<KisAiStrokePoint> sCurve = {
        KisAiStrokePoint(0.15, 0.70, 0.8),
        KisAiStrokePoint(0.38, 0.35, 0.8),
        KisAiStrokePoint(0.62, 0.65, 0.8),
        KisAiStrokePoint(0.85, 0.30, 0.8),
    };
    const QImage sImg = renderOp(pathOp(QStringLiteral("s_probe"), QStringLiteral("gpen"), sCurve, 0.5));
    QVERIFY(!sImg.isNull());
    const int sMax = maxAlpha(sImg);
    QVERIFY2(sMax <= 140, qPrintable(QStringLiteral("S-curve max alpha=%1 (beading)").arg(sMax)));
}

void KisAiCoverageRasterTest::testStrokeInteriorAlphaUniform()
{
    // A densely sampled wave: stabilisation keeps the shape within a pixel, so
    // the analytic curve is a fair probe path for the inked interior.
    QVector<KisAiStrokePoint> pts;
    for (int k = 0; k <= 10; ++k) {
        const qreal x = 0.15 + 0.07 * k;
        const qreal y = 0.5 + 0.18 * std::sin(2.0 * M_PI * (x - 0.15) / 0.70);
        pts.append(KisAiStrokePoint(x, y, 0.9));
    }
    const QImage img = renderOp(pathOp(QStringLiteral("uniform_probe"), QStringLiteral("gpen"), pts, 0.6, false, 0.04));
    QVERIFY(!img.isNull());

    QVector<QPointF> ctrl;
    for (const KisAiStrokePoint &p : pts) {
        ctrl.append(QPointF(p.pos.x() * kCanvas, p.pos.y() * kCanvas));
    }
    const QVector<QPointF> curve = KisAiStrokeRenderer::generateCatmullRomSpline(ctrl, 8, false);
    QVERIFY(curve.size() > 20);

    int lo = 255;
    int hi = 0;
    const int from = curve.size() / 5;
    const int to = curve.size() - curve.size() / 5;
    for (int i = from; i < to; ++i) {
        const int a = alphaNear(img, curve.at(i));
        lo = qMin(lo, a);
        hi = qMax(hi, a);
    }
    // Interior alpha is one value (0.6) everywhere: no join beading, no
    // filament stacking. Tapered tips are excluded from the sampled range.
    QVERIFY2(hi - lo <= 34, qPrintable(QStringLiteral("interior alpha range=%1..%2").arg(lo).arg(hi)));
    QVERIFY2(lo >= 130, qPrintable(QStringLiteral("interior alpha too low: %1").arg(lo)));
}

void KisAiCoverageRasterTest::testSharpCornerHasNoCoverageGap()
{
    const QVector<KisAiStrokePoint> pts = {
        KisAiStrokePoint(0.25, 0.25, 0.8),
        KisAiStrokePoint(0.50, 0.50, 0.8),
        KisAiStrokePoint(0.25, 0.75, 0.8),
    };
    const QImage img = renderOp(pathOp(QStringLiteral("corner_probe"), QStringLiteral("gpen"), pts, 0.8, false, 0.05));
    QVERIFY(!img.isNull());

    // Ring around the corner vertex must be fully covered (no outer-side gap).
    // Stroke half-width is ~5px here, so a 3px ring stays inside the ink.
    const QPointF corner(0.50 * kCanvas, 0.50 * kCanvas);
    int lo = 255;
    for (int i = 0; i < 24; ++i) {
        const qreal a = 2.0 * M_PI * i / 24;
        const QPointF probe = corner + QPointF(std::cos(a) * 3.0, std::sin(a) * 3.0);
        lo = qMin(lo, alphaNear(img, probe, 1));
    }
    QVERIFY2(lo >= 150, qPrintable(QStringLiteral("corner ring min alpha=%1 (coverage gap)").arg(lo)));
}

void KisAiCoverageRasterTest::testLongSegmentFlatness()
{
    // A long, strongly curved 3-point arc must be sampled densely enough that
    // the polyline hugs the analytic curve (the old fixed cap of 24 facets per
    // segment visibly faceted spans like this one).
    QVector<QPointF> curvy = {QPointF(20, 250), QPointF(256, 24), QPointF(492, 250)};
    QVector<qreal> pressures = {0.8, 0.8, 0.8};
    KisAiStrokeBrush brush;
    brush.profile = QStringLiteral("gpen");
    brush.size = 0.01;
    brush.sizeMode = QStringLiteral("ratio");

    const QVector<StrokeSample> samples =
        KisAiStrokeCoverageRaster::sampleStroke(curvy, pressures, false, true, brush, QSize(512, 512), 1);
    QVERIFY(samples.size() > 2);

    const QVector<QPointF> truth = KisAiStrokeRenderer::generateCatmullRomSpline(curvy, 128, false);
    QVERIFY(truth.size() > 100);

    auto distToPolyline = [](const QPointF &p, const QVector<StrokeSample> &poly) {
        qreal best = 1.0e9;
        for (int i = 0; i + 1 < poly.size(); ++i) {
            const QPointF a = poly.at(i).pos;
            const QPointF b = poly.at(i + 1).pos;
            const QPointF ab = b - a;
            const qreal len2 = ab.x() * ab.x() + ab.y() * ab.y();
            qreal t = 0.0;
            if (len2 > 1.0e-9) {
                t = qBound<qreal>(0.0, ((p.x() - a.x()) * ab.x() + (p.y() - a.y()) * ab.y()) / len2, 1.0);
            }
            const QPointF proj = a + ab * t;
            best = qMin(best, std::hypot(p.x() - proj.x(), p.y() - proj.y()));
        }
        return best;
    };

    qreal worst = 0.0;
    for (const QPointF &p : truth) {
        worst = qMax(worst, distToPolyline(p, samples));
    }
    QVERIFY2(worst <= 0.35, qPrintable(QStringLiteral("polyline deviates %1px from the curve").arg(worst)));

    // Straight spans stay cheap: subdivision only chases the width/taper
    // profile, never geometry that is already flat (depth cap 7 bounds it).
    QVector<QPointF> straight = {QPointF(10, 10), QPointF(12, 10), QPointF(14, 10)};
    const QVector<StrokeSample> flat =
        KisAiStrokeCoverageRaster::sampleStroke(straight, pressures, false, true, brush, QSize(512, 512), 1);
    QVERIFY(flat.size() < 40);
}

void KisAiCoverageRasterTest::testTaperedTipAndRoundCap()
{
    // Marker keeps 70% width at the entry: the round start cap is fat enough
    // to probe. Tapered ends shrink the cap, so the tip test uses thin ink.
    const QVector<KisAiStrokePoint> capPts = {
        KisAiStrokePoint(0.20, 0.50, 1.0),
        KisAiStrokePoint(0.50, 0.50, 1.0),
        KisAiStrokePoint(0.80, 0.50, 1.0),
    };
    const QImage img =
        renderOp(pathOp(QStringLiteral("cap_probe"), QStringLiteral("marker"), capPts, 1.0, false, 0.05));
    QVERIFY(!img.isNull());

    // Round start cap: the ring around the first point is fully covered.
    const QPointF start(0.20 * kCanvas, 0.50 * kCanvas);
    int lo = 255;
    for (int i = 0; i < 16; ++i) {
        const qreal a = 2.0 * M_PI * i / 16;
        lo = qMin(lo, alphaNear(img, start + QPointF(std::cos(a) * 2.5, std::sin(a) * 2.5), 1));
    }
    QVERIFY2(lo >= 150, qPrintable(QStringLiteral("start cap ring min alpha=%1").arg(lo)));

    // Tapered tip keeps the width monotone: the start is heavier than the end.
    const QVector<KisAiStrokePoint> pts = {
        KisAiStrokePoint(0.20, 0.50, 1.0),
        KisAiStrokePoint(0.50, 0.50, 0.7),
        KisAiStrokePoint(0.80, 0.50, 0.15),
    };
    const QImage thin = renderOp(pathOp(QStringLiteral("taper_probe"), QStringLiteral("fineliner"), pts, 1.0, false, 0.02));
    int startInk = 0;
    int endInk = 0;
    for (int y = 110; y < 146; ++y) {
        for (int x = 40; x < 80; ++x) {
            if (thin.pixelColor(x, y).alpha() > 10)
                ++startInk;
        }
        for (int x = 180; x < 220; ++x) {
            if (thin.pixelColor(x, y).alpha() > 10)
                ++endInk;
        }
    }
    QVERIFY(startInk > 0);
    QVERIFY(endInk > 0);
    QVERIFY2(startInk >= endInk,
             qPrintable(QStringLiteral("start ink=%1 end ink=%2").arg(startInk).arg(endInk)));
}

void KisAiCoverageRasterTest::testProfileTexturesModulateNotStack()
{
    // Bristle ("brush") body is 0.72 and strand accents top out at 0.82 —
    // under single compositing the whole stroke stays below 0.85 even at full
    // opacity. The old stacked passes reached ~0.95 at joins and strands.
    const QVector<KisAiStrokePoint> pts = {
        KisAiStrokePoint(0.15, 0.60, 0.8),
        KisAiStrokePoint(0.40, 0.40, 0.8),
        KisAiStrokePoint(0.65, 0.60, 0.8),
        KisAiStrokePoint(0.88, 0.40, 0.8),
    };
    const QImage img = renderOp(pathOp(QStringLiteral("bristle_probe"), QStringLiteral("brush"), pts, 1.0, false, 0.04));
    QVERIFY(!img.isNull());
    const int m = maxAlpha(img);
    QVERIFY2(m <= 225, qPrintable(QStringLiteral("brush max alpha=%1 (texture stacking)").arg(m)));
    QVERIFY2(m >= 175, qPrintable(QStringLiteral("brush max alpha=%1 (body missing)").arg(m)));

    // Watercolor wash body is 0.65, wet-edge rim 0.92 — never above 0.95.
    const QImage wash = renderOp(pathOp(QStringLiteral("wash_probe"), QStringLiteral("watercolor"), pts, 1.0, false, 0.04));
    QVERIFY(!wash.isNull());
    const int wm = maxAlpha(wash);
    QVERIFY2(wm <= 245, qPrintable(QStringLiteral("watercolor max alpha=%1 (texture stacking)").arg(wm)));
}

void KisAiCoverageRasterTest::testFineLineNoJoinBeading()
{
    const QVector<KisAiStrokePoint> pts = {
        KisAiStrokePoint(0.20, 0.35, 0.7),
        KisAiStrokePoint(0.45, 0.55, 0.9),
        KisAiStrokePoint(0.70, 0.35, 0.4),
    };
    const QImage img = renderOp(pathOp(QStringLiteral("fine_probe"), QStringLiteral("fineliner"), pts, 0.5, false, 0.004));
    QVERIFY(!img.isNull());
    const int m = maxAlpha(img);
    QVERIFY2(m <= 140, qPrintable(QStringLiteral("fineliner max alpha=%1 (join beading)").arg(m)));
    QVERIFY2(m >= 90, qPrintable(QStringLiteral("fineliner max alpha=%1 (ink missing)").arg(m)));
}

void KisAiCoverageRasterTest::testRibbonSelfOverlapNoBuildup()
{
    KisAiStrokeOperation ribbon;
    ribbon.kind = KisAiStrokeOperation::Kind::Ribbon;
    ribbon.id = QStringLiteral("ribbon_probe");
    ribbon.layer = QStringLiteral("Flats");
    ribbon.brush.color = QColor(120, 60, 90);
    ribbon.brush.opacity = 0.5;
    ribbon.spine = {QPointF(0.15, 0.30), QPointF(0.45, 0.45), QPointF(0.75, 0.30), QPointF(0.85, 0.70)};
    ribbon.widthStart = 0.03;
    ribbon.widthMid = 0.05;
    ribbon.widthEnd = 0.01;

    KisAiStrokeProgram prog;
    prog.operations = {ribbon};
    const QImage img = KisAiStrokeRenderer::renderProgramToImage(prog, QSize(kCanvas, kCanvas));
    QVERIFY(!img.isNull());
    const int m = maxAlpha(img);
    QVERIFY2(m <= 140, qPrintable(QStringLiteral("ribbon max alpha=%1 (self-overlap buildup)").arg(m)));
}

void KisAiCoverageRasterTest::testCornerPoolNoBeading()
{
    // Corner ink fillets union into the same coverage mask: a sharp-cornered
    // stroke at opacity 0.5 must not darken past 0.5 at the pooling corner.
    const QVector<KisAiStrokePoint> pts = {
        KisAiStrokePoint(0.20, 0.30, 0.8),
        KisAiStrokePoint(0.55, 0.50, 0.8),
        KisAiStrokePoint(0.20, 0.70, 0.8),
    };
    const QImage img = renderOp(pathOp(QStringLiteral("fillet_probe"), QStringLiteral("gpen"), pts, 0.5, false, 0.03));
    QVERIFY(!img.isNull());
    const int m = maxAlpha(img);
    QVERIFY2(m <= 140, qPrintable(QStringLiteral("corner-pool max alpha=%1 (fillet stacking)").arg(m)));
}

KISTEST_MAIN(KisAiCoverageRasterTest)
