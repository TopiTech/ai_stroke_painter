/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiLayoutEngine.h"
#include "KisAiLightRig.h"
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
    return QColor::fromHsv((c.hue() + 360) % 360,
                           qBound(0, int(c.saturation() * 1.05), 255),
                           qBound(0, int(c.value() * factor), 255), c.alpha());
}
} // namespace

KisAiStrokeOperation KisAiLayoutEngine::makeFill(
    const QString &id, const QString &layer,
    const QPolygonF &polygon, const QColor &color,
    const QString &profile, qreal opacity, const QString &style)
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

KisAiStrokeOperation KisAiLayoutEngine::makePath(
    const QString &id, const QString &layer,
    const QVector<KisAiStrokePoint> &points,
    const QColor &color, const QString &profile, qreal size, qreal opacity)
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
    // Canonical anime head contour:
    // - Smooth cranial dome
    // - Plump youthful cheek fullness around eye level
    // - Graceful jaw curve tapering to a delicate, softly rounded chin
    // Strictly symmetric around center.x().
    QPolygonF poly;
    const int segments = 48; // Multiple of 4 guarantees symmetry
    poly.reserve(segments);

    for (int i = 0; i < segments; ++i) {
        const qreal t = 2.0 * M_PI * i / segments;
        const qreal c = std::cos(t);
        const qreal s = std::sin(t); // -1 = crown, 0 = eyes/cheeks, +1 = chin tip

        qreal hw = width * 0.50;
        qreal y = center.y();

        if (s <= 0.0) {
            // Crown hemisphere (smooth dome)
            y += s * (height * 0.46);
        } else {
            // Lower face
            y += s * (height * 0.44);

            if (s < 0.35) {
                // Cheeks stay wide and plump
                hw *= (1.0 + 0.03 * std::sin(s / 0.35 * M_PI));
            } else {
                // Jaw taper: smooth transition to a soft, rounded chin (not a needle)
                const qreal jawT = (s - 0.35) / 0.65;
                const qreal taper = 1.0 - 0.35 * jawT + 0.06 * (jawT * jawT);
                hw *= taper;
            }
        }

        const qreal x = center.x() + c * hw;
        poly.append(QPointF(x, y));
    }
    return poly;
}

