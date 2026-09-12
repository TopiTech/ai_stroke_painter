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
    // Canonical anime head: soft superellipse crown tapering to a chin point.
    // Symmetric by construction around center.x().
    QPolygonF poly;
    const int segments = 28;
    poly.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        const qreal t = 2.0 * M_PI * i / segments; // 0 = +x axis
        const qreal c = std::cos(t), s = std::sin(t);
        // Superellipse exponent flattens the crown; chin sharpening below.
        qreal ex = 0.85, ey = 1.0;
        if (s > 0.15) { // jaw region: pull sides in toward the chin point
            const qreal jaw = qMin<qreal>(1.0, (s - 0.15) / 0.85);
            ex = 0.85 * (1.0 - 0.42 * jaw);
        }
        const qreal x = center.x() + (width * 0.5) * (c >= 0 ? std::pow(c, ex) : -std::pow(-c, ex));
        const qreal y = center.y() + (height * 0.5) * (s >= 0 ? std::pow(s, ey) : -std::pow(-s, ey));
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

QVector<KisAiStrokeOperation> KisAiLayoutEngine::hairMassForStyle(
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

    qreal backLength = headHeight * 1.15; // long_hime default
    if (style == QLatin1String("bob"))
        backLength = headHeight * 0.72;
    else if (style == QLatin1String("short_messy") || style == QLatin1String("short_straight"))
        backLength = headHeight * 0.62;
    else if (style == QLatin1String("twin_tails"))
        backLength = headHeight * 1.30;

    // Back mass: rounded curtain behind the head (appended before the face).
    QPolygonF back;
    back.append(QPointF(headCenter.x() - headWidth * 0.62, topY - headHeight * 0.10));
    back.append(QPointF(headCenter.x() + headWidth * 0.62, topY - headHeight * 0.10));
    back.append(QPointF(headCenter.x() + headWidth * 0.58, chinY + backLength * 0.55));
    back.append(QPointF(headCenter.x() + headWidth * 0.30, chinY + backLength * 0.72));
    back.append(QPointF(headCenter.x(), chinY + backLength * 0.62));
    back.append(QPointF(headCenter.x() - headWidth * 0.30, chinY + backLength * 0.72));
    back.append(QPointF(headCenter.x() - headWidth * 0.58, chinY + backLength * 0.55));
    ops.append(makeFill(QStringLiteral("hair_back_mass"), QStringLiteral("Flats"),
                        back, hair, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    // Fringe (M-bangs): symmetric zigzag across the forehead.
    const int spikes = (style == QLatin1String("short_messy")) ? 7 : 5;
    QPolygonF fringe;
    fringe.append(QPointF(headCenter.x() - headWidth * 0.52, topY + headHeight * 0.02));
    fringe.append(QPointF(headCenter.x() + headWidth * 0.52, topY + headHeight * 0.02));
    fringe.append(QPointF(headCenter.x() + headWidth * 0.52, topY + headHeight * 0.16));
    for (int i = spikes; i >= 0; --i) {
        const qreal x = headCenter.x() - headWidth * 0.52 + headWidth * 1.04 * i / spikes;
        const qreal dip = (i % 2 == 0) ? headHeight * 0.30 : headHeight * 0.20;
        fringe.append(QPointF(x, topY + dip));
    }
    fringe.append(QPointF(headCenter.x() - headWidth * 0.52, topY + headHeight * 0.16));
    ops.append(makeFill(QStringLiteral("hair_fringe"), QStringLiteral("Flats"),
                        fringe, darkerWarm(hair), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    // Side locks: tapered ribbons framing the face.
    for (int side = -1; side <= 1; side += 2) {
        KisAiStrokeOperation lock;
        lock.kind = KisAiStrokeOperation::Kind::Ribbon;
        lock.id = side < 0 ? QStringLiteral("hair_side_lock_l") : QStringLiteral("hair_side_lock_r");
        lock.layer = QStringLiteral("Flats");
        lock.brush.profile = QStringLiteral("hair");
        lock.brush.color = hair;
        lock.brush.opacity = 1.0;
        const qreal x0 = headCenter.x() + side * headWidth * 0.52;
        lock.spine = QVector<QPointF>{
            QPointF(x0, topY + headHeight * 0.10),
            QPointF(x0 + side * headWidth * 0.06, headCenter.y() + headHeight * 0.25),
            QPointF(x0 + side * headWidth * 0.02, chinY + backLength * 0.45),
        };
        lock.widthStart = 0.035;
        lock.widthMid = 0.028;
        lock.widthEnd = 0.006;
        ops.append(lock);
    }

    if (style == QLatin1String("twin_tails")) {
        for (int side = -1; side <= 1; side += 2) {
            QPolygonF tail;
            const qreal xRoot = headCenter.x() + side * headWidth * 0.55;
            tail.append(QPointF(xRoot, topY + headHeight * 0.05));
            tail.append(QPointF(xRoot + side * headWidth * 0.35, topY - headHeight * 0.25));
            tail.append(QPointF(xRoot + side * headWidth * 0.55, chinY + backLength * 0.55));
            tail.append(QPointF(xRoot + side * headWidth * 0.28, chinY + backLength * 0.60));
            tail.append(QPointF(xRoot + side * headWidth * 0.10, headCenter.y()));
            ops.append(makeFill(side < 0 ? QStringLiteral("hair_tail_l") : QStringLiteral("hair_tail_r"),
                                QStringLiteral("Flats"), tail, darkerWarm(hair, 0.9),
                                QStringLiteral("brush"), 1.0, QStringLiteral("contour")));
        }
    }

    // Angel halo: specular arc above the crown (Highlights).
    QVector<KisAiStrokePoint> halo;
    for (int i = 0; i <= 10; ++i) {
        const qreal t = M_PI * (0.15 + 0.70 * i / 10.0);
        halo.append(KisAiStrokePoint(headCenter.x() + std::cos(t) * headWidth * 0.34,
                                     topY - headHeight * 0.06 - std::sin(t) * headHeight * 0.10,
                                     0.35 + 0.5 * std::sin(M_PI * i / 10.0)));
    }
    ops.append(makePath(QStringLiteral("hair_angel_halo"), QStringLiteral("Highlights"), halo,
                        QColor(255, 255, 255), QStringLiteral("airbrush"), 0.006, 0.75));
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
    wash.gradientColors = QVector<QColor>{top, bottom};
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
    } else {
        // Character backdrop: soft floor shadow ellipse grounds the bust.
        ops.append(makeFill(QStringLiteral("bg_floor_shadow"), QStringLiteral("Background"),
                            ellipsePolygon(QPointF(0.5, 0.94), 0.30, 0.045),
                            darkerWarm(QColor(160, 170, 190), 0.75),
                            QStringLiteral("watercolor"), 0.5, QStringLiteral("wash")));
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
    ops.append(hairMassForStyle(spec, hc, hw, hh));

    // Neck + shoulders + clothing bust.
    QPolygonF neck;
    neck.append(QPointF(hc.x() - hw * 0.16, hc.y() + hh * 0.42));
    neck.append(QPointF(hc.x() + hw * 0.16, hc.y() + hh * 0.42));
    neck.append(QPointF(hc.x() + hw * 0.20, hc.y() + hh * 0.62));
    neck.append(QPointF(hc.x() - hw * 0.20, hc.y() + hh * 0.62));
    ops.append(makeFill(QStringLiteral("neck"), QStringLiteral("Flats"), neck,
                        darkerWarm(skin), QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

    const qreal shoulderY = hc.y() + hh * 0.62;
    QPolygonF torso;
    torso.append(QPointF(hc.x() - hw * 0.20, shoulderY));
    torso.append(QPointF(hc.x() + hw * 0.20, shoulderY));
    torso.append(QPointF(hc.x() + hw * 0.95, shoulderY + hh * 0.55));
    torso.append(QPointF(hc.x() - hw * 0.95, shoulderY + hh * 0.55));
    const QColor cloth = spec.palette.accents.isEmpty() ? spec.palette.keyColor : spec.palette.accents.first();
    ops.append(makeFill(QStringLiteral("clothing"), QStringLiteral("Flats"), torso,
                        cloth, QStringLiteral("brush"), 1.0, QStringLiteral("contour")));

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

    // Eye pair (procedural AnimeEye, rig-placed and symmetric).
    const auto eyes = eyePairCenters(hc, hw, hh, spec.subject.facing);
    const qreal gazeShift = spec.head.gaze == QLatin1String("left") ? -0.012
        : spec.head.gaze == QLatin1String("right") ? 0.012 : 0.0;
    const QString expr = spec.head.expression;
    const QString eyeExpr = (expr == QLatin1String("half")) ? QStringLiteral("half")
        : (expr == QLatin1String("closed")) ? QStringLiteral("closed")
        : (expr == QLatin1String("smile_closed") || expr == QLatin1String("neutral")) ? QStringLiteral("smile")
        : QStringLiteral("open");
    const QPointF centers[2] = {eyes.first, eyes.second};
    for (int i = 0; i < 2; ++i) {
        KisAiStrokeOperation eye;
        eye.kind = KisAiStrokeOperation::Kind::AnimeEye;
        eye.id = i == 0 ? QStringLiteral("left_eye") : QStringLiteral("right_eye");
        eye.layer = QStringLiteral("Lineart");
        eye.eyeCenter = QPointF(centers[i].x() + gazeShift, centers[i].y());
        const qreal eyeH = (eyeExpr == QLatin1String("open") || eyeExpr == QLatin1String("smile"))
            ? hh * 0.20 : hh * 0.10;
        eye.eyeSize = QSizeF(hw * 0.30, eyeH);
        eye.eyeIrisColor = spec.head.eyeColor;
        eye.eyeSecondaryColor = KisAiLightRig::highlightColor(spec.head.eyeColor,
            KisAiLightRig::fromSpec(spec));
        eye.eyeStyle = QStringLiteral("sparkle");
        eye.eyeExpression = eyeExpr;
        eye.eyeIsRight = (i == 1);
        ops.append(eye);
    }

    // Brows.
    for (int side = -1; side <= 1; side += 2) {
        const qreal bx = hc.x() + side * hw * 0.19;
        QVector<KisAiStrokePoint> brow;
        brow.append(KisAiStrokePoint(bx - hw * 0.09, hc.y() - hh * 0.10, 0.5));
        brow.append(KisAiStrokePoint(bx, hc.y() - hh * 0.12, 0.9));
        brow.append(KisAiStrokePoint(bx + hw * 0.09, hc.y() - hh * 0.10, 0.5));
        ops.append(makePath(side < 0 ? QStringLiteral("brow_l") : QStringLiteral("brow_r"),
                            QStringLiteral("Lineart"), brow,
                            darkerWarm(hair, 0.7), QStringLiteral("gpen"), 0.004, 0.9));
    }

    // Nose: tiny shadow dot + highlight point (never a black hole).
    ops.append(makeFill(QStringLiteral("nose_shadow"), QStringLiteral("Shading"),
                        ellipsePolygon(QPointF(hc.x(), hc.y() + hh * 0.22), hw * 0.018, hh * 0.014),
                        darkerWarm(skin, 0.88), QStringLiteral("watercolor"), 0.5, QStringLiteral("wash")));

    // Mouth by expression.
    if (expr == QLatin1String("smile_open")) {
        QPolygonF mouth;
        mouth.append(QPointF(hc.x() - hw * 0.07, hc.y() + hh * 0.32));
        mouth.append(QPointF(hc.x() + hw * 0.07, hc.y() + hh * 0.32));
        mouth.append(QPointF(hc.x() + hw * 0.05, hc.y() + hh * 0.38));
        mouth.append(QPointF(hc.x() - hw * 0.05, hc.y() + hh * 0.38));
        ops.append(makeFill(QStringLiteral("mouth"), QStringLiteral("Lineart"), mouth,
                            QColor(150, 70, 70), QStringLiteral("brush"), 0.95, QStringLiteral("contour")));
    } else {
        QVector<KisAiStrokePoint> lip;
        const qreal smileLift = (expr == QLatin1String("neutral") || expr == QLatin1String("closed")) ? 0.0 : -0.008;
        lip.append(KisAiStrokePoint(hc.x() - hw * 0.07, hc.y() + hh * 0.34, 0.4));
        lip.append(KisAiStrokePoint(hc.x(), hc.y() + hh * 0.345 + smileLift, 0.9));
        lip.append(KisAiStrokePoint(hc.x() + hw * 0.07, hc.y() + hh * 0.34, 0.4));
        ops.append(makePath(QStringLiteral("mouth"), QStringLiteral("Lineart"), lip,
                            QColor(150, 70, 70), QStringLiteral("gpen"), 0.0035, 0.9));
    }

    // Cheek blush (renderer softens blush ids into radial washes).
    for (int side = -1; side <= 1; side += 2) {
        ops.append(makeFill(side < 0 ? QStringLiteral("blush_l") : QStringLiteral("blush_r"),
                            QStringLiteral("Shading"),
                            ellipsePolygon(QPointF(hc.x() + side * hw * 0.30, hc.y() + hh * 0.26), hw * 0.09, hh * 0.05),
                            QColor(255, 159, 178), QStringLiteral("watercolor"), 0.35, QStringLiteral("wash")));
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
