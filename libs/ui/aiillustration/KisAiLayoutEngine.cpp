/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiLayoutEngine.h"
#include "KisAiLightRig.h"
#include "KisAiRigLibrary.h"
#include "KisAiStrokeQualityUtils.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

namespace
{
QPolygonF ellipsePolygon(const QPointF &center, qreal rx, qreal ry, int segments = 20)
{
    QPolygonF poly;
    poly.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        const qreal t = 2.0 * M_PI * i / segments;
        poly.append(QPointF(center.x() + rx * std::cos(t), center.y() + ry * std::sin(t)));
    }
    return poly;
}

QColor darkerWarm(const QColor &c, qreal factor = 0.82)
{
    if (c.hue() < 0)
        return QColor(qBound(0, int(c.red() * factor), 255),
                      qBound(0, int(c.green() * factor), 255),
                      qBound(0, int(c.blue() * factor), 255),
                      c.alpha());
    return QColor::fromHsv((c.hue() + 360) % 360,
                           qBound(0, int(c.saturation() * 1.05), 255),
                           qBound(0, int(c.value() * factor), 255),
                           c.alpha());
}

QColor mixToward(const QColor &base, const QColor &target, qreal t)
{
    const qreal k = qBound<qreal>(0.0, t, 1.0);
    return QColor::fromRgbF(base.redF() * (1.0 - k) + target.redF() * k,
                            base.greenF() * (1.0 - k) + target.greenF() * k,
                            base.blueF() * (1.0 - k) + target.blueF() * k,
                            base.alphaF() * (1.0 - k) + target.alphaF() * k);
}

// V6 W1: resolve narrative.time fallback + colorScript midtone blend once,
// so every downstream rig shares one resolved spec.
KisAiSceneSpec resolveSpecForLayout(const KisAiSceneSpec &spec)
{
    KisAiSceneSpec out = spec;
    if ((out.light.timeOfDay == QLatin1String("day") || out.light.timeOfDay.trimmed().isEmpty())
        && !out.narrative.time.trimmed().isEmpty())
        out.light.timeOfDay = KisAiRigLibrary::narrativeTimeToTimeOfDay(out.narrative.time, out.light.timeOfDay);
    if (out.colorScript.midtone.isValid() && out.colorScript.midtone.alpha() > 0) {
        const qreal t = qBound<qreal>(0.0, out.colorScript.accentWeight, 1.0) * 0.35;
        const QColor mid = out.colorScript.midtone;
        out.head.hairColor = mixToward(out.head.hairColor, mid, t);
        out.head.skinTone = mixToward(out.head.skinTone, mid, t);
        out.head.eyeColor = mixToward(out.head.eyeColor, mid, t);
        out.clothing.color = mixToward(out.clothing.color, mid, t);
        out.clothing.secondaryColor = mixToward(out.clothing.secondaryColor, mid, t);
        out.clothing.accentColor = mixToward(out.clothing.accentColor, mid, t);
    }
    return out;
}

// V6 W1:副次装飾 budget. Only full detail earns lid shadow + tear trough.
bool decorationPolicyForDetail(qreal detailLevel)
{
    return qBound<qreal>(0.0, detailLevel, 1.0) >= 0.45;
}

void sampleCubicBezier(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, int steps, QPolygonF *poly)
{
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / steps;
        const qreal it = 1.0 - t;
        const QPointF pt = it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
        poly->append(pt);
    }
}
} // namespace

KisAiStrokeOperation KisAiLayoutEngine::makeFill(const QString &id,
                                                 const QString &layer,
                                                 const QPolygonF &polygon,
                                                 const QColor &color,
                                                 const QString &profile,
                                                 qreal opacity,
                                                 const QString &style)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.id = id;
    op.layer = layer;
    op.polygon = polygon;
    op.brush.profile = profile;
    op.brush.color = color;
    op.brush.opacity = opacity;
    op.brush.size = 0.03;
    op.fillStyle = style;
    return op;
}

KisAiStrokeOperation KisAiLayoutEngine::makePath(const QString &id,
                                                 const QString &layer,
                                                 const QVector<KisAiStrokePoint> &points,
                                                 const QColor &color,
                                                 const QString &profile,
                                                 qreal size,
                                                 qreal opacity)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = id;
    op.layer = layer;
    op.points = points;
    op.brush.profile = profile;
    op.brush.color = color;
    op.brush.size = size;
    op.brush.opacity = opacity;
    op.closed = false;
    op.smooth = true;
    return op;
}

QPolygonF KisAiLayoutEngine::headOutlinePolygon(const QPointF &center, qreal width, qreal height)
{
    // V7: High-continuity cubic Bezier curvature from KisAiRigLibrary
    return KisAiRigLibrary::headOutlineBezier(center, width, height, 0.0);
}