QPair<QPointF, QPointF> KisAiLayoutEngine::eyePairCenters(
    const QPointF &headCenter, qreal headWidth, qreal headHeight, const QString &facing)
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
    return qMakePair(QPointF(headCenter.x() - spread + shift, eyeY),
                     QPointF(headCenter.x() + spread + shift, eyeY));
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairBackMassForStyle(
    const KisAiSceneSpec &spec,
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

    // 1. Back mass (inner shade behind head)
    {
        QPolygonF innerBack;
        const int topSteps = 10;
        for (int i = 0; i <= topSteps; ++i) {
            const qreal t = M_PI * (1.0 - (qreal)i / topSteps);
            const qreal bx = headCenter.x() + std::cos(t) * (headWidth * 0.62);
            const qreal by = headCenter.y() - std::sin(t) * (headHeight * 0.58);
            innerBack.append(QPointF(bx, by));
        }
        innerBack.append(QPointF(headCenter.x() + headWidth * 0.65, chinY + backLength * 0.60));
        innerBack.append(QPointF(headCenter.x() + headWidth * 0.35, chinY + backLength * 0.85));
        innerBack.append(QPointF(headCenter.x(), chinY + backLength * 0.72));
        innerBack.append(QPointF(headCenter.x() - headWidth * 0.35, chinY + backLength * 0.85));
        innerBack.append(QPointF(headCenter.x() - headWidth * 0.65, chinY + backLength * 0.60));
        ops.append(makeFill(QStringLiteral("hair_inner_shade"), QStringLiteral("Flats"),
                            innerBack, darkerWarm(hair, 0.72), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
    }

    // 2. Back mass (main volume behind the face)
    {
        QPolygonF back;
        // Smooth cranial top dome from left temple over crown to right temple
        const int topSteps = 12;
        for (int i = 0; i <= topSteps; ++i) {
            const qreal t = M_PI * (1.0 - (qreal)i / topSteps);
            const qreal bx = headCenter.x() + std::cos(t) * (headWidth * 0.66);
            const qreal by = headCenter.y() - std::sin(t) * (headHeight * 0.62);
            back.append(QPointF(bx, by));
        }

        if (style == QLatin1String("bob")) {
            back.append(QPointF(headCenter.x() + headWidth * 0.68, headCenter.y() + headHeight * 0.20));
            back.append(QPointF(headCenter.x() + headWidth * 0.50, chinY + backLength * 0.35));
            back.append(QPointF(headCenter.x(), chinY + backLength * 0.28));
            back.append(QPointF(headCenter.x() - headWidth * 0.50, chinY + backLength * 0.35));
            back.append(QPointF(headCenter.x() - headWidth * 0.68, headCenter.y() + headHeight * 0.20));
        } else if (style == QLatin1String("short_messy")) {
            back.append(QPointF(headCenter.x() + headWidth * 0.62, headCenter.y() + headHeight * 0.15));
            back.append(QPointF(headCenter.x() + headWidth * 0.45, chinY + backLength * 0.20));
            back.append(QPointF(headCenter.x() + headWidth * 0.20, chinY + backLength * 0.35));
            back.append(QPointF(headCenter.x(), chinY + backLength * 0.25));
            back.append(QPointF(headCenter.x() - headWidth * 0.20, chinY + backLength * 0.35));
            back.append(QPointF(headCenter.x() - headWidth * 0.45, chinY + backLength * 0.20));
            back.append(QPointF(headCenter.x() - headWidth * 0.62, headCenter.y() + headHeight * 0.15));
        } else {
            // long_hime / long_wavy default
            back.append(QPointF(headCenter.x() + headWidth * 0.66, headCenter.y() + headHeight * 0.25));
            back.append(QPointF(headCenter.x() + headWidth * 0.58, chinY + backLength * 0.55));
            back.append(QPointF(headCenter.x() + headWidth * 0.32, chinY + backLength * 0.78));
            back.append(QPointF(headCenter.x(), chinY + backLength * 0.65));
            back.append(QPointF(headCenter.x() - headWidth * 0.32, chinY + backLength * 0.78));
            back.append(QPointF(headCenter.x() - headWidth * 0.58, chinY + backLength * 0.55));
            back.append(QPointF(headCenter.x() - headWidth * 0.66, headCenter.y() + headHeight * 0.25));
        }
        ops.append(makeFill(QStringLiteral("hair_back_mass"), QStringLiteral("Flats"),
                            back, hair, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
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
                                QStringLiteral("Flats"), tail, darkerWarm(hair, 0.92),
                                QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

            // Hair ribbon band (tie)
            QPolygonF band;
            const qreal bx = xRoot + side * headWidth * 0.15;
            const qreal by = topY - headHeight * 0.08;
            band.append(QPointF(bx - 0.02, by - 0.02));
            band.append(QPointF(bx + 0.02, by - 0.02));
            band.append(QPointF(bx + 0.02, by + 0.02));
            band.append(QPointF(bx - 0.02, by + 0.02));
            ops.append(makeFill(side < 0 ? QStringLiteral("hair_band_l") : QStringLiteral("hair_band_r"),
                                QStringLiteral("Flats"), band, spec.palette.accents.isEmpty() ? QColor(230, 60, 80) : spec.palette.accents.first(),
                                QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
        }
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairFrontMassForStyle(
    const KisAiSceneSpec &spec,
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
            const struct SweptPt { qreal xR; qreal dipR; } sweptPts[] = {
                { sign * 0.52, 0.70 },
                { sign * 0.28, 0.64 },
                { sign * 0.05, 0.58 },
                { -sign * 0.20, 0.50 },
                { -sign * 0.48, 0.44 },
            };
            for (const auto &sp : sweptPts) {
                const qreal px = headCenter.x() + sp.xR * headWidth;
                const qreal py = topY + sp.dipR * headHeight;
                fringe.append(QPointF(px, py));
                fringeLine.append(KisAiStrokePoint(px, py, 0.8));
            }
        } else {
            // Classic Anime M-Fringe: 13 points forming 5 distinct, tapered clumps
            // Valleys cut up toward forehead, Tips taper down over eyes and eyebrows
            struct ClumpPoint { qreal xRatio; qreal dipRatio; };
            const ClumpPoint clumps[] = {
                {  0.54, 0.70 }, // Right temple lock tip
                {  0.46, 0.48 }, // Valley
                {  0.36, 0.60 }, // Right outer clump tip
                {  0.26, 0.45 }, // Valley
                {  0.16, 0.58 }, // Right center clump tip (touches right eye)
                {  0.07, 0.44 }, // Right-center valley
                {  0.00, 0.41 }, // Center M slit (reveals eyebrow center)
                { -0.07, 0.44 }, // Left-center valley
                { -0.16, 0.58 }, // Left center clump tip (touches left eye)
                { -0.26, 0.45 }, // Valley
                { -0.36, 0.60 }, // Left outer clump tip
                { -0.46, 0.48 }, // Valley
                { -0.54, 0.70 }, // Left temple lock tip
            };
            for (const auto &cl : clumps) {
                const qreal px = headCenter.x() + cl.xRatio * headWidth;
                const qreal py = topY + cl.dipRatio * headHeight;
                fringe.append(QPointF(px, py));
                fringeLine.append(KisAiStrokePoint(px, py, 0.8));
            }
        }

        ops.append(makeFill(QStringLiteral("hair_fringe"), QStringLiteral("Flats"),
                            fringe, darkerWarm(hair, 0.96), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

        // Fringe clump lineart (GPen outline for clean anime cel look)
        if (fringeLine.size() >= 2) {
            ops.append(makePath(QStringLiteral("hair_fringe_line"), QStringLiteral("Lineart"),
                                fringeLine, darkerWarm(hair, 0.65), QStringLiteral("gpen"), 0.0035, 0.95));
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
        ops.append(makePath(QStringLiteral("hair_ahoge"), QStringLiteral("Lineart"), ahoge,
                            darkerWarm(hair, 0.85), QStringLiteral("gpen"), 0.0035, 0.95));
    }

    // 5. Angel halo: specular arc above the crown (Highlights)
    QVector<KisAiStrokePoint> halo;
    for (int i = 0; i <= 12; ++i) {
        const qreal t = M_PI * (0.12 + 0.76 * i / 12.0);
        halo.append(KisAiStrokePoint(headCenter.x() + std::cos(t) * headWidth * 0.36,
                                     topY - headHeight * 0.05 - std::sin(t) * headHeight * 0.10,
                                     0.30 + 0.65 * std::sin(M_PI * i / 12.0)));
    }
    ops.append(makePath(QStringLiteral("hair_angel_halo"), QStringLiteral("Highlights"), halo,
                        QColor(255, 255, 255), QStringLiteral("airbrush"), 0.006, 0.80));

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairMassForStyle(
    const KisAiSceneSpec &spec,
    const QPointF &headCenter,
    qreal headWidth,
    qreal headHeight)
{
    QVector<KisAiStrokeOperation> ops = hairBackMassForStyle(spec, headCenter, headWidth, headHeight);
    ops.append(hairFrontMassForStyle(spec, headCenter, headWidth, headHeight));
    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::backgroundForSpec(
    const KisAiSceneSpec &spec,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops;
    const QString tod = spec.light.timeOfDay;

    QColor top(135, 180, 220), bottom(240, 244, 248);
    if (tod == QLatin1String("night")) {
        top = QColor(8, 12, 32);
        bottom = QColor(38, 52, 96);
    } else if (tod == QLatin1String("sunset")) {
        top = QColor(70, 60, 130);
        bottom = QColor(250, 150, 90);
    }
    if (spec.subject.type == QLatin1String("landscape") && !spec.palette.accents.isEmpty()) {
        bottom = spec.palette.accents.first();
    }

    KisAiStrokeOperation wash;
    wash.kind = KisAiStrokeOperation::Kind::GradientFill;
    wash.id = QStringLiteral("bg_wash");
    wash.layer = QStringLiteral("Background");
    wash.polygon = QPolygonF{}; // empty = full canvas
    // D4-3: 3-stop sky — zenith / horizon glow / ground haze for air depth.
    QColor mid = top;
    if (tod == QLatin1String("night")) {
        mid = QColor(20, 30, 70);
    } else if (tod == QLatin1String("sunset")) {
        mid = QColor(200, 100, 140);
    } else {
        mid = QColor(180, 210, 235);
    }
    wash.gradientColors = QVector<QColor>{top, mid, bottom};
    wash.angleDeg = 90.0;
    wash.brush.profile = QStringLiteral("watercolor");
    wash.brush.color = top;
    wash.brush.opacity = 1.0;
    wash.fillStyle = QStringLiteral("directional");
    ops.append(wash);

    if (spec.background.type == QLatin1String("night_sky_town")
        || (tod == QLatin1String("night") && spec.background.elements.contains(QStringLiteral("moon")))) {
        ops.append(makeFill(QStringLiteral("bg_moon"), QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.78, 0.18), 0.055, 0.055),
                            QColor(250, 244, 220), QStringLiteral("brush"), 1.0, QStringLiteral("wash")));
    }
    if (tod == QLatin1String("day") || tod == QLatin1String("sunset")) {
        ops.append(makeFill(QStringLiteral("bg_sun"), QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.76, 0.20), 0.045, 0.045),
                            tod == QLatin1String("sunset") ? QColor(255, 150, 80) : QColor(255, 246, 220),
                            QStringLiteral("brush"), 1.0, QStringLiteral("wash")));
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
        const QColor ridgeColor = tod == QLatin1String("night") ? QColor(20, 30, 60) : QColor(90, 110, 130);
        ops.append(makeFill(QStringLiteral("bg_ridge"), QStringLiteral("Background"),
                            ridge, ridgeColor, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
        // D4-3: distant haze band — aerial perspective between sky and ridge.
        QPolygonF haze;
        haze.append(QPointF(0.0, 0.62));
        haze.append(QPointF(0.30, 0.55));
        haze.append(QPointF(0.62, 0.60));
        haze.append(QPointF(1.0, 0.52));
        haze.append(QPointF(1.0, 0.66));
        haze.append(QPointF(0.0, 0.70));
        const QColor hazeColor = tod == QLatin1String("night") ? QColor(50, 65, 120, 110)
            : tod == QLatin1String("sunset") ? QColor(240, 170, 150, 110) : QColor(210, 225, 240, 110);
        ops.append(makeFill(QStringLiteral("bg_haze"), QStringLiteral("Background"),
                            haze, hazeColor, QStringLiteral("watercolor"), 0.45, QStringLiteral("wash")));
    } else {
        // Character backdrop: soft floor shadow ellipse grounds the bust.
        ops.append(makeFill(QStringLiteral("bg_floor_shadow"), QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.5, 0.94), 0.30, 0.045),
                            darkerWarm(QColor(160, 170, 190), 0.75),
                            QStringLiteral("watercolor"), 0.5, QStringLiteral("wash")));
    }
    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::clothingForSpec(
    const KisAiSceneSpec &spec,
    const QPointF &hc,
    qreal hw,
    qreal hh,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
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
    ops.append(makeFill(QStringLiteral("neck"), QStringLiteral("Flats"), neck,
                        darkerWarm(skin, 0.95), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    // Neck cast shadow (under chin shadow)
    QPolygonF neckShadow;
    neckShadow.append(QPointF(hc.x() - neckW * 0.45, neckTopY));
    neckShadow.append(QPointF(hc.x() + neckW * 0.45, neckTopY));
    neckShadow.append(QPointF(hc.x() + neckW * 0.35, neckTopY + hh * 0.12));
    neckShadow.append(QPointF(hc.x(), neckTopY + hh * 0.16));
    neckShadow.append(QPointF(hc.x() - neckW * 0.35, neckTopY + hh * 0.12));
    ops.append(makeFill(QStringLiteral("neck_shadow"), QStringLiteral("Shading"), neckShadow,
                        darkerWarm(skin, 0.82), QStringLiteral("watercolor"), 0.65, QStringLiteral("wash")));

    // Sternocleidomastoid lines (首筋の繊細なライン)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> scm;
        scm.append(KisAiStrokePoint(hc.x() + side * neckW * 0.40, neckTopY + hh * 0.05, 0.4));
        scm.append(KisAiStrokePoint(hc.x() + side * neckW * 0.15, neckBotY - hh * 0.02, 0.6));
        ops.append(makePath(side < 0 ? QStringLiteral("neck_scm_l") : QStringLiteral("neck_scm_r"),
                            QStringLiteral("Lineart"), scm,
                            darkerWarm(skin, 0.70), QStringLiteral("gpen"), 0.0025, 0.60));
    }

    // 2. Clavicle lines (鎖骨)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> clavicle;
        clavicle.append(KisAiStrokePoint(hc.x() + side * neckW * 0.12, neckBotY, 0.7));
        clavicle.append(KisAiStrokePoint(hc.x() + side * neckW * 0.75, neckBotY + hh * 0.02, 0.8));
        clavicle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.45, neckBotY + hh * 0.06, 0.4));
        ops.append(makePath(side < 0 ? QStringLiteral("clavicle_l") : QStringLiteral("clavicle_r"),
                            QStringLiteral("Lineart"), clavicle,
                            darkerWarm(skin, 0.68), QStringLiteral("gpen"), 0.0028, 0.70));
    }

    // 3. Clothing Body Mass (natural sloping shoulders and chest contours)
    QPolygonF torso;
    torso.append(QPointF(hc.x() - neckW * 0.55, neckBotY));
    torso.append(QPointF(hc.x() + neckW * 0.55, neckBotY));
    torso.append(QPointF(hc.x() + shoulderW * 0.55, shoulderY + hh * 0.10));
    torso.append(QPointF(hc.x() + shoulderW * 0.62, bustBottomY));
    torso.append(QPointF(hc.x() - shoulderW * 0.62, bustBottomY));
    torso.append(QPointF(hc.x() - shoulderW * 0.55, shoulderY + hh * 0.10));
    ops.append(makeFill(QStringLiteral("clothing"), QStringLiteral("Flats"), torso,
                        mainCloth, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    // 4. Style-Specific Costume Details
    if (style == QLatin1String("school_uniform") || style == QLatin1String("sailor")) {
        // Sailor Collar: V-neck triangular flap
        QPolygonF collar;
        collar.append(QPointF(hc.x() - neckW * 0.70, neckBotY - hh * 0.02));
        collar.append(QPointF(hc.x() + neckW * 0.70, neckBotY - hh * 0.02));
        collar.append(QPointF(hc.x() + neckW * 0.50, shoulderY + hh * 0.22));
        collar.append(QPointF(hc.x(), shoulderY + hh * 0.28));
        collar.append(QPointF(hc.x() - neckW * 0.50, shoulderY + hh * 0.22));
        ops.append(makeFill(QStringLiteral("cloth_sailor_collar"), QStringLiteral("Flats"), collar,
                            secCloth, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

        // Collar stripe lineart
        QVector<KisAiStrokePoint> stripe;
        stripe.append(KisAiStrokePoint(hc.x() - neckW * 0.64, neckBotY + hh * 0.02, 0.8));
        stripe.append(KisAiStrokePoint(hc.x() - neckW * 0.44, shoulderY + hh * 0.20, 0.8));
        stripe.append(KisAiStrokePoint(hc.x(), shoulderY + hh * 0.25, 0.9));
        stripe.append(KisAiStrokePoint(hc.x() + neckW * 0.44, shoulderY + hh * 0.20, 0.8));
        stripe.append(KisAiStrokePoint(hc.x() + neckW * 0.64, neckBotY + hh * 0.02, 0.8));
        ops.append(makePath(QStringLiteral("cloth_collar_stripe"), QStringLiteral("Lineart"), stripe,
                            mainCloth.darker(130), QStringLiteral("gpen"), 0.0035, 0.95));

        // Chest Ribbon / Scarf
        const qreal knotY = shoulderY + hh * 0.22;
        QPolygonF knot;
        knot.append(QPointF(hc.x() - 0.022, knotY - 0.015));
        knot.append(QPointF(hc.x() + 0.022, knotY - 0.015));
        knot.append(QPointF(hc.x() + 0.018, knotY + 0.018));
        knot.append(QPointF(hc.x() - 0.018, knotY + 0.018));
        ops.append(makeFill(QStringLiteral("cloth_ribbon_knot"), QStringLiteral("Flats"), knot,
                            accCloth.darker(115), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

        // Ribbon wings (left and right)
        for (int side = -1; side <= 1; side += 2) {
            QPolygonF wing;
            wing.append(QPointF(hc.x() + side * 0.015, knotY - 0.005));
            wing.append(QPointF(hc.x() + side * 0.08, knotY + 0.02));
            wing.append(QPointF(hc.x() + side * 0.065, knotY + 0.09));
            wing.append(QPointF(hc.x() + side * 0.01, knotY + 0.03));
            ops.append(makeFill(side < 0 ? QStringLiteral("ribbon_wing_l") : QStringLiteral("ribbon_wing_r"),
                                QStringLiteral("Flats"), wing, accCloth, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
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
                                QStringLiteral("Flats"), hoodFold, mainCloth.lighter(115), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
        }

        // Drawstrings (フードの紐)
        for (int side = -1; side <= 1; side += 2) {
            QVector<KisAiStrokePoint> string;
            const qreal sx = hc.x() + side * 0.045;
            string.append(KisAiStrokePoint(sx, shoulderY + hh * 0.14, 0.7));
            string.append(KisAiStrokePoint(sx + side * 0.01, shoulderY + hh * 0.30, 0.8));
            string.append(KisAiStrokePoint(sx, shoulderY + hh * 0.44, 0.6));
            ops.append(makePath(side < 0 ? QStringLiteral("hood_string_l") : QStringLiteral("hood_string_r"),
                                QStringLiteral("Lineart"), string, secCloth, QStringLiteral("gpen"), 0.003, 0.95));
        }

    } else if (style == QLatin1String("dress")) {
        // Sweetheart / Square neckline with collar trim
        QPolygonF chestSkin;
        chestSkin.append(QPointF(hc.x() - neckW * 0.55, neckBotY));
        chestSkin.append(QPointF(hc.x() + neckW * 0.55, neckBotY));
        chestSkin.append(QPointF(hc.x() + neckW * 0.45, shoulderY + hh * 0.15));
        chestSkin.append(QPointF(hc.x(), shoulderY + hh * 0.20));
        chestSkin.append(QPointF(hc.x() - neckW * 0.45, shoulderY + hh * 0.15));
        ops.append(makeFill(QStringLiteral("dress_decollete"), QStringLiteral("Flats"), chestSkin,
                            skin, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

        // Neckline trim / frill
        QVector<KisAiStrokePoint> trim;
        trim.append(KisAiStrokePoint(hc.x() - neckW * 0.52, shoulderY + hh * 0.14, 0.8));
        trim.append(KisAiStrokePoint(hc.x() - neckW * 0.25, shoulderY + hh * 0.18, 0.9));
        trim.append(KisAiStrokePoint(hc.x(), shoulderY + hh * 0.21, 0.8));
        trim.append(KisAiStrokePoint(hc.x() + neckW * 0.25, shoulderY + hh * 0.18, 0.9));
        trim.append(KisAiStrokePoint(hc.x() + neckW * 0.52, shoulderY + hh * 0.14, 0.8));
        ops.append(makePath(QStringLiteral("dress_trim"), QStringLiteral("Lineart"), trim,
                            secCloth, QStringLiteral("gpen"), 0.0035, 0.95));

    } else {
        // Casual Crewneck / T-Shirt
        QPolygonF crewNeck;
        crewNeck.append(QPointF(hc.x() - neckW * 0.50, neckBotY));
        crewNeck.append(QPointF(hc.x() + neckW * 0.50, neckBotY));
        crewNeck.append(QPointF(hc.x() + neckW * 0.38, neckBotY + hh * 0.10));
        crewNeck.append(QPointF(hc.x(), neckBotY + hh * 0.14));
        crewNeck.append(QPointF(hc.x() - neckW * 0.38, neckBotY + hh * 0.10));
        ops.append(makeFill(QStringLiteral("casual_rib_collar"), QStringLiteral("Flats"), crewNeck,
                            secCloth, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
    }

    // Fabric wrinkle shadow lines (胸元と脇の布シワ)
    for (int side = -1; side <= 1; side += 2) {
        QVector<KisAiStrokePoint> wrinkle;
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.50, shoulderY + hh * 0.25, 0.3));
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.30, shoulderY + hh * 0.38, 0.7));
        wrinkle.append(KisAiStrokePoint(hc.x() + side * shoulderW * 0.18, shoulderY + hh * 0.44, 0.2));
        ops.append(makePath(side < 0 ? QStringLiteral("wrinkle_l") : QStringLiteral("wrinkle_r"),
                            QStringLiteral("Lineart"), wrinkle,
                            mainCloth.darker(140), QStringLiteral("gpen"), 0.0028, 0.75));
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::characterProgram(
    const KisAiSceneSpec &spec,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops;
    const QPointF hc = spec.composition.headCenter;
    const qreal hh = spec.composition.headHeight;
    const qreal hw = hh * 0.78;
    const QColor skin = spec.head.skinTone;
    const QColor hair = spec.head.hairColor;

    // Hair back mass first (behind everything in Flats order).
    ops.append(hairBackMassForStyle(spec, hc, hw, hh));

    // Anatomical neck + shoulders + clothing
    ops.append(clothingForSpec(spec, hc, hw, hh, canvasSize));

    // Face skin over hair back mass.
    ops.append(makeFill(QStringLiteral("face_skin"), QStringLiteral("Flats"),
                        headOutlinePolygon(hc, hw, hh), skin,
                        QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    // Ears.
    for (int side = -1; side <= 1; side += 2) {
        ops.append(makeFill(side < 0 ? QStringLiteral("ear_l") : QStringLiteral("ear_r"),
                            QStringLiteral("Flats"),
                            ellipsePolygon(QPointF(hc.x() + side * hw * 0.50, hc.y() + hh * 0.10), hw * 0.05, hh * 0.07),
                            skin, QStringLiteral("brush"), 1.0, QStringLiteral("wash")));
    }

    // Eye pair with perspective compression and smart facial features
    const auto eyes = eyePairCenters(hc, hw, hh, spec.subject.facing);
    const qreal gazeShift = spec.head.gaze == QLatin1String("left") ? -0.012
        : spec.head.gaze == QLatin1String("right") ? 0.012 : 0.0;
    const QString expr = spec.head.expression;
    const QString eyeExpr = (expr == QLatin1String("half")) ? QStringLiteral("half")
        : (expr == QLatin1String("closed")) ? QStringLiteral("closed")
        : (expr == QLatin1String("smile_closed") || expr == QLatin1String("neutral")) ? QStringLiteral("smile")
        : QStringLiteral("open");

    const bool isRightFacing = (spec.subject.facing == QLatin1String("front-right"));
    const bool isLeftFacing = (spec.subject.facing == QLatin1String("front-left"));
    const qreal eyeScaleX[2] = {
        isRightFacing ? 0.86 : (isLeftFacing ? 1.04 : 1.0),  // Left eye
        isRightFacing ? 1.04 : (isLeftFacing ? 0.86 : 1.0),  // Right eye
    };

    const QPointF centers[2] = {eyes.first, eyes.second};
    for (int i = 0; i < 2; ++i) {
        KisAiStrokeOperation eye;
        eye.kind = KisAiStrokeOperation::Kind::AnimeEye;
        eye.id = i == 0 ? QStringLiteral("left_eye") : QStringLiteral("right_eye");
        eye.layer = QStringLiteral("Lineart");
        eye.eyeCenter = QPointF(centers[i].x() + gazeShift, centers[i].y());
        const qreal eyeH = (eyeExpr == QLatin1String("open") || eyeExpr == QLatin1String("smile"))
            ? hh * 0.20 : hh * 0.10;
        eye.eyeSize = QSizeF(hw * 0.30 * eyeScaleX[i], eyeH);
        eye.eyeIrisColor = spec.head.eyeColor;
        eye.eyeSecondaryColor = KisAiLightRig::highlightColor(spec.head.eyeColor,
            KisAiLightRig::fromSpec(spec));
        eye.eyeStyle = QStringLiteral("sparkle");
        eye.eyeExpression = eyeExpr;
        eye.eyeIsRight = (i == 1);
        ops.append(eye);

        // Eyelid cast shadow over upper sclera & iris
        QPolygonF eyelidShadow;
        const qreal ew = eye.eyeSize.width();
        const qreal eh = eye.eyeSize.height();
        eyelidShadow.append(QPointF(eye.eyeCenter.x() - ew * 0.45, eye.eyeCenter.y() - eh * 0.10));
        eyelidShadow.append(QPointF(eye.eyeCenter.x() + ew * 0.45, eye.eyeCenter.y() - eh * 0.10));
        eyelidShadow.append(QPointF(eye.eyeCenter.x() + ew * 0.35, eye.eyeCenter.y() + eh * 0.12));
        eyelidShadow.append(QPointF(eye.eyeCenter.x() - ew * 0.35, eye.eyeCenter.y() + eh * 0.12));
        ops.append(makeFill(i == 0 ? QStringLiteral("eyelid_shade_l") : QStringLiteral("eyelid_shade_r"),
                            QStringLiteral("Shading"), eyelidShadow,
                            darkerWarm(skin, 0.78), QStringLiteral("watercolor"), 0.40, QStringLiteral("wash")));

        // Tear trough (涙袋) subtle highlight: delicate and soft
        QVector<KisAiStrokePoint> tearTrough;
        tearTrough.append(KisAiStrokePoint(eye.eyeCenter.x() - ew * 0.25, eye.eyeCenter.y() + eh * 0.52, 0.2));
        tearTrough.append(KisAiStrokePoint(eye.eyeCenter.x(), eye.eyeCenter.y() + eh * 0.54, 0.45));
        tearTrough.append(KisAiStrokePoint(eye.eyeCenter.x() + ew * 0.25, eye.eyeCenter.y() + eh * 0.52, 0.2));
        ops.append(makePath(i == 0 ? QStringLiteral("tear_trough_l") : QStringLiteral("tear_trough_r"),
                            QStringLiteral("Highlights"), tearTrough,
                            QColor(255, 242, 245), QStringLiteral("airbrush"), 0.0022, 0.40));
    }

    // Brows: gentle anime arch sitting right above the eye socket
    for (int side = -1; side <= 1; side += 2) {
        const qreal innerX = hc.x() + side * hw * 0.08;
        const qreal archX  = hc.x() + side * hw * 0.18;
        const qreal outerX = hc.x() + side * hw * 0.28;
        const qreal baseBrowY = hc.y() - hh * 0.04;

        QVector<KisAiStrokePoint> brow;
        brow.append(KisAiStrokePoint(innerX, baseBrowY + hh * 0.005, 0.45));
        brow.append(KisAiStrokePoint(archX,  baseBrowY - hh * 0.012, 0.85));
        brow.append(KisAiStrokePoint(outerX, baseBrowY + hh * 0.002, 0.30));
        ops.append(makePath(side < 0 ? QStringLiteral("brow_l") : QStringLiteral("brow_r"),
                            QStringLiteral("Lineart"), brow,
                            darkerWarm(hair, 0.72), QStringLiteral("gpen"), 0.0030, 0.85));
    }

    // Nose: shadow dot + specular highlight point (delicate anime nose)
    ops.append(makeFill(QStringLiteral("nose_shadow"), QStringLiteral("Shading"),
                        ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.22), hw * 0.016, hh * 0.012),
                        darkerWarm(skin, 0.88), QStringLiteral("watercolor"), 0.45, QStringLiteral("wash")));
    ops.append(makeFill(QStringLiteral("nose_tip_hl"), QStringLiteral("Highlights"),
                        ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.20), hw * 0.008, hh * 0.008),
                        QColor(255, 255, 255), QStringLiteral("airbrush"), 0.75, QStringLiteral("wash")));

    // Mouth by expression with lip gloss highlight
    if (expr == QLatin1String("smile_open")) {
        QPolygonF mouth;
        const qreal mw = hw * 0.075;
        const qreal mTopY = hc.y() + hh * 0.325;
        const qreal mBotY = hc.y() + hh * 0.375;
        // Upper edge: gentle flat/arch
        mouth.append(QPointF(hc.x() - mw, mTopY));
        mouth.append(QPointF(hc.x(), mTopY + hh * 0.004));
        mouth.append(QPointF(hc.x() + mw, mTopY));
        // Lower edge: smooth semicircular bowl
        const int mSteps = 8;
        for (int i = 0; i <= mSteps; ++i) {
            const qreal t = M_PI * (qreal)i / mSteps;
            mouth.append(QPointF(hc.x() + std::cos(t) * mw, mTopY + std::sin(t) * (mBotY - mTopY)));
        }
        ops.append(makeFill(QStringLiteral("mouth"), QStringLiteral("Lineart"), mouth,
                            QColor(190, 60, 75), QStringLiteral("brush"), 0.95, QStringLiteral("contour")));
    } else {
        QVector<KisAiStrokePoint> lip;
        const qreal smileLift = (expr == QLatin1String("neutral") || expr == QLatin1String("closed")) ? 0.0 : -0.008;
        lip.append(KisAiStrokePoint(hc.x() - hw * 0.07, hc.y() + hh * 0.34, 0.4));
        lip.append(KisAiStrokePoint(hc.x(), hc.y() + hh * 0.345 + smileLift, 0.9));
        lip.append(KisAiStrokePoint(hc.x() + hw * 0.07, hc.y() + hh * 0.34, 0.4));
        ops.append(makePath(QStringLiteral("mouth"), QStringLiteral("Lineart"), lip,
                            QColor(160, 65, 75), QStringLiteral("gpen"), 0.0035, 0.9));

        // Lip gloss highlight on lower lip
        ops.append(makeFill(QStringLiteral("lip_gloss"), QStringLiteral("Highlights"),
                            ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.355), hw * 0.015, hh * 0.007),
                            QColor(255, 255, 255), QStringLiteral("airbrush"), 0.65, QStringLiteral("wash")));
    }

    // Cheek blush (soft natural bloom)
    for (int side = -1; side <= 1; side += 2) {
        ops.append(makeFill(side < 0 ? QStringLiteral("blush_l") : QStringLiteral("blush_r"),
                            QStringLiteral("Shading"),
                            ellipsePolygon(QPointF(hc.x() + side * hw * 0.30, hc.y() + hh * 0.26), hw * 0.09, hh * 0.05),
                            QColor(255, 159, 178), QStringLiteral("watercolor"), 0.35, QStringLiteral("wash")));
    }

    // D3-5: skin subsurface scattering — faint warm red where light passes
    // through thin skin (nose tip, cheek peaks, ears). Suppressed at night,
    // amplified at sunset via the LightRig time-of-day.
    {
        const QString tod = spec.light.timeOfDay;
        const qreal sssGain = tod == QLatin1String("night") ? 0.55
            : tod == QLatin1String("sunset") ? 1.35 : 1.0;
        const qreal sssAlpha = qBound<qreal>(0.06, 0.16 * sssGain, 0.24);
        ops.append(makeFill(QStringLiteral("sss_nose_tip"), QStringLiteral("Shading"),
                            ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.235), hw * 0.022, hh * 0.016),
                            QColor(255, 122, 120), QStringLiteral("watercolor"), sssAlpha, QStringLiteral("wash")));
        for (int side = -1; side <= 1; side += 2) {
            ops.append(makeFill(side < 0 ? QStringLiteral("sss_cheek_l") : QStringLiteral("sss_cheek_r"),
                                QStringLiteral("Shading"),
                                ellipsePolygon(QPointF(hc.x() + side * hw * 0.30, hc.y() + hh * 0.26), hw * 0.055, hh * 0.030),
                                QColor(255, 138, 130), QStringLiteral("watercolor"), sssAlpha * 0.8, QStringLiteral("wash")));
            ops.append(makeFill(side < 0 ? QStringLiteral("sss_ear_l") : QStringLiteral("sss_ear_r"),
                                QStringLiteral("Shading"),
                                ellipsePolygon(QPointF(hc.x() + side * hw * 0.50, hc.y() + hh * 0.10), hw * 0.030, hh * 0.045),
                                QColor(255, 120, 115), QStringLiteral("watercolor"), sssAlpha, QStringLiteral("wash")));
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
            ops.append(makePath(QStringLiteral("face_contour"), QStringLiteral("Lineart"), jaw,
                                QColor(28, 24, 40), QStringLiteral("gpen"), 0.005, 1.0));
        }
    }

    // Hair front mass (fringe, side locks, ahoge, halo) over face skin and contour
    ops.append(hairFrontMassForStyle(spec, hc, hw, hh));

    // Rig-driven shading: core shadows + rim + chin AO + hair band.
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

    // Opt-in lineart hierarchy (outer contours heavier than details).
    KisAiStrokeQualityUtils::applyLineartHierarchy(ops);
    return ops;
}

QVector<KisAiStrokeOperation> KisAiLayoutEngine::landscapeProgram(
    const KisAiSceneSpec &spec,
    const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops = backgroundForSpec(spec, canvasSize);
    const KisAiLightSettings rig = KisAiLightRig::fromSpec(spec);

    // Foreground meadow mass for depth.
    QPolygonF meadow;
    meadow.append(QPointF(0.0, 0.80));
    meadow.append(QPointF(0.25, 0.74));
    meadow.append(QPointF(0.55, 0.82));
    meadow.append(QPointF(0.80, 0.76));
    meadow.append(QPointF(1.0, 0.84));
    meadow.append(QPointF(1.0, 1.0));
    meadow.append(QPointF(0.0, 1.0));
    const QColor meadowColor = spec.light.timeOfDay == QLatin1String("night") ? QColor(24, 40, 60) : QColor(96, 140, 110);
    ops.append(makeFill(QStringLiteral("meadow"), QStringLiteral("Flats"), meadow,
                        meadowColor, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    QVector<KisAiStrokeOperation> flatsOnly;
    for (const KisAiStrokeOperation &op : ops) {
        if (KisAiStrokeProgramCodec::normalizeLayerName(op.layer) == QLatin1String("Flats"))
            flatsOnly.append(op);
    }
    ops.append(KisAiLightRig::synthesizeShading(flatsOnly, rig, canvasSize, nullptr));
    return ops;
}

KisAiStrokeProgram KisAiLayoutEngine::generateProgram(
    const KisAiSceneSpec &spec,
    const QSize &canvasSize)
{
    KisAiStrokeProgram program;
    program.schemaVersion = 2;
    program.prompt = spec.prompt;
    program.canvasSize = canvasSize.isValid() ? canvasSize : QSize(1024, 1024);
    program.title = QStringLiteral("SceneSpec Composition");

    QVector<KisAiStrokeOperation> ops;
    if (spec.subject.type == QLatin1String("landscape")) {
        ops = landscapeProgram(spec, program.canvasSize); // includes background
    } else {
        ops = backgroundForSpec(spec, program.canvasSize);
        ops.append(characterProgram(spec, program.canvasSize));
    }

    program.operations = ops;
    KisAiStrokeQualityReport report;
    KisAiStrokeProgram refined = KisAiStrokeProgramCodec::refineForRendering(program, &report);
    refined.prompt = spec.prompt;
    refined.title = program.title;
    return refined;
}