QPair<QPointF, QPointF>
KisAiLayoutEngine::eyePairCenters(const QPointF &headCenter, qreal headWidth, qreal headHeight, const QString &facing)
{
    const qreal eyeY = headCenter.y() + headHeight * 0.08;
    qreal spread = headWidth * 0.19;
    qreal shift = 0.0;
    if (facing == QLatin1String("front-right"))
        shift = headWidth * 0.03;
    else if (facing == QLatin1String("front-left"))
        shift = -headWidth * 0.03;
    else if (facing == QLatin1String("profile"))
        spread = headWidth * 0.10;
    return qMakePair(QPointF(headCenter.x() - spread + shift, eyeY), QPointF(headCenter.x() + spread + shift, eyeY));
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairBackMassForStyle(const KisAiSceneSpec &spec,
                                                                      const QPointF &headCenter,
                                                                      qreal headWidth,
                                                                      qreal headHeight)
{
    QVector<KisAiStrokeOperation> ops;
    const QColor hair = spec.head.hairColor;
    const QString style = spec.head.hairStyle;
    const qreal topY = headCenter.y() - headHeight * 0.5;
    const qreal chinY = headCenter.y() + headHeight * 0.5;

    qreal backLength = headHeight * 1.25; // long_hime default
    if (style == QLatin1String("bob"))
        backLength = headHeight * 0.72;
    else if (style == QLatin1String("short_messy") || style == QLatin1String("short_straight"))
        backLength = headHeight * 0.58;
    else if (style == QLatin1String("twin_tails"))
        backLength = headHeight * 1.35;
    else if (style == QLatin1String("long_wavy"))
        backLength = headHeight * 1.30;
    else if (style == QLatin1String("pony_tail"))
        backLength = headHeight * 0.45;
    else if (style == QLatin1String("half_up"))
        backLength = headHeight * 1.15;
    else if (style == QLatin1String("wolf_cut"))
        backLength = headHeight * 0.85;
    else if (style == QLatin1String("braided"))
        backLength = headHeight * 1.20;

    // 1. Back mass (inner shade behind head): Smooth cranial dome & soft taper
    {
        QPolygonF innerBack;
        const int topSteps = 24;
        for (int i = 0; i <= topSteps; ++i) {
            const qreal t = M_PI * (1.0 - (qreal)i / topSteps);
            const qreal bx = headCenter.x() + std::cos(t) * (headWidth * 0.62);
            const qreal by = headCenter.y() - std::sin(t) * (headHeight * 0.58);
            innerBack.append(QPointF(bx, by));
        }
        // Smooth Bezier contours down each flank
        sampleCubicBezier(QPointF(headCenter.x() + headWidth * 0.62, headCenter.y()),
                          QPointF(headCenter.x() + headWidth * 0.66, chinY + backLength * 0.35),
                          QPointF(headCenter.x() + headWidth * 0.48, chinY + backLength * 0.70),
                          QPointF(headCenter.x(), chinY + backLength * 0.72),
                          16, &innerBack);
        sampleCubicBezier(QPointF(headCenter.x(), chinY + backLength * 0.72),
                          QPointF(headCenter.x() - headWidth * 0.48, chinY + backLength * 0.70),
                          QPointF(headCenter.x() - headWidth * 0.66, chinY + backLength * 0.35),
                          QPointF(headCenter.x() - headWidth * 0.62, headCenter.y()),
                          16, &innerBack);
        ops.append(makeFill(QStringLiteral("hair_inner_shade"),
                            QStringLiteral("Flats"),
                            innerBack,
                            darkerWarm(hair, 0.72),
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));
    }

    // 2. Back mass (main volume behind the face): Full organic flow
    {
        QPolygonF back;
        // Smooth cranial top dome from left temple over crown to right temple
        const int topSteps = 28;
        for (int i = 0; i <= topSteps; ++i) {
            const qreal t = M_PI * (1.0 - (qreal)i / topSteps);
            const qreal bx = headCenter.x() + std::cos(t) * (headWidth * 0.68);
            const qreal by = headCenter.y() - std::sin(t) * (headHeight * 0.64);
            back.append(QPointF(bx, by));
        }

        const QPointF rTemple(headCenter.x() + headWidth * 0.68, headCenter.y());
        const QPointF lTemple(headCenter.x() - headWidth * 0.68, headCenter.y());

        if (style == QLatin1String("bob")) {
            sampleCubicBezier(rTemple,
                              QPointF(headCenter.x() + headWidth * 0.70, headCenter.y() + headHeight * 0.22),
                              QPointF(headCenter.x() + headWidth * 0.52, chinY + backLength * 0.32),
                              QPointF(headCenter.x(), chinY + backLength * 0.28),
                              16, &back);
            sampleCubicBezier(QPointF(headCenter.x(), chinY + backLength * 0.28),
                              QPointF(headCenter.x() - headWidth * 0.52, chinY + backLength * 0.32),
                              QPointF(headCenter.x() - headWidth * 0.70, headCenter.y() + headHeight * 0.22),
                              lTemple,
                              16, &back);
        } else if (style == QLatin1String("short_messy") || style == QLatin1String("short_straight")) {
            sampleCubicBezier(rTemple,
                              QPointF(headCenter.x() + headWidth * 0.64, headCenter.y() + headHeight * 0.15),
                              QPointF(headCenter.x() + headWidth * 0.38, chinY + backLength * 0.28),
                              QPointF(headCenter.x(), chinY + backLength * 0.24),
                              16, &back);
            sampleCubicBezier(QPointF(headCenter.x(), chinY + backLength * 0.24),
                              QPointF(headCenter.x() - headWidth * 0.38, chinY + backLength * 0.28),
                              QPointF(headCenter.x() - headWidth * 0.64, headCenter.y() + headHeight * 0.15),
                              lTemple,
                              16, &back);
        } else {
            // long_hime / long_wavy default: flowing S-curve down shoulders with natural tips
            sampleCubicBezier(rTemple,
                              QPointF(headCenter.x() + headWidth * 0.72, headCenter.y() + headHeight * 0.26),
                              QPointF(headCenter.x() + headWidth * 0.60, chinY + backLength * 0.55),
                              QPointF(headCenter.x() + headWidth * 0.30, chinY + backLength * 0.76),
                              18, &back);
            sampleCubicBezier(QPointF(headCenter.x() + headWidth * 0.30, chinY + backLength * 0.76),
                              QPointF(headCenter.x() + headWidth * 0.12, chinY + backLength * 0.70),
                              QPointF(headCenter.x() - headWidth * 0.12, chinY + backLength * 0.70),
                              QPointF(headCenter.x() - headWidth * 0.30, chinY + backLength * 0.76),
                              14, &back);
            sampleCubicBezier(QPointF(headCenter.x() - headWidth * 0.30, chinY + backLength * 0.76),
                              QPointF(headCenter.x() - headWidth * 0.60, chinY + backLength * 0.55),
                              QPointF(headCenter.x() - headWidth * 0.72, headCenter.y() + headHeight * 0.26),
                              lTemple,
                              18, &back);
        }
        ops.append(makeFill(QStringLiteral("hair_back_mass"),
                            QStringLiteral("Flats"),
                            back,
                            hair,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));
    }

    // 3. Twin tails (if selected - tails hang behind/at sides)
    if (style == QLatin1String("twin_tails")) {
        for (int side = -1; side <= 1; side += 2) {
            // Volumetric multi-point tail
            QPolygonF tail;
            const qreal xRoot = headCenter.x() + side * headWidth * 0.54;
            tail.append(QPointF(xRoot, topY + headHeight * 0.04));
            tail.append(QPointF(xRoot + side * headWidth * 0.25, topY - headHeight * 0.20));
            tail.append(QPointF(xRoot + side * headWidth * 0.48, topY - headHeight * 0.15));
            tail.append(QPointF(xRoot + side * headWidth * 0.60, headCenter.y() + headHeight * 0.20));
            tail.append(QPointF(xRoot + side * headWidth * 0.45, chinY + backLength * 0.60));
            tail.append(QPointF(xRoot + side * headWidth * 0.22, chinY + backLength * 0.68));
            tail.append(QPointF(xRoot + side * headWidth * 0.12, headCenter.y() + headHeight * 0.30));
            ops.append(makeFill(side < 0 ? QStringLiteral("hair_tail_l") : QStringLiteral("hair_tail_r"),
                                QStringLiteral("Flats"),
                                tail,
                                darkerWarm(hair, 0.92),
                                QStringLiteral("brush"),
                                1.0,
                                QStringLiteral("contour")));

            // Hair ribbon band (tie)
            QPolygonF band;
            const qreal bx = xRoot + side * headWidth * 0.15;
            const qreal by = topY - headHeight * 0.08;
            band.append(QPointF(bx - 0.02, by - 0.02));
            band.append(QPointF(bx + 0.02, by - 0.02));
            band.append(QPointF(bx + 0.02, by + 0.02));
            band.append(QPointF(bx - 0.02, by + 0.02));
            ops.append(makeFill(side < 0 ? QStringLiteral("hair_band_l") : QStringLiteral("hair_band_r"),
                                QStringLiteral("Flats"),
                                band,
                                spec.palette.accents.isEmpty() ? QColor(230, 60, 80) : spec.palette.accents.first(),
                                QStringLiteral("brush"),
                                1.0,
                                QStringLiteral("contour")));
        }
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairFrontMassForStyle(const KisAiSceneSpec &spec,
                                                                       const QPointF &headCenter,
                                                                       qreal headWidth,
                                                                       qreal headHeight)
{
    QVector<KisAiStrokeOperation> ops;
    const QColor hair = spec.head.hairColor;
    const QString bangs = spec.head.hairBangs;
    const qreal topY = headCenter.y() - headHeight * 0.5;
    const qreal chinY = headCenter.y() + headHeight * 0.5;
    const QString style = spec.head.hairStyle;

    qreal backLength = headHeight * 1.25;
    if (style == QLatin1String("bob"))
        backLength = headHeight * 0.72;
    else if (style == QLatin1String("short_messy") || style == QLatin1String("short_straight"))
        backLength = headHeight * 0.58;
    else if (style == QLatin1String("twin_tails"))
        backLength = headHeight * 1.35;
    else if (style == QLatin1String("long_wavy"))
        backLength = headHeight * 1.30;

    // 1. Main fringe clumps & crown volume
    {
        QPolygonF fringe;
        // Crown dome arc covering top of head with rich anime volume (matches back mass)
        const int crownSteps = 12;
        for (int i = 0; i <= crownSteps; ++i) {
            const qreal t = M_PI * (1.0 - (qreal)i / crownSteps); // PI to 0
            const qreal rx = headWidth * 0.66;
            const qreal ry = headHeight * 0.62;
            const qreal cx = headCenter.x() + std::cos(t) * rx;
            const qreal cy = headCenter.y() - std::sin(t) * ry;
            fringe.append(QPointF(cx, cy));
        }

        // Clump baseline from right temple down across forehead to left temple
        QVector<KisAiStrokePoint> fringeLine;
        if (bangs == QLatin1String("straight_cut")) {
            // Hime blunt cut bangs: clean horizontal line touching eye level
            const int cutSteps = 12;
            for (int i = cutSteps; i >= 0; --i) {
                const qreal x = headCenter.x() - headWidth * 0.54 + headWidth * 1.08 * i / cutSteps;
                const qreal dip = headHeight * 0.56 + ((i % 2 == 1) ? headHeight * 0.02 : 0.0);
                fringe.append(QPointF(x, topY + dip));
                fringeLine.append(KisAiStrokePoint(x, topY + dip, 0.8));
            }
        } else if (bangs == QLatin1String("swept_left") || bangs == QLatin1String("swept_right")) {
            // Asymmetric swept bangs
            const qreal sign = (bangs == QLatin1String("swept_left")) ? -1.0 : 1.0;
            const struct SweptPt {
                qreal xR;
                qreal dipR;
            } sweptPts[] = {
                {sign * 0.52, 0.70},
                {sign * 0.28, 0.64},
                {sign * 0.05, 0.58},
                {-sign * 0.20, 0.50},
                {-sign * 0.48, 0.44},
            };
            for (const auto &sp : sweptPts) {
                const qreal px = headCenter.x() + sp.xR * headWidth;
                const qreal py = topY + sp.dipR * headHeight;
                fringe.append(QPointF(px, py));
                fringeLine.append(KisAiStrokePoint(px, py, 0.8));
            }
        } else {
            // Classic Anime M-Fringe with smooth flowing clumps (Valley -> Clump Tip -> Valley)
            // Each clump is shaped by smooth Bezier arcs rather than raw coarse linear segments.
            struct ClumpSpec {
                qreal leftValleyX, leftValleyY;
                qreal tipX, tipY;
                qreal rightValleyX, rightValleyY;
            };
            const ClumpSpec clumps[] = {
                {-0.54, 0.70, -0.50, 0.68, -0.46, 0.48}, // Left temple lock
                {-0.46, 0.48, -0.36, 0.62, -0.26, 0.45}, // Left outer clump
                {-0.26, 0.45, -0.16, 0.59, -0.07, 0.43}, // Left center clump (over eye)
                {-0.07, 0.43,  0.00, 0.41,  0.07, 0.43}, // Center M slit
                { 0.07, 0.43,  0.16, 0.59,  0.26, 0.45}, // Right center clump (over eye)
                { 0.26, 0.45,  0.36, 0.62,  0.46, 0.48}, // Right outer clump
                { 0.46, 0.48,  0.50, 0.68,  0.54, 0.70}, // Right temple lock
            };

            for (const auto &cs : clumps) {
                const QPointF p0(headCenter.x() + cs.leftValleyX * headWidth, topY + cs.leftValleyY * headHeight);
                const QPointF pTip(headCenter.x() + cs.tipX * headWidth, topY + cs.tipY * headHeight);
                const QPointF p1(headCenter.x() + cs.rightValleyX * headWidth, topY + cs.rightValleyY * headHeight);

                // Sample down to tip
                for (int s = 0; s <= 6; ++s) {
                    const qreal t = qreal(s) / 6.0;
                    const qreal it = 1.0 - t;
                    const QPointF ctrl(p0.x() + (pTip.x() - p0.x()) * 0.6, p0.y() + (pTip.y() - p0.y()) * 0.85);
                    const QPointF pt = it * it * p0 + 2.0 * it * t * ctrl + t * t * pTip;
                    fringe.append(pt);
                    fringeLine.append(KisAiStrokePoint(pt.x(), pt.y(), 0.5 + 0.45 * t));
                }
                // Sample back up to valley
                for (int s = 1; s <= 6; ++s) {
                    const qreal t = qreal(s) / 6.0;
                    const qreal it = 1.0 - t;
                    const QPointF ctrl(pTip.x() + (p1.x() - pTip.x()) * 0.4, p1.y() + (pTip.y() - p1.y()) * 0.85);
                    const QPointF pt = it * it * pTip + 2.0 * it * t * ctrl + t * t * p1;
                    fringe.append(pt);
                    fringeLine.append(KisAiStrokePoint(pt.x(), pt.y(), 0.95 - 0.45 * t));
                }
            }
        }

        KisAiStrokeOperation fringeFill = makeFill(QStringLiteral("hair_fringe"),
                                                   QStringLiteral("Flats"),
                                                   fringe,
                                                   darkerWarm(hair, 0.96),
                                                   QStringLiteral("brush"),
                                                   1.0,
                                                   QStringLiteral("contour"));
        fringeFill.groupId = QStringLiteral("hair_fringe");
        fringeFill.role = QStringLiteral("mass");
        ops.append(fringeFill);

        // V9: one Path per fringe clump (valley-to-valley), never a single zigzag.
        if (fringeLine.size() >= 2) {
            int clumpIdx = 0;
            QVector<KisAiStrokePoint> current;
            current.reserve(16);
            for (int i = 0; i < fringeLine.size(); ++i) {
                current.append(fringeLine.at(i));
                const bool isValley =
                    (i > 0 && i + 1 < fringeLine.size() && fringeLine.at(i).pos.y() < fringeLine.at(i - 1).pos.y()
                     && fringeLine.at(i).pos.y() < fringeLine.at(i + 1).pos.y());
                const bool isEnd = (i == fringeLine.size() - 1);
                if ((isValley || isEnd) && current.size() >= 3) {
                    KisAiStrokeOperation clump = makePath(QStringLiteral("hair_fringe_clump_%1").arg(clumpIdx),
                                                          QStringLiteral("Lineart"),
                                                          current,
                                                          darkerWarm(hair, 0.65),
                                                          QStringLiteral("gpen"),
                                                          0.0032,
                                                          0.95);
                    clump.groupId = QStringLiteral("hair_fringe_%1").arg(clumpIdx);
                    clump.parentId = QStringLiteral("hair_fringe");
                    clump.role = QStringLiteral("contour");
                    ops.append(clump);
                    ++clumpIdx;
                    current.clear();
                    if (isValley)
                        current.append(fringeLine.at(i));
                }
            }
        }

        // Smooth flowing hair strands (no cross-hatch/wireframe lattice)
        const qreal strandOffsets[] = {-0.32, -0.14, 0.0, 0.14, 0.32};
        for (int s = 0; s < 5; ++s) {
            const qreal offX = strandOffsets[s] * headWidth;
            QVector<KisAiStrokePoint> strand;
            for (int i = 0; i <= 8; ++i) {
                const qreal t = qreal(i) / 8.0;
                const qreal px = headCenter.x() + offX * (0.35 + 0.65 * t) + std::sin(t * M_PI * 1.2) * (headWidth * 0.02);
                const qreal py = topY + headHeight * (0.08 + 0.40 * t);
                const qreal pressure = 0.2 + 0.65 * std::sin(M_PI * t);
                strand.append(KisAiStrokePoint(px, py, pressure));
            }
            ops.append(makePath(QStringLiteral("hair_strand_%1").arg(s),
                                QStringLiteral("Lineart"),
                                strand,
                                darkerWarm(hair, 0.72),
                                QStringLiteral("fineliner"),
                                0.0016,
                                0.75));
        }

        // D3-3: Bangs Skin Bleed (前髪の肌透け) - gentle soft wash at clump tips so eyebrows/eyes peek through
        QPolygonF bleedPoly;
        bleedPoly.reserve(fringeLine.size() * 2);
        for (const KisAiStrokePoint &pt : fringeLine) {
            bleedPoly.append(pt.pos);
        }
        for (int i = fringeLine.size() - 1; i >= 0; --i) {
            bleedPoly.append(QPointF(fringeLine[i].pos.x(), fringeLine[i].pos.y() - headHeight * 0.055));
        }
        if (bleedPoly.size() >= 3) {
            ops.append(makeFill(QStringLiteral("hair_bangs_bleed"),
                                QStringLiteral("Shading"),
                                bleedPoly,
                                spec.head.skinTone,
                                QStringLiteral("watercolor"),
                                0.18,
                                QStringLiteral("wash")));
        }
    }

    // 2. Side locks (顔周りの毛束): primary lock + delicate sub-lock
    for (int side = -1; side <= 1; side += 2) {
        // Main tapered side lock
        KisAiStrokeOperation lock;
        lock.kind = KisAiStrokeOperation::Kind::Ribbon;
        lock.id = side < 0 ? QStringLiteral("hair_side_lock_l") : QStringLiteral("hair_side_lock_r");
        lock.layer = QStringLiteral("Flats");
        lock.brush.profile = QStringLiteral("hair");
        lock.brush.color = hair;
        lock.brush.opacity = 1.0;
        const qreal x0 = headCenter.x() + side * headWidth * 0.50;
        lock.spine = QVector<QPointF>{
            QPointF(x0, topY + headHeight * 0.10),
            QPointF(x0 + side * headWidth * 0.06, headCenter.y() + headHeight * 0.22),
            QPointF(x0 + side * headWidth * 0.01, chinY + backLength * 0.35),
            QPointF(x0 - side * headWidth * 0.02, chinY + backLength * 0.48), // curving inward
        };
        lock.widthStart = 0.038;
        lock.widthMid = 0.028;
        lock.widthEnd = 0.005;
        ops.append(lock);

        // Side lock contour lineart
        QVector<KisAiStrokePoint> lockLine;
        for (int i = 0; i < lock.spine.size(); ++i) {
            const qreal p = (i == 0 || i == lock.spine.size() - 1) ? 0.35 : 0.85;
            lockLine.append(KisAiStrokePoint(lock.spine[i].x(), lock.spine[i].y(), p));
        }
        ops.append(makePath(side < 0 ? QStringLiteral("hair_side_line_l") : QStringLiteral("hair_side_line_r"),
                            QStringLiteral("Lineart"),
                            lockLine,
                            darkerWarm(hair, 0.65),
                            QStringLiteral("gpen"),
                            0.0028,
                            0.90));

        // Sub-strand (delicate secondary lock for fullness)
        KisAiStrokeOperation subLock;
        subLock.kind = KisAiStrokeOperation::Kind::Ribbon;
        subLock.id = side < 0 ? QStringLiteral("hair_sub_lock_l") : QStringLiteral("hair_sub_lock_r");
        subLock.layer = QStringLiteral("Flats");
        subLock.brush.profile = QStringLiteral("brush");
        subLock.brush.color = darkerWarm(hair, 0.88);
        subLock.brush.opacity = 0.95;
        subLock.spine = QVector<QPointF>{
            QPointF(x0 + side * headWidth * 0.05, topY + headHeight * 0.18),
            QPointF(x0 + side * headWidth * 0.09, headCenter.y() + headHeight * 0.28),
            QPointF(x0 + side * headWidth * 0.05, chinY + backLength * 0.30),
        };
        subLock.widthStart = 0.018;
        subLock.widthMid = 0.014;
        subLock.widthEnd = 0.003;
        ops.append(subLock);
    }

    // 4. Ahoge (アホ毛 - top flyaway strand for lively anime feel)
    {
        QVector<KisAiStrokePoint> ahoge;
        ahoge.append(KisAiStrokePoint(headCenter.x() + headWidth * 0.02, topY - headHeight * 0.10, 0.7));
        ahoge.append(KisAiStrokePoint(headCenter.x() + headWidth * 0.12, topY - headHeight * 0.26, 0.9));
        ahoge.append(KisAiStrokePoint(headCenter.x() + headWidth * 0.05, topY - headHeight * 0.32, 0.5));
        ahoge.append(KisAiStrokePoint(headCenter.x() - headWidth * 0.04, topY - headHeight * 0.28, 0.2));
        ops.append(makePath(QStringLiteral("hair_ahoge"),
                            QStringLiteral("Lineart"),
                            ahoge,
                            darkerWarm(hair, 0.85),
                            QStringLiteral("gpen"),
                            0.0035,
                            0.95));
    }

    // 5. Angel halo: multi-layer specular arc above the crown (Highlights)
    // 5a. Soft broad aura glow
    QVector<KisAiStrokePoint> haloAura;
    for (int i = 0; i <= 12; ++i) {
        const qreal t = M_PI * (0.12 + 0.76 * i / 12.0);
        haloAura.append(KisAiStrokePoint(headCenter.x() + std::cos(t) * headWidth * 0.36,
                                         topY - headHeight * 0.05 - std::sin(t) * headHeight * 0.10,
                                         0.30 + 0.65 * std::sin(M_PI * i / 12.0)));
    }
    ops.append(makePath(QStringLiteral("hair_angel_halo_aura"),
                        QStringLiteral("Highlights"),
                        haloAura,
                        QColor(255, 255, 255),
                        QStringLiteral("airbrush"),
                        0.008,
                        0.45));

    // 5b. Crisp specular core ribbon
    QVector<KisAiStrokePoint> haloCore;
    for (int i = 2; i <= 10; ++i) {
        const qreal t = M_PI * (0.12 + 0.76 * i / 12.0);
        haloCore.append(KisAiStrokePoint(headCenter.x() + std::cos(t) * headWidth * 0.36,
                                         topY - headHeight * 0.05 - std::sin(t) * headHeight * 0.10,
                                         0.20 + 0.80 * std::sin(M_PI * (i - 2) / 8.0)));
    }
    ops.append(makePath(QStringLiteral("hair_angel_halo_core"),
                        QStringLiteral("Highlights"),
                        haloCore,
                        QColor(255, 255, 255),
                        QStringLiteral("fineliner"),
                        0.0024,
                        0.85));

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairMassForStyle(const KisAiSceneSpec &spec,
                                                                  const QPointF &headCenter,
                                                                  qreal headWidth,
                                                                  qreal headHeight)
{
    QVector<KisAiStrokeOperation> ops = hairBackMassForStyle(spec, headCenter, headWidth, headHeight);
    ops.append(hairFrontMassForStyle(spec, headCenter, headWidth, headHeight));
    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::backgroundForSpec(const KisAiSceneSpec &spec, const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops;
    const QString tod = spec.light.timeOfDay;

    // V6 W2: single source of truth — sky stops derive from the LightRig LUT.
    const KisAiLightRig::TimeOfDayLut lut = KisAiLightRig::timeOfDayLut(tod);
    QColor top = lut.skyTop;
    QColor mid = lut.skyMid;
    QColor bottom = lut.skyBottom;
    if (spec.subject.type == QLatin1String("landscape") && !spec.palette.accents.isEmpty()) {
        const qreal t = qBound<qreal>(0.0, spec.colorScript.accentWeight, 1.0);
        bottom = mixToward(bottom, spec.palette.accents.first(), t);
    }

    KisAiStrokeOperation wash;
    wash.kind = KisAiStrokeOperation::Kind::GradientFill;
    wash.id = QStringLiteral("bg_wash");
    wash.layer = QStringLiteral("Background");
    wash.polygon = QPolygonF{}; // empty = full canvas
    // V6 W2: 3-stop sky comes from the LUT (zenith / horizon glow / ground haze).
    wash.gradientColors = QVector<QColor>{top, mid, bottom};
    wash.angleDeg = 90.0;
    wash.brush.profile = QStringLiteral("watercolor");
    wash.brush.color = top;
    wash.brush.opacity = 1.0;
    wash.fillStyle = QStringLiteral("directional");
    ops.append(wash);

    if (spec.background.type == QLatin1String("night_sky_town")
        || (tod == QLatin1String("night") && spec.background.elements.contains(QStringLiteral("moon")))) {
        ops.append(makeFill(QStringLiteral("bg_moon"),
                            QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.78, 0.18), 0.055, 0.055),
                            QColor(250, 244, 220),
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("wash")));
    }
    if (tod == QLatin1String("day") || tod == QLatin1String("sunset")) {
        ops.append(makeFill(QStringLiteral("bg_sun"),
                            QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.76, 0.20), 0.045, 0.045),
                            tod == QLatin1String("sunset") ? QColor(255, 150, 80) : QColor(255, 246, 220),
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("wash")));
    }
    if (spec.subject.type == QLatin1String("landscape")) {
        QPolygonF ridge;
        ridge.append(QPointF(0.0, 0.72));
        ridge.append(QPointF(0.18, 0.52));
        ridge.append(QPointF(0.36, 0.66));
        ridge.append(QPointF(0.55, 0.48));
        ridge.append(QPointF(0.74, 0.64));
        ridge.append(QPointF(1.0, 0.55));
        ridge.append(QPointF(1.0, 1.0));
        ridge.append(QPointF(0.0, 1.0));
        const QColor ridgeColor = darkerWarm(lut.skyMid, 0.55);
        ops.append(makeFill(QStringLiteral("bg_ridge"),
                            QStringLiteral("Background"),
                            ridge,
                            ridgeColor,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));
        // D4-3: distant haze band — aerial perspective between sky and ridge.
        QPolygonF haze;
        haze.append(QPointF(0.0, 0.62));
        haze.append(QPointF(0.30, 0.55));
        haze.append(QPointF(0.62, 0.60));
        haze.append(QPointF(1.0, 0.52));
        haze.append(QPointF(1.0, 0.66));
        haze.append(QPointF(0.0, 0.70));
        // V6 W2: aerial perspective band tinted from the LUT sky bottom.
        const QColor hazeColor(lut.skyBottom.red(), lut.skyBottom.green(), lut.skyBottom.blue(), 110);
        ops.append(makeFill(QStringLiteral("bg_haze"),
                            QStringLiteral("Background"),
                            haze,
                            hazeColor,
                            QStringLiteral("watercolor"),
                            0.45,
                            QStringLiteral("wash")));
    } else {
        // Character backdrop: soft floor shadow ellipse grounds the bust.
        ops.append(makeFill(QStringLiteral("bg_floor_shadow"),
                            QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.5, 0.94), 0.30, 0.045),
                            darkerWarm(QColor(160, 170, 190), 0.75),
                            QStringLiteral("watercolor"),
                            0.5,
                            QStringLiteral("wash")));
    }
    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::clothingForSpec(const KisAiSceneSpec &spec,
                                                                 const QPointF &headCenter,
                                                                 qreal headWidth,
                                                                 qreal headHeight,
                                                                 const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    const QPointF &hc = headCenter;
    const qreal hw = headWidth;
    const qreal hh = headHeight;
    QVector<KisAiStrokeOperation> ops;
    const QColor skin = spec.head.skinTone;
    const QString style = spec.clothing.style;
    const QColor mainCloth = spec.clothing.color;
    const QColor secCloth = spec.clothing.secondaryColor;
    const QColor accCloth = spec.clothing.accentColor;

    const qreal neckTopY = hc.y() + hh * 0.42;
    const qreal neckBotY = hc.y() + hh * 0.64;
    const qreal neckW = hw * 0.34;
    const qreal shoulderY = neckBotY + hh * 0.05;
    const qreal bustBottomY = shoulderY + hh * 0.65;
    const qreal shoulderW = hw * 1.55;

    // 1. Anatomical Neck Base
    QPolygonF neck;
    neck.append(QPointF(hc.x() - neckW * 0.48, neckTopY));
    neck.append(QPointF(hc.x() + neckW * 0.48, neckTopY));
    neck.append(QPointF(hc.x() + neckW * 0.55, neckBotY));
    neck.append(QPointF(hc.x() - neckW * 0.55, neckBotY));
    ops.append(makeFill(QStringLiteral("neck"),
                        QStringLiteral("Flats"),
                        neck,
                        darkerWarm(skin, 0.95),
                        QStringLiteral("brush"),
                        1.0,
                        QStringLiteral("contour")));

    // Neck cast shadow (under chin shadow): smooth parabolic curve following jaw
    QPolygonF neckShadow;
    const QPointF chinLeft(hc.x() - neckW * 0.48, neckTopY);
    const QPointF chinRight(hc.x() + neckW * 0.48, neckTopY);
    sampleCubicBezier(chinLeft,
                      QPointF(hc.x() - neckW * 0.25, neckTopY + hh * 0.015),
                      QPointF(hc.x() + neckW * 0.25, neckTopY + hh * 0.015),
                      chinRight, 8, &neckShadow);
    sampleCubicBezier(chinRight,
                      QPointF(hc.x() + neckW * 0.32, neckTopY + hh * 0.13),
                      QPointF(hc.x() - neckW * 0.32, neckTopY + hh * 0.13),
                      chinLeft, 12, &neckShadow);
    ops.append(makeFill(QStringLiteral("neck_shadow"),
                        QStringLiteral("Shading"),
                        neckShadow,
                        darkerWarm(skin, 0.82),
                        QStringLiteral("watercolor"),
                        0.65,
                        QStringLiteral("wash")));

    // Sternocleidomastoid lines (首筋の繊細なライン)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> scm;
        scm.append(KisAiStrokePoint(hc.x() + side * neckW * 0.40, neckTopY + hh * 0.05, 0.4));
        scm.append(KisAiStrokePoint(hc.x() + side * neckW * 0.15, neckBotY - hh * 0.02, 0.6));
        ops.append(makePath(side < 0 ? QStringLiteral("neck_scm_l") : QStringLiteral("neck_scm_r"),
                            QStringLiteral("Lineart"),
                            scm,
                            darkerWarm(skin, 0.70),
                            QStringLiteral("gpen"),
                            0.0025,
                            0.60));
    }

    // 2. Clavicle lines (鎖骨)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> clavicle;
        clavicle.append(KisAiStrokePoint(hc.x() + side * neckW * 0.12, neckBotY, 0.7));
        clavicle.append(KisAiStrokePoint(hc.x() + side * neckW * 0.75, neckBotY + hh * 0.02, 0.8));
        clavicle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.45, neckBotY + hh * 0.06, 0.4));
        ops.append(makePath(side < 0 ? QStringLiteral("clavicle_l") : QStringLiteral("clavicle_r"),
                            QStringLiteral("Lineart"),
                            clavicle,
                            darkerWarm(skin, 0.68),
                            QStringLiteral("gpen"),
                            0.0028,
                            0.70));
    }

    // 3. Clothing Body Mass (natural sloping shoulders and chest contours)
    QPolygonF torso;
    torso.append(QPointF(hc.x() - neckW * 0.55, neckBotY));
    torso.append(QPointF(hc.x() + neckW * 0.55, neckBotY));
    torso.append(QPointF(hc.x() + shoulderW * 0.55, shoulderY + hh * 0.10));
    torso.append(QPointF(hc.x() + shoulderW * 0.62, bustBottomY));
    torso.append(QPointF(hc.x() - shoulderW * 0.62, bustBottomY));
    torso.append(QPointF(hc.x() - shoulderW * 0.55, shoulderY + hh * 0.10));
    ops.append(makeFill(QStringLiteral("clothing"),
                        QStringLiteral("Flats"),
                        torso,
                        mainCloth,
                        QStringLiteral("brush"),
                        1.0,
                        QStringLiteral("contour")));

    // V7: Procedural Drapery Folds (Tension folds; route away from open chest on dress)
    const QColor clothShadow = darkerWarm(mainCloth, 0.72);
    if (style != QLatin1String("dress")) {
        const QPointF leftShoulder(hc.x() - shoulderW * 0.40, shoulderY + hh * 0.15);
        const QPointF rightShoulder(hc.x() + shoulderW * 0.40, shoulderY + hh * 0.15);
        const QPointF chestCenter(hc.x(), shoulderY + hh * 0.35);
        ops.append(
            KisAiRigLibrary::draperyFoldOps(leftShoulder, chestCenter, 2.0, mainCloth, clothShadow, QStringLiteral("l")));
        ops.append(
            KisAiRigLibrary::draperyFoldOps(rightShoulder, chestCenter, 2.0, mainCloth, clothShadow, QStringLiteral("r")));
    } else {
        const QPointF leftShoulder(hc.x() - shoulderW * 0.42, shoulderY + hh * 0.16);
        const QPointF leftBustSide(hc.x() - shoulderW * 0.25, shoulderY + hh * 0.38);
        const QPointF rightShoulder(hc.x() + shoulderW * 0.42, shoulderY + hh * 0.16);
        const QPointF rightBustSide(hc.x() + shoulderW * 0.25, shoulderY + hh * 0.38);
        ops.append(
            KisAiRigLibrary::draperyFoldOps(leftShoulder, leftBustSide, 1.5, mainCloth, clothShadow, QStringLiteral("dress_l")));
        ops.append(
            KisAiRigLibrary::draperyFoldOps(rightShoulder, rightBustSide, 1.5, mainCloth, clothShadow, QStringLiteral("dress_r")));
    }

    // 4. Style-Specific Costume Details
    if (style == QLatin1String("school_uniform") || style == QLatin1String("sailor")) {
        // Sailor Collar: V-neck triangular flap
        QPolygonF collar;
        collar.append(QPointF(hc.x() - neckW * 0.70, neckBotY - hh * 0.02));
        collar.append(QPointF(hc.x() + neckW * 0.70, neckBotY - hh * 0.02));
        collar.append(QPointF(hc.x() + neckW * 0.50, shoulderY + hh * 0.22));
        collar.append(QPointF(hc.x(), shoulderY + hh * 0.28));
        collar.append(QPointF(hc.x() - neckW * 0.50, shoulderY + hh * 0.22));
        ops.append(makeFill(QStringLiteral("cloth_sailor_collar"),
                            QStringLiteral("Flats"),
                            collar,
                            secCloth,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));

        // Collar stripe lineart
        QVector<KisAiStrokePoint> stripe;
        stripe.append(KisAiStrokePoint(hc.x() - neckW * 0.64, neckBotY + hh * 0.02, 0.8));
        stripe.append(KisAiStrokePoint(hc.x() - neckW * 0.44, shoulderY + hh * 0.20, 0.8));
        stripe.append(KisAiStrokePoint(hc.x(), shoulderY + hh * 0.25, 0.9));
        stripe.append(KisAiStrokePoint(hc.x() + neckW * 0.44, shoulderY + hh * 0.20, 0.8));
        stripe.append(KisAiStrokePoint(hc.x() + neckW * 0.64, neckBotY + hh * 0.02, 0.8));
        ops.append(makePath(QStringLiteral("cloth_collar_stripe"),
                            QStringLiteral("Lineart"),
                            stripe,
                            mainCloth.darker(130),
                            QStringLiteral("gpen"),
                            0.0035,
                            0.95));

        // Chest Ribbon / Scarf
        const qreal knotY = shoulderY + hh * 0.22;
        QPolygonF knot;
        knot.append(QPointF(hc.x() - 0.022, knotY - 0.015));
        knot.append(QPointF(hc.x() + 0.022, knotY - 0.015));
        knot.append(QPointF(hc.x() + 0.018, knotY + 0.018));
        knot.append(QPointF(hc.x() - 0.018, knotY + 0.018));
        ops.append(makeFill(QStringLiteral("cloth_ribbon_knot"),
                            QStringLiteral("Flats"),
                            knot,
                            accCloth.darker(115),
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));

        // Ribbon wings (left and right)
        for (int side = -1; side <= 1; side += 2) {
            QPolygonF wing;
            wing.append(QPointF(hc.x() + side * 0.015, knotY - 0.005));
            wing.append(QPointF(hc.x() + side * 0.08, knotY + 0.02));
            wing.append(QPointF(hc.x() + side * 0.065, knotY + 0.09));
            wing.append(QPointF(hc.x() + side * 0.01, knotY + 0.03));
            ops.append(makeFill(side < 0 ? QStringLiteral("ribbon_wing_l") : QStringLiteral("ribbon_wing_r"),
                                QStringLiteral("Flats"),
                                wing,
                                accCloth,
                                QStringLiteral("brush"),
                                1.0,
                                QStringLiteral("contour")));
        }

    } else if (style == QLatin1String("hoodie")) {
        // Hood folds around neck
        for (int side = -1; side <= 1; side += 2) {
            QPolygonF hoodFold;
            hoodFold.append(QPointF(hc.x() + side * neckW * 0.40, neckBotY - hh * 0.05));
            hoodFold.append(QPointF(hc.x() + side * shoulderW * 0.35, neckBotY + hh * 0.06));
            hoodFold.append(QPointF(hc.x() + side * shoulderW * 0.28, shoulderY + hh * 0.18));
            hoodFold.append(QPointF(hc.x() + side * neckW * 0.20, shoulderY + hh * 0.15));
            ops.append(makeFill(side < 0 ? QStringLiteral("hood_fold_l") : QStringLiteral("hood_fold_r"),
                                QStringLiteral("Flats"),
                                hoodFold,
                                mainCloth.lighter(115),
                                QStringLiteral("brush"),
                                1.0,
                                QStringLiteral("contour")));
        }

        // Drawstrings (フードの紐)
        for (int side = -1; side <= 1; side += 2) {
            QVector<KisAiStrokePoint> string;
            const qreal sx = hc.x() + side * 0.045;
            string.append(KisAiStrokePoint(sx, shoulderY + hh * 0.14, 0.7));
            string.append(KisAiStrokePoint(sx + side * 0.01, shoulderY + hh * 0.30, 0.8));
            string.append(KisAiStrokePoint(sx, shoulderY + hh * 0.44, 0.6));
            ops.append(makePath(side < 0 ? QStringLiteral("hood_string_l") : QStringLiteral("hood_string_r"),
                                QStringLiteral("Lineart"),
                                string,
                                secCloth,
                                QStringLiteral("gpen"),
                                0.003,
                                0.95));
        }

    } else if (style == QLatin1String("dress")) {
        // Sweetheart / Square neckline with collar trim
        QPolygonF chestSkin;
        chestSkin.append(QPointF(hc.x() - neckW * 0.55, neckBotY));
        chestSkin.append(QPointF(hc.x() + neckW * 0.55, neckBotY));
        chestSkin.append(QPointF(hc.x() + neckW * 0.45, shoulderY + hh * 0.15));
        chestSkin.append(QPointF(hc.x(), shoulderY + hh * 0.20));
        chestSkin.append(QPointF(hc.x() - neckW * 0.45, shoulderY + hh * 0.15));
        ops.append(makeFill(QStringLiteral("dress_decollete"),
                            QStringLiteral("Flats"),
                            chestSkin,
                            skin,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));

        // Neckline trim / frill
        QVector<KisAiStrokePoint> trim;
        trim.append(KisAiStrokePoint(hc.x() - neckW * 0.52, shoulderY + hh * 0.14, 0.8));
        trim.append(KisAiStrokePoint(hc.x() - neckW * 0.25, shoulderY + hh * 0.18, 0.9));
        trim.append(KisAiStrokePoint(hc.x(), shoulderY + hh * 0.21, 0.8));
        trim.append(KisAiStrokePoint(hc.x() + neckW * 0.25, shoulderY + hh * 0.18, 0.9));
        trim.append(KisAiStrokePoint(hc.x() + neckW * 0.52, shoulderY + hh * 0.14, 0.8));
        ops.append(makePath(QStringLiteral("dress_trim"),
                            QStringLiteral("Lineart"),
                            trim,
                            secCloth,
                            QStringLiteral("gpen"),
                            0.0035,
                            0.95));

    } else {
        // Casual Crewneck / T-Shirt
        QPolygonF crewNeck;
        crewNeck.append(QPointF(hc.x() - neckW * 0.50, neckBotY));
        crewNeck.append(QPointF(hc.x() + neckW * 0.50, neckBotY));
        crewNeck.append(QPointF(hc.x() + neckW * 0.38, neckBotY + hh * 0.10));
        crewNeck.append(QPointF(hc.x(), neckBotY + hh * 0.14));
        crewNeck.append(QPointF(hc.x() - neckW * 0.38, neckBotY + hh * 0.10));
        ops.append(makeFill(QStringLiteral("casual_rib_collar"),
                            QStringLiteral("Flats"),
                            crewNeck,
                            secCloth,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));
    }

    // Fabric wrinkle shadow lines (胸元と脇の布シワ)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> wrinkle;
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.50, shoulderY + hh * 0.25, 0.3));
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.30, shoulderY + hh * 0.38, 0.7));
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.18, shoulderY + hh * 0.44, 0.2));
        ops.append(makePath(side < 0 ? QStringLiteral("wrinkle_l") : QStringLiteral("wrinkle_r"),
                            QStringLiteral("Lineart"),
                            wrinkle,
                            mainCloth.darker(140),
                            QStringLiteral("gpen"),
                            0.0028,
                            0.75));
    }

    // Anatomic clavicle (鎖骨) lineart - delicate natural lines across upper chest
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> clavicle;
        clavicle.append(KisAiStrokePoint(hc.x() + side * neckW * 0.18, shoulderY + hh * 0.08, 0.20));
        clavicle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.22, shoulderY + hh * 0.07, 0.65));
        clavicle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.42, shoulderY + hh * 0.09, 0.25));
        ops.append(makePath(side < 0 ? QStringLiteral("clavicle_l") : QStringLiteral("clavicle_r"),
                            QStringLiteral("Lineart"),
                            clavicle,
                            darkerWarm(skin, 0.70),
                            QStringLiteral("fineliner"),
                            0.0018,
                            0.65));
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::characterProgram(const KisAiSceneSpec &spec, const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> ops;
    const QPointF hc = spec.composition.headCenter;
    const qreal hh = spec.composition.headHeight;
    const qreal hw = hh * 0.78;
    const QColor skin = spec.head.skinTone;

    // Hair back mass first (behind everything in Flats order).
    ops.append(hairBackMassForStyle(spec, hc, hw, hh));

    // Anatomical neck + shoulders + clothing
    ops.append(clothingForSpec(spec, hc, hw, hh, canvasSize));

    // Face skin over hair back mass.
    ops.append(makeFill(QStringLiteral("face_skin"),
                        QStringLiteral("Flats"),
                        headOutlinePolygon(hc, hw, hh),
                        skin,
                        QStringLiteral("brush"),
                        1.0,
                        QStringLiteral("contour")));

    // Ears.
    for (int side = -1; side <= 1; side += 2) {
        ops.append(
            makeFill(side < 0 ? QStringLiteral("ear_l") : QStringLiteral("ear_r"),
                     QStringLiteral("Flats"),
                     ellipsePolygon(QPointF(hc.x() + side * hw * 0.50, hc.y() + hh * 0.10), hw * 0.05, hh * 0.07),
                     skin,
                     QStringLiteral("brush"),
                     1.0,
                     QStringLiteral("wash")));
    }

    // V6 W1: Rig-driven face. One parameter set drives eyes, lids, brows,
    // nose, mouth and hair highlight bands; LLM rig values finally reach ink.
    KisAiRigParameterSet rigParams = KisAiRigLibrary::parametersFromSpec(spec);
    KisAiRigLibrary::applyCameraAdjust(rigParams, spec.camera);
    KisAiRigLibrary::applyDetailBudget(rigParams, spec.style.detailLevel);
    rigParams = KisAiRigLibrary::clamped(rigParams);
    const auto eyePlaces = KisAiRigLibrary::eyePlacements(rigParams);
    ops.append(KisAiRigLibrary::eyePairOps(rigParams));
    ops.append(KisAiRigLibrary::doubleLidOps(rigParams));
    ops.append(KisAiRigLibrary::browOps(rigParams));
    ops.append(KisAiRigLibrary::noseOps(rigParams));
    ops.append(KisAiRigLibrary::mouthOps(rigParams));
    ops.append(KisAiRigLibrary::hairHighlightOps(rigParams));

    // Accessory ornaments share Rig geometry; gated by the detail budget.
    // V6 W2: lid shadows derive from the rig so night/sunset shift their tone.
    const KisAiLightSettings ornamentRig = KisAiLightRig::fromSpec(spec);
    if (decorationPolicyForDetail(spec.style.detailLevel)) {
        const KisAiEyePlacement sides[2] = {eyePlaces.first, eyePlaces.second};
        for (int i = 0; i < 2; ++i) {
            const QPointF ec = sides[i].center;
            const qreal ew = sides[i].size.width();
            const qreal eh = sides[i].size.height();
            QPolygonF eyelidShadow;
            eyelidShadow.append(QPointF(ec.x() - ew * 0.45, ec.y() - eh * 0.10));
            eyelidShadow.append(QPointF(ec.x() + ew * 0.45, ec.y() - eh * 0.10));
            eyelidShadow.append(QPointF(ec.x() + ew * 0.35, ec.y() + eh * 0.12));
            eyelidShadow.append(QPointF(ec.x() - ew * 0.35, ec.y() + eh * 0.12));
            ops.append(makeFill(i == 0 ? QStringLiteral("eyelid_shade_l") : QStringLiteral("eyelid_shade_r"),
                                QStringLiteral("Shading"),
                                eyelidShadow,
                                KisAiLightRig::shadowColor(skin, ornamentRig),
                                QStringLiteral("watercolor"),
                                0.40,
                                QStringLiteral("wash")));

            QVector<KisAiStrokePoint> tearTrough;
            tearTrough.append(KisAiStrokePoint(ec.x() - ew * 0.25, ec.y() + eh * 0.52, 0.2));
            tearTrough.append(KisAiStrokePoint(ec.x(), ec.y() + eh * 0.54, 0.45));
            tearTrough.append(KisAiStrokePoint(ec.x() + ew * 0.25, ec.y() + eh * 0.52, 0.2));
            ops.append(makePath(i == 0 ? QStringLiteral("tear_trough_l") : QStringLiteral("tear_trough_r"),
                                QStringLiteral("Highlights"),
                                tearTrough,
                                QColor(255, 242, 245),
                                QStringLiteral("airbrush"),
                                0.0022,
                                0.40));
        }
    }

    // Cheek blush (soft natural bloom)
    for (int side = -1; side <= 1; side += 2) {
        ops.append(
            makeFill(side < 0 ? QStringLiteral("blush_l") : QStringLiteral("blush_r"),
                     QStringLiteral("Shading"),
                     ellipsePolygon(QPointF(hc.x() + side * hw * 0.30, hc.y() + hh * 0.26), hw * 0.09, hh * 0.05),
                     QColor(255, 159, 178),
                     QStringLiteral("watercolor"),
                     0.35,
                     QStringLiteral("wash")));
    }

    // V6 W2: skin subsurface scattering tinted from the LUT sssTint.
    // Suppressed at night, amplified at sunset via the LightRig LUT.
    {
        const KisAiLightRig::TimeOfDayLut sssLut = KisAiLightRig::timeOfDayLut(spec.light.timeOfDay);
        const qreal sssGain = spec.light.timeOfDay == QLatin1String("night") ? 0.55
            : spec.light.timeOfDay == QLatin1String("sunset")                ? 1.35
                                                                             : 1.0;
        const qreal sssAlpha = qBound<qreal>(0.06, 0.16 * sssGain, 0.24);
        const QColor sssNose = sssLut.sssTint;
        ops.append(makeFill(QStringLiteral("sss_nose_tip"),
                            QStringLiteral("Shading"),
                            ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.235), hw * 0.022, hh * 0.016),
                            sssNose,
                            QStringLiteral("watercolor"),
                            sssAlpha,
                            QStringLiteral("wash")));
        for (int side = -1; side <= 1; side += 2) {
            ops.append(
                makeFill(side < 0 ? QStringLiteral("sss_cheek_l") : QStringLiteral("sss_cheek_r"),
                         QStringLiteral("Shading"),
                         ellipsePolygon(QPointF(hc.x() + side * hw * 0.30, hc.y() + hh * 0.26), hw * 0.055, hh * 0.030),
                         sssNose,
                         QStringLiteral("watercolor"),
                         sssAlpha * 0.8,
                         QStringLiteral("wash")));
            ops.append(
                makeFill(side < 0 ? QStringLiteral("sss_ear_l") : QStringLiteral("sss_ear_r"),
                         QStringLiteral("Shading"),
                         ellipsePolygon(QPointF(hc.x() + side * hw * 0.50, hc.y() + hh * 0.10), hw * 0.030, hh * 0.045),
                         sssNose,
                         QStringLiteral("watercolor"),
                         sssAlpha,
                         QStringLiteral("wash")));
        }
    }

    // Face contour lineart: crisp jaw arc reusing the rig outline.
    {
        const QPolygonF outline = headOutlinePolygon(hc, hw, hh);
        QVector<KisAiStrokePoint> jaw;
        for (int i = 0; i < outline.size(); ++i) {
            const QPointF &pt = outline.at(i);
            if (pt.y() > hc.y() - hh * 0.05) // jaw half only; crown hides under fringe
                jaw.append(KisAiStrokePoint(pt.x(), pt.y(), 0.85));
        }
        if (jaw.size() >= 2) {
            KisAiStrokeOperation jawOp = makePath(QStringLiteral("face_contour"),
                                                  QStringLiteral("Lineart"),
                                                  jaw,
                                                  QColor(28, 24, 40),
                                                  QStringLiteral("gpen"),
                                                  0.005,
                                                  1.0);
            jawOp.groupId = QStringLiteral("jaw");
            jawOp.role = QStringLiteral("contour");
            ops.append(jawOp);
        }
    }

    // Hair front mass (fringe, side locks, ahoge, halo) over face skin and contour
    ops.append(hairFrontMassForStyle(spec, hc, hw, hh));

    // V7: Hierarchical 3D hair clumps with ribbon flows, tapered tips & cast shadows
    ops.append(KisAiRigLibrary::hierarchicalHairClumpOps(rigParams, canvasSize, 42));

    // V6 W1: character weather/props share the landscape BackdropRig.
    // The face-box guard inside backdropWeatherOps keeps skies off faces.
    ops.append(KisAiRigLibrary::backdropWeatherOps(rigParams, canvasSize, 42));

    // Rig-driven shading: core + form + bounce + rim + chin AO + hair band.
    // V6 W2: 4-layer completion — form softness and floor bounce join core.
    const KisAiLightSettings rig = KisAiLightRig::fromSpec(spec);
    QVector<KisAiStrokeOperation> flatsOnly;
    for (const KisAiStrokeOperation &op : ops) {
        if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats")
            && (op.kind == KisAiStrokeOperation::Kind::Fill || op.kind == KisAiStrokeOperation::Kind::GradientFill))
            flatsOnly.append(op);
    }
    KisAiLightRig::HeadAnchor anchor;
    anchor.headCenter = hc;
    anchor.headHeight = hh;
    anchor.headWidth = hw;
    ops.append(KisAiLightRig::synthesizeShading(flatsOnly, rig, canvasSize, &anchor));
    ops.append(KisAiLightRig::synthesizeFormShading(flatsOnly, rig, canvasSize));
    ops.append(KisAiLightRig::synthesizeBounceLight(flatsOnly, rig, canvasSize));
    ops.append(KisAiLightRig::synthesizeVolumetricShading(flatsOnly, rig, canvasSize, &anchor));
    ops.append(KisAiLightRig::synthesizeMaterialOptics(flatsOnly, rig, canvasSize, &anchor));

    // V10: Curvature-following jagged angel halo highlight for hair
    ops.append(KisAiStrokeQualityUtils::generateJaggedHairHalo(
        hc, hw * 2.0, hh, spec.head.hairColor, canvasSize, -0.10,
        spec.rig.hairHighlightBands > 0 ? spec.rig.hairHighlightBands : 1, 42));

    // V10: Floating angel halo torus above crown when prompt requests angel or halo
    const QString lowerPrompt = spec.prompt.toLower();
    const bool hasFloatingHalo = lowerPrompt.contains(QStringLiteral("angel"))
        || lowerPrompt.contains(QStringLiteral("halo"))
        || lowerPrompt.contains(QStringLiteral("天使"))
        || lowerPrompt.contains(QStringLiteral("輪"));
    if (hasFloatingHalo) {
        const QColor haloCol = spec.clothing.accentColor.isValid() && spec.clothing.accentColor.alpha() > 0
            ? spec.clothing.accentColor
            : QColor(255, 225, 120);
        ops.append(KisAiStrokeQualityUtils::generateFloatingAngelHalo(hc, hw, hh, haloCol, canvasSize));
    }

    // Opt-in lineart hierarchy (outer contours heavier than details).
    KisAiStrokeQualityUtils::applyLineartHierarchy(ops);

    // V10: Light direction and occlusion-aware line weight modulation
    KisAiStrokeQualityUtils::applyOcclusionAndLightingLineWeight(ops, rig.direction);

    // V10: Automatic corner inking fillets at acute junctions
    const auto fillets = KisAiStrokeQualityUtils::applyCornerInkingFillets(ops, canvasSize);
    ops.append(fillets);

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::landscapeProgram(const KisAiSceneSpec &spec, const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> ops = backgroundForSpec(spec, canvasSize);
    const KisAiLightSettings rig = KisAiLightRig::fromSpec(spec);

    const QString lowerPrompt = spec.prompt.toLower();
    const bool hasMountain = lowerPrompt.contains(QStringLiteral("mountain"))
        || lowerPrompt.contains(QStringLiteral("fuji")) || lowerPrompt.contains(QStringLiteral("山"))
        || lowerPrompt.contains(QStringLiteral("peak")) || lowerPrompt.contains(QStringLiteral("landscape"))
        || lowerPrompt.contains(QStringLiteral("scenery"));

    const bool hasSakura = lowerPrompt.contains(QStringLiteral("sakura"))
        || lowerPrompt.contains(QStringLiteral("cherry")) || lowerPrompt.contains(QStringLiteral("桜"))
        || lowerPrompt.contains(QStringLiteral("tree")) || lowerPrompt.contains(QStringLiteral("blossom"))
        || lowerPrompt.contains(QStringLiteral("花"));

    const bool hasWater = lowerPrompt.contains(QStringLiteral("water")) || lowerPrompt.contains(QStringLiteral("lake"))
        || lowerPrompt.contains(QStringLiteral("sea")) || lowerPrompt.contains(QStringLiteral("ocean"))
        || lowerPrompt.contains(QStringLiteral("river")) || lowerPrompt.contains(QStringLiteral("湖"))
        || lowerPrompt.contains(QStringLiteral("水")) || lowerPrompt.contains(QStringLiteral("海"))
        || lowerPrompt.contains(QStringLiteral("川")) || lowerPrompt.contains(QStringLiteral("風景"))
        || !lowerPrompt.contains(QStringLiteral("desert"));

    constexpr qreal horizonY = 0.62;

    // 1. Far Distance: Majestic Mountain (e.g. Mount Fuji)
    if (hasMountain) {
        ops.append(KisAiRigLibrary::mountainOps(spec, canvasSize, 42));
    }

    // 2. Midground / Foreground Ground: Water Surface or Meadow
    if (hasWater) {
        ops.append(KisAiRigLibrary::waterSurfaceOps(spec, canvasSize, horizonY, 42));
    } else {
        QPolygonF meadow;
        meadow.append(QPointF(0.0, 0.80));
        meadow.append(QPointF(0.25, 0.74));
        meadow.append(QPointF(0.55, 0.82));
        meadow.append(QPointF(0.80, 0.76));
        meadow.append(QPointF(1.0, 0.84));
        meadow.append(QPointF(1.0, 1.0));
        meadow.append(QPointF(0.0, 1.0));
        // V6 W2: meadow grass tinted from the LUT sky mid for time-of-day unity.
        const KisAiLightRig::TimeOfDayLut meadowLut = KisAiLightRig::timeOfDayLut(spec.light.timeOfDay);
        const QColor meadowColor = spec.light.timeOfDay == QLatin1String("night")
            ? darkerWarm(meadowLut.skyMid, 0.60)
            : mixToward(QColor(96, 140, 110), meadowLut.skyMid, 0.20);
        ops.append(makeFill(QStringLiteral("meadow"),
                            QStringLiteral("Flats"),
                            meadow,
                            meadowColor,
                            QStringLiteral("brush"),
                            1.0,
                            QStringLiteral("contour")));
    }

    // 3. Middleground Hero Feature: Sakura Tree with branching and blooming clusters
    if (hasSakura) {
        ops.append(KisAiRigLibrary::sakuraTreeOps(spec, canvasSize, 42));
    }

    // 4. Backdrop weather / atmospheric props (clouds, stars, etc.)
    const KisAiRigParameterSet rigParams = KisAiRigLibrary::parametersFromSpec(spec);
    ops.append(KisAiRigLibrary::backdropWeatherOps(rigParams, canvasSize, 42));

    QVector<KisAiStrokeOperation> flatsOnly;
    for (const KisAiStrokeOperation &op : ops) {
        if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats"))
            flatsOnly.append(op);
    }
    ops.append(KisAiLightRig::synthesizeShading(flatsOnly, rig, canvasSize, nullptr));
    // V6 W2: 4-layer completion for landscapes too (core + form + bounce).
    ops.append(KisAiLightRig::synthesizeFormShading(flatsOnly, rig, canvasSize));
    ops.append(KisAiLightRig::synthesizeBounceLight(flatsOnly, rig, canvasSize));
    KisAiStrokeQualityUtils::applyLineartHierarchy(ops);
    return ops;
}

KisAiStrokeProgram KisAiLayoutEngine::generateProgram(const KisAiSceneSpec &spec, const QSize &canvasSize)
{
    // V6 W1: resolve narrative.time + colorScript once; every rig below
    // shares the resolved spec while prompt/title stay verbatim.
    const KisAiSceneSpec resolved = resolveSpecForLayout(spec);
    KisAiStrokeProgram program;
    program.schemaVersion = 2;
    program.prompt = spec.prompt;
    program.canvasSize = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);
    program.title = QStringLiteral("SceneSpec Composition");

    QVector<KisAiStrokeOperation> ops;
    if (resolved.subject.type == QLatin1String("landscape")) {
        ops = landscapeProgram(resolved, program.canvasSize); // includes background
    } else {
        ops = backgroundForSpec(resolved, program.canvasSize);
        ops.append(characterProgram(resolved, program.canvasSize));
    }

    // V7: Apply Art Style Pipeline before final packaging
    applyArtStylePipeline(ops, resolved.style);

    program.operations = ops;
    KisAiStrokeQualityReport report;
    KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);
    refined.prompt = spec.prompt;
    refined.title = program.title;
    // V6 W4: brush preset hints (KisPainter path groundwork) at the single exit.
    KisAiStrokeQualityUtils::assignBrushPresetHints(refined);
    return refined;
}

void KisAiLayoutEngine::applyArtStylePipeline(QVector<KisAiStrokeOperation> &operations, const KisAiSceneStyleV2 &style)
{
    const QString art = style.artStyleId.toLower();

    if (art == QLatin1String("watercolor")) {
        // Watercolor Style: soft washes, wet edges, translucent pencil lineart
        for (KisAiStrokeOperation &op : operations) {
            const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
            if (op.kind == KisAiStrokeOperation::Kind::Fill) {
                op.brush.profile = QStringLiteral("watercolor");
                op.fillStyle = QStringLiteral("wash");
                if (lName == QLatin1String("Flats")) {
                    op.brush.opacity = qBound<qreal>(0.75, op.brush.opacity * 0.90, 0.95);
                }
            } else if (op.kind == KisAiStrokeOperation::Kind::Path && lName == QLatin1String("Lineart")) {
                op.brush.profile = QStringLiteral("pencil");
                op.brush.opacity = qBound<qreal>(0.55, op.brush.opacity * 0.78, 0.85);
            }
        }
    } else if (art == QLatin1String("impasto") || art == QLatin1String("painterly")) {
        // Impasto / Painterly Style: visible bristle brushwork, thicker energetic lines
        for (KisAiStrokeOperation &op : operations) {
            const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
            if (op.kind == KisAiStrokeOperation::Kind::Fill) {
                op.brush.profile = QStringLiteral("brush");
                op.brush.opacity = 1.0;
            } else if (op.kind == KisAiStrokeOperation::Kind::Path && lName == QLatin1String("Lineart")) {
                op.brush.size *= 1.25;
                op.brush.profile = QStringLiteral("brush");
            }
        }
    } else if (art == QLatin1String("ink_sketch") || art == QLatin1String("ink_manga")) {
        // Ink Manga / Sketch Style: high-contrast dark ink lines, crisp cross-hatching
        for (KisAiStrokeOperation &op : operations) {
            const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
            if (op.kind == KisAiStrokeOperation::Kind::Path && lName == QLatin1String("Lineart")) {
                op.brush.profile = QStringLiteral("gpen");
                op.brush.opacity = 1.0;
                op.brush.color = QColor(18, 18, 24);
            }
        }
    } else if (art == QLatin1String("cyber_neon")) {
        // Cyber Neon: luminous neon tubes, screen-blend highlights
        for (KisAiStrokeOperation &op : operations) {
            const QString lName = KisAiStrokeProgramCodec::normalizeLayerName(op.layer);
            if (lName == QLatin1String("Highlights") || lName == QLatin1String("FX")) {
                op.brush.profile = QStringLiteral("neon");
                op.blendMode = QStringLiteral("screen");
                op.brush.opacity = qMin<qreal>(1.0, op.brush.opacity * 1.35);
            }
        }
    }
}
