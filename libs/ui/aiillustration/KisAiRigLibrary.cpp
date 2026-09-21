/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiRigLibrary.h"

#include "KisAiStrokeProgram.h"

#include <QRandomGenerator>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace
{
qreal clampRange(qreal v, qreal lo, qreal hi)
{
    return qBound<qreal>(lo, v, hi);
}

QColor mixColor(const QColor &a, const QColor &b, qreal t)
{
    t = clampRange(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() * (1.0 - t) + b.redF() * t,
                            a.greenF() * (1.0 - t) + b.greenF() * t,
                            a.blueF() * (1.0 - t) + b.blueF() * t,
                            a.alphaF() * (1.0 - t) + b.alphaF() * t);
}

QColor lighten(const QColor &c, qreal t)
{
    return mixColor(c, QColor(255, 255, 255), t);
}

QColor darken(const QColor &c, qreal t)
{
    return mixColor(c, QColor(12, 12, 22), t);
}

qreal lineWeightBase(const QString &lineWeight)
{
    if (lineWeight == QLatin1String("delicate"))
        return 0.0035;
    if (lineWeight == QLatin1String("bold"))
        return 0.0065;
    return 0.0048;
}

KisAiStrokeOperation makePathOp(const QString &id,
                                const QVector<KisAiStrokePoint> &points,
                                const QColor &color,
                                qreal size,
                                qreal opacity,
                                const QString &layer = QStringLiteral("Lineart"))
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = id;
    op.layer = layer;
    op.points = points;
    op.brush.profile = QStringLiteral("gpen");
    op.brush.color = color;
    op.brush.size = size;
    op.brush.opacity = clampRange(opacity, 0.0, 1.0);
    op.closed = false;
    op.smooth = true;
    op.role = QStringLiteral("face_detail");
    return op;
}

KisAiStrokeOperation makeFillOp(const QString &id,
                                const QPolygonF &polygon,
                                const QColor &color,
                                qreal opacity,
                                const QString &layer = QStringLiteral("Flats"))
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.id = id;
    op.layer = layer;
    op.polygon = polygon;
    op.brush.profile = QStringLiteral("brush");
    op.brush.color = color;
    op.brush.opacity = clampRange(opacity, 0.0, 1.0);
    op.brush.size = 0.03;
    op.fillStyle = QStringLiteral("contour");
    return op;
}

KisAiStrokePoint pt(qreal x, qreal y, qreal pressure = 0.8)
{
    KisAiStrokePoint p;
    p.pos = QPointF(x, y);
    p.pressure = clampRange(pressure, 0.0, 1.0);
    return p;
}

struct EyeAnchor {
    QPointF center;
    qreal width;
    qreal height;
};

// Canonical anime eye placement mirrored around the head axis. Keeping this
// in one function is what guarantees the pair never drifts asymmetrically.
// V6 W1: facing compensation (front-right/front-left shift, profile narrow)
// lives here so every Rig consumer shares identical geometry.
QPair<EyeAnchor, EyeAnchor> eyeAnchors(const KisAiRigParameterSet &params)
{
    qreal spread = params.headWidth * 0.19;
    qreal shift = 0.0;
    const QString facing = params.facing.trimmed().toLower();
    if (facing == QLatin1String("front-right"))
        shift = params.headWidth * 0.03;
    else if (facing == QLatin1String("front-left"))
        shift = -params.headWidth * 0.03;
    else if (facing == QLatin1String("profile"))
        spread = params.headWidth * 0.10;
    const qreal eyeY = params.headCenter.y() + params.headHeight * 0.08;
    const qreal baseW = params.headWidth * 0.17;
    const qreal baseH = params.headWidth * 0.16 * (0.30 + 0.70 * clampRange(params.eyeLeft.aperture, 0.0, 1.0));

    EyeAnchor left;
    left.center = QPointF(params.headCenter.x() - spread + shift, eyeY);
    left.width = baseW;
    left.height = baseH;

    EyeAnchor right = left;
    right.center = QPointF(params.headCenter.x() + spread + shift, eyeY);
    return qMakePair(left, right);
}

// V6 W1: canonical eye pair placement (center + size) with facing
// compensation. Must live at global scope: qualified definitions are
// illegal inside the anonymous namespace above.
} // namespace

QPair<KisAiEyePlacement, KisAiEyePlacement> KisAiRigLibrary::eyePlacements(const KisAiRigParameterSet &params)
{
    KisAiRigParameterSet placed = params;
    qreal spread = placed.headWidth * 0.19;
    qreal shift = 0.0;
    const QString facing = placed.facing.trimmed().toLower();
    if (facing == QLatin1String("front-right"))
        shift = placed.headWidth * 0.03;
    else if (facing == QLatin1String("front-left"))
        shift = -placed.headWidth * 0.03;
    else if (facing == QLatin1String("profile"))
        spread = placed.headWidth * 0.10;
    const qreal eyeY = placed.headCenter.y() + placed.headHeight * 0.08;
    const qreal baseW = placed.headWidth * 0.17;
    const qreal baseH = placed.headWidth * 0.16 * (0.30 + 0.70 * qBound<qreal>(0.0, placed.eyeLeft.aperture, 1.0));
    KisAiEyePlacement left;
    left.center = QPointF(placed.headCenter.x() - spread + shift, eyeY);
    left.size = QSizeF(baseW, baseH);
    KisAiEyePlacement right;
    right.center = QPointF(placed.headCenter.x() + spread + shift, eyeY);
    right.size = QSizeF(baseW, baseH);
    return qMakePair(left, right);
}

void KisAiRigLibrary::applyCameraAdjust(KisAiRigParameterSet &params, const KisAiSceneCameraV2 &camera)
{
    const QString focal = camera.focal.trimmed().toLower();
    if (focal == QLatin1String("short"))
        params.headHeight *= 0.98;
    else if (focal == QLatin1String("long"))
        params.headHeight *= 1.02;
    const QString tilt = camera.tilt.trimmed().toLower();
    if (tilt == QLatin1String("high_angle"))
        params.headCenter.setY(params.headCenter.y() - 0.02);
    else if (tilt == QLatin1String("low_angle"))
        params.headCenter.setY(params.headCenter.y() + 0.02);
}

void KisAiRigLibrary::applyDetailBudget(KisAiRigParameterSet &params, qreal detailLevel)
{
    const qreal detail = qBound<qreal>(0.0, detailLevel, 1.0);
    if (detail < 0.35)
        params.hair.highlightBands = qMin(params.hair.highlightBands, 1);
}

namespace
{
QString eyeExpressionFor(const KisAiRigParameterSet &params)
{
    const QString &expr = params.eyeLeft.expression;
    if (params.eyeLeft.aperture < 0.15 || expr == QLatin1String("closed"))
        return QStringLiteral("closed");
    if (expr == QLatin1String("half"))
        return QStringLiteral("half");
    if (expr == QLatin1String("smile"))
        return QStringLiteral("smile");
    return QStringLiteral("open");
}

QString eyeStyleFor(const KisAiRigParameterSet &params)
{
    const QString &h = params.eyeLeft.highlightShape;
    if (h.isEmpty() || h == QLatin1String("twin_dot"))
        return QStringLiteral("dual_dot");
    if (h == QLatin1String("streak"))
        return QStringLiteral("sparkle");
    if (h == QLatin1String("soft"))
        return QStringLiteral("gradient");
    return QStringLiteral("sparkle");
}

// Upper-lash tapered stroke: inner corner thin, mid thick, outer corner
// tapering out — the single most identity-defining anime line.
QVector<KisAiStrokePoint> lashPath(const EyeAnchor &anchor, bool isRight)
{
    const int steps = 8;
    QVector<KisAiStrokePoint> pts;
    pts.reserve(steps + 1);
    const qreal dir = isRight ? 1.0 : -1.0;
    // Path runs inner corner -> outer corner relative to the face.
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / steps;
        const qreal x = anchor.center.x() - dir * anchor.width * 0.5 + dir * anchor.width * t;
        // Gentle upward arch peaking at t = 0.55.
        const qreal arch = std::sin(M_PI * qBound<qreal>(0.0, t * 0.95 + 0.05, 1.0)) * anchor.height * 0.10;
        const qreal y = anchor.center.y() - anchor.height * 0.42 - arch;
        // Pressure envelope: thick middle, tapered ends (smoothstep'd by renderer).
        const qreal pressure = 0.35 + 0.65 * std::sin(M_PI * t);
        pts.append(pt(x, y, pressure));
    }
    return pts;
}
} // namespace

KisAiRigParameterSet KisAiRigLibrary::parametersFromSpec(const KisAiSceneSpec &spec)
{
    KisAiRigParameterSet p;

    p.headCenter = spec.composition.headCenter;
    p.headHeight = spec.composition.headHeight;
    p.headWidth = spec.composition.headHeight * 0.78; // canonical anime head ratio
    p.facing = spec.subject.facing;

    p.hairColor = spec.head.hairColor;
    p.eyeColor = spec.head.eyeColor;
    p.skinTone = spec.head.skinTone;
    p.lineColor = darken(spec.head.hairColor, 0.55);
    p.lineWeight = spec.style.lineWeight;

    // Expression drives both eye and mouth families consistently.
    QString eyeExpr = QStringLiteral("open");
    QString mouthExpr = QStringLiteral("smile");
    if (spec.head.expression == QLatin1String("smile_open")) {
        eyeExpr = QStringLiteral("smile");
        mouthExpr = QStringLiteral("open_smile");
    } else if (spec.head.expression == QLatin1String("smile_closed")) {
        eyeExpr = QStringLiteral("smile");
        mouthExpr = QStringLiteral("smile");
    } else if (spec.head.expression == QLatin1String("neutral")) {
        eyeExpr = QStringLiteral("open");
        mouthExpr = QStringLiteral("closed_line");
    } else if (spec.head.expression == QLatin1String("half")) {
        eyeExpr = QStringLiteral("half");
        mouthExpr = QStringLiteral("small_open");
    } else if (spec.head.expression == QLatin1String("closed")) {
        eyeExpr = QStringLiteral("closed");
        mouthExpr = QStringLiteral("smile");
    }

    p.eyeLeft.expression = eyeExpr;
    p.eyeRight.expression = eyeExpr;
    p.eyeLeft.gaze = spec.head.gaze;
    p.eyeRight.gaze = spec.head.gaze;
    p.eyeLeft.highlightShape = spec.rig.eyeHighlight;
    p.eyeRight.highlightShape = spec.rig.eyeHighlight;
    p.eyeLeft.aperture = spec.rig.eyeAperture;
    p.eyeRight.aperture = spec.rig.eyeAperture;
    p.eyeLeft.irisRatio = spec.rig.irisRatio;
    p.eyeRight.irisRatio = spec.rig.irisRatio;
    p.eyeLeft.doubleLid = spec.rig.doubleLid;
    p.eyeRight.doubleLid = spec.rig.doubleLid;

    p.brow.enabled = spec.rig.hasBrows;
    p.brow.thicknessScale = 1.0;
    p.brow.angleScale = (eyeExpr == QLatin1String("smile")) ? 1.2 : 1.0;

    p.nose.enabled = true;
    p.nose.shadowStrength = 0.35;

    p.mouth.expression = mouthExpr;
    p.mouth.widthScale = spec.rig.mouthWidthScale;
    p.mouth.highlight = true;

    p.hair.strandDensity = spec.rig.hairStrandDensity;
    p.hair.flyaway = spec.rig.hairFlyaway;
    p.hair.highlightBands = spec.rig.hairHighlightBands;

    // V6 W1: narrative.time resolves the time of day when the light block
    // is still at its default; an explicit light.time stays authoritative.
    QString resolvedTime = spec.light.timeOfDay;
    if ((resolvedTime == QLatin1String("day") || resolvedTime.trimmed().isEmpty())
        && !spec.narrative.time.trimmed().isEmpty())
        resolvedTime = narrativeTimeToTimeOfDay(spec.narrative.time, resolvedTime);
    p.backdrop.timeOfDay = resolvedTime;
    p.backdrop.weather = spec.narrative.weather.isEmpty() ? QStringLiteral("clear") : spec.narrative.weather;
    p.backdrop.props = spec.narrative.props;

    return clamped(p);
}

KisAiRigParameterSet KisAiRigLibrary::clamped(const KisAiRigParameterSet &params)
{
    KisAiRigParameterSet p = params;
    p.headHeight = clampRange(p.headHeight, 0.15, 0.80);
    p.headWidth = clampRange(p.headWidth, p.headHeight * 0.5, p.headHeight * 1.2);
    p.headCenter.setX(clampRange(p.headCenter.x(), 0.2, 0.8));
    p.headCenter.setY(clampRange(p.headCenter.y(), 0.15, 0.7));
    // ポーズ量も不変条件内に収める (ヘッダの [-15,15] / [-0.08,0.08] / [-0.10,0.10])。
    // 現在の parametersFromSpec は 0 を入れるが、将来の patch 経路が値を
    // 設定しても不変条件を破らないようここで clamp する。
    p.headTiltDeg = clampRange(p.headTiltDeg, -15.0, 15.0);
    p.shoulderSlope = clampRange(p.shoulderSlope, -0.08, 0.08);
    p.torsoTurn = clampRange(p.torsoTurn, -0.10, 0.10);

    for (KisAiEyeRigParams *eye : {&p.eyeLeft, &p.eyeRight}) {
        eye->aperture = clampRange(eye->aperture, 0.0, 1.0);
        eye->irisRatio = clampRange(eye->irisRatio, 0.35, 0.85);
        if (eye->highlightShape.isEmpty())
            eye->highlightShape = QStringLiteral("twin_dot");
    }
    p.brow.thicknessScale = clampRange(p.brow.thicknessScale, 0.5, 1.6);
    p.brow.angleScale = clampRange(p.brow.angleScale, 0.4, 1.6);
    p.nose.shadowStrength = clampRange(p.nose.shadowStrength, 0.1, 0.6);
    p.mouth.widthScale = clampRange(p.mouth.widthScale, 0.6, 1.4);
    p.hair.strandDensity = clampRange(p.hair.strandDensity, 0.0, 1.0);
    p.hair.flyaway = clampRange(p.hair.flyaway, 0.0, 1.0);
    p.hair.highlightBands = qBound(0, p.hair.highlightBands, 3);
    return p;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::eyePairOps(const KisAiRigParameterSet &params, quint32 seed)
{
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;
    const auto anchors = eyeAnchors(params);
    const QString expr = eyeExpressionFor(params);
    const QColor lashColor = darken(params.hairColor, 0.65);
    const qreal lashSize = lineWeightBase(params.lineWeight) * 1.25;

    const struct {
        const EyeAnchor &anchor;
        bool isRight;
        const char *side;
    } eyes[2] = {
        {anchors.first, false, "l"},
        {anchors.second, true, "r"},
    };

    for (const auto &eye : eyes) {
        const QString side = QString::fromLatin1(eye.side);

        if (expr == QLatin1String("closed")) {
            // Single gentle downward arc — no iris, no highlight.
            QVector<KisAiStrokePoint> pts;
            const int steps = 6;
            for (int i = 0; i <= steps; ++i) {
                const qreal t = qreal(i) / steps;
                const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.5 + eye.anchor.width * t;
                const qreal y = eye.anchor.center.y() + std::sin(M_PI * t) * eye.anchor.height * 0.35;
                pts.append(pt(x, y, 0.4 + 0.5 * std::sin(M_PI * t)));
            }
            ops.append(makePathOp(QStringLiteral("rig_eye_%1_closed").arg(side), pts, lashColor, lashSize, 0.95));
            continue;
        }

        // Open / smile / half eye: procedural AnimeEye assembly.
        KisAiStrokeOperation eyeOp;
        eyeOp.kind = KisAiStrokeOperation::Kind::AnimeEye;
        eyeOp.id = QStringLiteral("rig_eye_%1").arg(side);
        eyeOp.layer = QStringLiteral("Flats");
        eyeOp.eyeCenter = eye.anchor.center;
        eyeOp.eyeSize = QSizeF(eye.anchor.width, eye.anchor.height);
        eyeOp.eyeIrisColor = params.eyeColor;
        eyeOp.eyeSecondaryColor = lighten(params.eyeColor, 0.45);
        eyeOp.eyeStyle = eyeStyleFor(params);
        eyeOp.eyeExpression = expr;
        eyeOp.eyeIsRight = eye.isRight;
        eyeOp.brush.opacity = 1.0;
        ops.append(eyeOp);

        // Upper lash: the identity line. Painted above the assembly.
        ops.append(makePathOp(QStringLiteral("rig_eye_%1_lash").arg(side),
                              lashPath(eye.anchor, eye.isRight),
                              lashColor,
                              lashSize,
                              1.0));
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::eyePairLineartOps(const KisAiRigParameterSet &params, quint32 seed)
{
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;
    const auto anchors = eyeAnchors(params);
    const QString expr = eyeExpressionFor(params);
    const QColor inkColor(20, 18, 28);
    const qreal baseSize = lineWeightBase(params.lineWeight);
    const qreal lashSize = baseSize * 1.35;
    const qreal irisSize = baseSize * 0.70;
    const qreal detailSize = baseSize * 0.50;

    const struct {
        const EyeAnchor &anchor;
        bool isRight;
        const char *side;
    } eyes[2] = {
        {anchors.first, false, "l"},
        {anchors.second, true, "r"},
    };

    for (const auto &eye : eyes) {
        const QString side = QString::fromLatin1(eye.side);
        const qreal dir = eye.isRight ? 1.0 : -1.0;
        const QPointF c = eye.anchor.center;
        const qreal ew = eye.anchor.width;
        const qreal eh = eye.anchor.height;

        if (expr == QLatin1String("closed")) {
            // Closed eye: gentle curving arc with tapered lashes
            QVector<KisAiStrokePoint> pts;
            const int steps = 8;
            for (int i = 0; i <= steps; ++i) {
                const qreal t = qreal(i) / steps;
                const qreal x = c.x() - dir * ew * 0.5 + dir * ew * t;
                const qreal y = c.y() + std::sin(M_PI * t) * eh * 0.35;
                pts.append(pt(x, y, 0.4 + 0.6 * std::sin(M_PI * t)));
            }
            ops.append(makePathOp(QStringLiteral("lineart_eye_%1_closed").arg(side), pts, inkColor, lashSize, 1.0));
            continue;
        }

        // 1. Upper Lash Main Arch (bold hero line)
        ops.append(makePathOp(QStringLiteral("lineart_eye_%1_lash").arg(side),
                              lashPath(eye.anchor, eye.isRight),
                              inkColor,
                              lashSize,
                              1.0));

        // 2. Outer lash flick / wing (flutter at eye corner)
        {
            QVector<KisAiStrokePoint> wing;
            const qreal startX = c.x() + dir * ew * 0.45;
            const qreal startY = c.y() - eh * 0.38;
            wing.append(pt(startX, startY, 0.9));
            wing.append(pt(startX + dir * ew * 0.12, startY - eh * 0.15, 0.4));
            wing.append(pt(startX + dir * ew * 0.18, startY - eh * 0.22, 0.15));
            ops.append(makePathOp(QStringLiteral("lineart_eye_%1_wing").arg(side), wing, inkColor, lashSize * 0.9, 1.0));
        }

        // 3. Double eyelid crease line (delicate parallel arch)
        if (eye.isRight ? params.eyeRight.doubleLid : params.eyeLeft.doubleLid) {
            QVector<KisAiStrokePoint> lidPts;
            const int steps = 6;
            for (int i = 0; i <= steps; ++i) {
                const qreal t = qreal(i) / steps;
                const qreal x = c.x() - dir * ew * 0.38 + dir * ew * 0.76 * t;
                const qreal y = c.y() - eh * 0.68 - std::sin(M_PI * t) * eh * 0.14;
                lidPts.append(pt(x, y, 0.3 + 0.4 * std::sin(M_PI * t)));
            }
            ops.append(makePathOp(QStringLiteral("lineart_eye_%1_lid").arg(side), lidPts, inkColor, detailSize * 1.1, 0.85));
        }

        // 4. Iris Outer Contour Line (uncolored, beautiful open-top ellipse arc)
        {
            QVector<KisAiStrokePoint> irisPts;
            const int steps = 14;
            const qreal radX = ew * 0.32;
            const qreal radY = eh * 0.44;
            const QPointF irisC(c.x(), c.y() - eh * 0.02);
            for (int i = 0; i <= steps; ++i) {
                const qreal t = M_PI * (0.08 + 0.84 * qreal(i) / steps);
                const qreal px = irisC.x() + std::cos(t) * radX;
                const qreal py = irisC.y() + std::sin(t) * radY;
                const qreal p = 0.5 + 0.5 * std::sin(M_PI * qreal(i) / steps);
                irisPts.append(pt(px, py, p));
            }
            ops.append(makePathOp(QStringLiteral("lineart_eye_%1_iris_contour").arg(side), irisPts, inkColor, irisSize, 1.0));
        }

        // 5. Pupil (dark inking core circle)
        {
            QVector<KisAiStrokePoint> pupilPts;
            const QPointF pupilC(c.x(), c.y() - eh * 0.10);
            const qreal prX = ew * 0.09;
            const qreal prY = eh * 0.13;
            const int steps = 10;
            for (int i = 0; i <= steps; ++i) {
                const qreal angle = 2.0 * M_PI * qreal(i) / steps;
                pupilPts.append(pt(pupilC.x() + std::cos(angle) * prX, pupilC.y() + std::sin(angle) * prY, 0.7));
            }
            KisAiStrokeOperation pupilOp = makePathOp(QStringLiteral("lineart_eye_%1_pupil").arg(side), pupilPts, inkColor, detailSize * 1.2, 1.0);
            pupilOp.closed = true;
            ops.append(pupilOp);
        }

        // 6. Catchlight Highlight Outline Ring (unfilled circular boundary - coloring book ready)
        {
            QVector<KisAiStrokePoint> catchPts;
            const QPointF catchC(c.x() - dir * ew * 0.10, c.y() - eh * 0.15);
            const qreal crX = ew * 0.06;
            const qreal crY = eh * 0.07;
            const int steps = 8;
            for (int i = 0; i <= steps; ++i) {
                const qreal angle = 2.0 * M_PI * qreal(i) / steps;
                catchPts.append(pt(catchC.x() + std::cos(angle) * crX, catchC.y() + std::sin(angle) * crY, 0.5));
            }
            KisAiStrokeOperation catchOp = makePathOp(QStringLiteral("lineart_eye_%1_catch").arg(side), catchPts, inkColor, detailSize * 0.85, 0.9);
            catchOp.closed = true;
            ops.append(catchOp);
        }

        // 7. Iris hatching lines (manga inking striations under pupil)
        {
            const int striations = 4;
            for (int i = 0; i < striations; ++i) {
                const qreal t = qreal(i) / qreal(striations - 1);
                const qreal x = c.x() - ew * 0.15 + ew * 0.30 * t;
                QVector<KisAiStrokePoint> hatch;
                hatch.append(pt(x, c.y() + eh * 0.05, 0.2));
                hatch.append(pt(x, c.y() + eh * 0.28, 0.45));
                ops.append(makePathOp(QStringLiteral("lineart_eye_%1_hatch_%2").arg(side).arg(i), hatch, inkColor, detailSize * 0.7, 0.65));
            }
        }

        // 8. Lower lash rim / tick strokes
        {
            QVector<KisAiStrokePoint> lowerLash;
            const qreal lx = c.x() + dir * ew * 0.15;
            const qreal ly = c.y() + eh * 0.48;
            lowerLash.append(pt(lx - ew * 0.15, ly, 0.2));
            lowerLash.append(pt(lx, ly + eh * 0.02, 0.5));
            lowerLash.append(pt(lx + ew * 0.15, ly - eh * 0.02, 0.2));
            ops.append(makePathOp(QStringLiteral("lineart_eye_%1_lower").arg(side), lowerLash, inkColor, detailSize, 0.85));
        }
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::doubleLidOps(const KisAiRigParameterSet &params)
{
    QVector<KisAiStrokeOperation> ops;
    if (!params.eyeLeft.doubleLid && !params.eyeRight.doubleLid)
        return ops;

    const auto anchors = eyeAnchors(params);
    const QColor creaseColor = darken(params.hairColor, 0.45);
    const qreal creaseSize = lineWeightBase(params.lineWeight) * 0.7;

    const struct {
        const EyeAnchor &anchor;
        const char *side;
    } eyes[2] = {
        {anchors.first, "l"},
        {anchors.second, "r"},
    };

    for (const auto &eye : eyes) {
        const bool hasLid = (std::strcmp(eye.side, "l") == 0) ? params.eyeLeft.doubleLid : params.eyeRight.doubleLid;
        if (!hasLid)
            continue;
        QVector<KisAiStrokePoint> pts;
        const int steps = 5;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.42 + eye.anchor.width * 0.84 * t;
            const qreal y =
                eye.anchor.center.y() - eye.anchor.height * 0.72 - std::sin(M_PI * t) * eye.anchor.height * 0.12;
            pts.append(pt(x, y, 0.3 + 0.35 * std::sin(M_PI * t)));
        }
        ops.append(makePathOp(QStringLiteral("rig_eye_%1_lid").arg(eye.side), pts, creaseColor, creaseSize, 0.55));
    }
    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::browOps(const KisAiRigParameterSet &params)
{
    QVector<KisAiStrokeOperation> ops;
    if (!params.brow.enabled)
        return ops;

    const auto anchors = eyeAnchors(params);
    const QColor browColor = darken(params.hairColor, 0.50);
    const qreal browSize = lineWeightBase(params.lineWeight) * 1.1 * params.brow.thicknessScale;
    const qreal lift = 0.30 * params.brow.angleScale; // arch expressiveness

    const struct {
        const EyeAnchor &anchor;
        const char *side;
    } eyes[2] = {
        {anchors.first, "l"},
        {anchors.second, "r"},
    };

    for (const auto &eye : eyes) {
        QVector<KisAiStrokePoint> pts;
        const int steps = 5;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.55 + eye.anchor.width * 1.10 * t;
            const qreal y = eye.anchor.center.y() - eye.anchor.height * (1.15 + lift * std::sin(M_PI * t));
            pts.append(pt(x, y, 0.35 + 0.6 * std::sin(M_PI * t)));
        }
        ops.append(makePathOp(QStringLiteral("rig_brow_%1").arg(eye.side), pts, browColor, browSize, 0.9));
    }
    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::noseOps(const KisAiRigParameterSet &params)
{
    QVector<KisAiStrokeOperation> ops;
    if (!params.nose.enabled)
        return ops;

    const qreal noseY = params.headCenter.y() + params.headHeight * 0.18;
    const qreal noseX = params.headCenter.x();
    const qreal s = params.headWidth * 0.012;
    const QColor shadowColor = mixColor(params.skinTone, QColor(150, 90, 80), params.nose.shadowStrength);

    // 1. Delicate nasal bridge highlight
    {
        QPolygonF bridgePoly;
        const qreal bhTop = noseY - s * 3.5;
        const qreal bhBot = noseY - s * 0.8;
        const qreal bhW = s * 0.55;
        bridgePoly.append(QPointF(noseX - bhW * 0.5, bhTop));
        bridgePoly.append(QPointF(noseX + bhW * 0.5, bhTop));
        bridgePoly.append(QPointF(noseX + bhW * 0.8, bhBot));
        bridgePoly.append(QPointF(noseX - bhW * 0.8, bhBot));
        KisAiStrokeOperation hlOp;
        hlOp.kind = KisAiStrokeOperation::Kind::Fill;
        hlOp.id = QStringLiteral("rig_nose_bridge_highlight");
        hlOp.groupId = QStringLiteral("nose");
        hlOp.role = QStringLiteral("highlight");
        hlOp.layer = QStringLiteral("Highlights");
        hlOp.polygon = bridgePoly;
        hlOp.brush.color = QColor(255, 255, 255);
        hlOp.brush.opacity = 0.40;
        hlOp.brush.profile = QStringLiteral("airbrush");
        hlOp.fillStyle = QStringLiteral("wash");
        hlOp.blendMode = QStringLiteral("screen");
        ops.append(hlOp);
    }

    // 2. Soft warm tone cast shadow wash under nose tip
    {
        QPolygonF shadowPoly;
        shadowPoly.append(QPointF(noseX, noseY));
        shadowPoly.append(QPointF(noseX + s * 2.2, noseY + s * 0.6));
        shadowPoly.append(QPointF(noseX + s * 1.5, noseY + s * 1.6));
        shadowPoly.append(QPointF(noseX - s * 0.4, noseY + s * 0.8));
        KisAiStrokeOperation shOp;
        shOp.kind = KisAiStrokeOperation::Kind::Fill;
        shOp.id = QStringLiteral("rig_nose_shadow");
        shOp.groupId = QStringLiteral("nose");
        shOp.role = QStringLiteral("shadow");
        shOp.layer = QStringLiteral("Shading");
        shOp.polygon = shadowPoly;
        shOp.brush.color = shadowColor;
        shOp.brush.opacity = qBound<qreal>(0.20, params.nose.shadowStrength * 0.45, 0.50);
        shOp.brush.profile = QStringLiteral("watercolor");
        shOp.fillStyle = QStringLiteral("wash");
        ops.append(shOp);
    }

    // 3. Delicate pressure-tapered tip mark
    {
        QVector<KisAiStrokePoint> pts;
        pts.append(pt(noseX - s * 0.35, noseY + s * 0.05, 0.30));
        pts.append(pt(noseX, noseY + s * 0.35, 0.85));
        pts.append(pt(noseX + s * 0.75, noseY + s * 0.20, 0.30));
        ops.append(makePathOp(QStringLiteral("rig_nose_point"),
                              pts,
                              mixColor(params.skinTone, QColor(115, 60, 52), 0.50),
                              lineWeightBase(params.lineWeight) * 0.75,
                              0.85));
    }
    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::mouthOps(const KisAiRigParameterSet &params)
{
    QVector<KisAiStrokeOperation> ops;

    KisAiStrokeOperation mouth;
    mouth.kind = KisAiStrokeOperation::Kind::AnimeMouth;
    mouth.id = QStringLiteral("rig_mouth");
    mouth.layer = QStringLiteral("Flats");
    mouth.mouthCenter = QPointF(params.headCenter.x(), params.headCenter.y() + params.headHeight * 0.30);
    mouth.mouthSize = QSizeF(params.headWidth * 0.11 * params.mouth.widthScale, params.headWidth * 0.05);
    mouth.mouthExpression = params.mouth.expression;
    mouth.mouthLipColor = mixColor(params.skinTone, QColor(224, 117, 125), 0.55);
    mouth.mouthHasHighlight = params.mouth.highlight;
    mouth.brush.opacity = 1.0;
    ops.append(mouth);
    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::hairHighlightOps(const KisAiRigParameterSet &params, quint32 seed)
{
    QVector<KisAiStrokeOperation> ops;
    const int bands = qBound(0, params.hair.highlightBands, 3);
    if (bands <= 0)
        return ops;

    const qreal topY = params.headCenter.y() - params.headHeight * 0.50;
    const qreal bandScale = params.headWidth * 0.05 * (0.6 + 0.4 * params.hair.strandDensity);
    const QColor mainBand = lighten(params.hairColor, 0.62);
    const QColor subBand = lighten(params.hairColor, 0.40);
    const QColor counterBand = lighten(params.hairColor, 0.75);

    struct BandDef {
        QColor color;
        qreal yBase; // relative to topY in headHeight units
        qreal xShift; // relative to headWidth
        qreal widthScale;
        qreal opacity;
    };

    QVector<BandDef> defs;
    defs.append({mainBand, 0.12, 0.02, 1.0, 0.60});
    if (bands >= 2)
        defs.append({subBand, 0.22, -0.16, 0.6, 0.40});
    if (bands >= 3)
        defs.append({counterBand, 0.07, -0.22, 0.45, 0.35});

    int index = 0;
    for (const BandDef &def : defs) {
        // Each band is a screen-blended ribbon following the dome curvature.
        QVector<QPointF> spine;
        const int steps = 7;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = params.headCenter.x() + (t - 0.5) * params.headWidth * 0.72 + def.xShift * params.headWidth;
            const qreal y = topY + params.headHeight * def.yBase + std::sin(M_PI * t) * params.headHeight * 0.045;
            spine.append(QPointF(x, y));
        }

        KisAiStrokeOperation band;
        band.kind = KisAiStrokeOperation::Kind::Ribbon;
        band.id = QStringLiteral("rig_hair_band_%1").arg(index++);
        band.layer = QStringLiteral("Highlights");
        band.spine = spine;
        band.widthStart = bandScale * 0.25 * def.widthScale;
        band.widthMid = bandScale * def.widthScale;
        band.widthEnd = bandScale * 0.15 * def.widthScale;
        band.brush.profile = QStringLiteral("airbrush");
        band.brush.color = def.color;
        band.brush.opacity = def.opacity;
        band.blendMode = QStringLiteral("screen");
        ops.append(band);
    }

    Q_UNUSED(seed);
    return ops;
}

QString KisAiRigLibrary::narrativeTimeToTimeOfDay(const QString &narrativeTime, const QString &fallback)
{
    const QString t = narrativeTime.trimmed().toLower();
    if (t.isEmpty())
        return fallback;
    if (t.contains(QLatin1String("golden")) || t.contains(QLatin1String("sunset")) || t.contains(QLatin1String("dusk"))
        || t.contains(QLatin1String("evening")))
        return QStringLiteral("sunset");
    if (t.contains(QLatin1String("night")) || t.contains(QLatin1String("midnight")) || t.contains(QLatin1String("moon"))
        || t.contains(QLatin1String("star")))
        return QStringLiteral("night");
    if (t.contains(QLatin1String("morning")) || t.contains(QLatin1String("noon")) || t.contains(QLatin1String("day")))
        return QStringLiteral("day");
    return fallback;
}

QVector<KisAiStrokeOperation>
KisAiRigLibrary::backdropWeatherOps(const KisAiRigParameterSet &params, const QSize &canvasSize, quint32 seed)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops;
    if (seed == 0)
        seed = 42;

    const QString weather = params.backdrop.weather;
    const QString tod = params.backdrop.timeOfDay;

    // Face box that weather effects must never enter.
    const QRectF faceBox(params.headCenter.x() - params.headWidth * 0.85,
                         params.headCenter.y() - params.headHeight * 0.70,
                         params.headWidth * 1.7,
                         params.headHeight * 2.1);

    if (weather == QLatin1String("rain")) {
        KisAiStrokeOperation rain;
        rain.kind = KisAiStrokeOperation::Kind::Particles;
        rain.id = QStringLiteral("rig_weather_rain");
        rain.layer = QStringLiteral("FX");
        rain.particleShape = QStringLiteral("dot");
        rain.particleCount = 42;
        // Keep the band clear of the face: left/right side slivers.
        rain.bounds = QRectF(0.0, 0.0, 1.0, 1.0);
        rain.brush.profile = QStringLiteral("marker");
        rain.brush.color = QColor(190, 210, 235, 90);
        rain.brush.opacity = 0.5;
        rain.angleDeg = 18.0;
        ops.append(rain);
    } else if (weather == QLatin1String("snow")) {
        KisAiStrokeOperation snow;
        snow.kind = KisAiStrokeOperation::Kind::Particles;
        snow.id = QStringLiteral("rig_weather_snow");
        snow.layer = QStringLiteral("FX");
        snow.particleShape = QStringLiteral("dot");
        snow.particleCount = 28;
        snow.bounds = QRectF(0.0, 0.0, 1.0, 1.0);
        snow.brush.profile = QStringLiteral("airbrush");
        snow.brush.color = QColor(255, 255, 255, 170);
        snow.brush.opacity = 0.7;
        ops.append(snow);
    } else if (weather == QLatin1String("cloudy")) {
        // Two soft cloud masses in the upper corners, away from the face box.
        for (int side = 0; side < 2; ++side) {
            QPolygonF cloud;
            const qreal cx = side == 0 ? 0.16 : 0.84;
            const qreal cy = 0.14;
            const qreal r = 0.11;
            for (int i = 0; i < 12; ++i) {
                const qreal t = 2.0 * M_PI * i / 12.0;
                const qreal wob = 0.75 + 0.25 * std::sin(3.0 * t + side);
                cloud.append(QPointF(cx + std::cos(t) * r * wob, cy + std::sin(t) * r * 0.55 * wob));
            }
            const QColor cloudColor = tod == QLatin1String("night") ? QColor(70, 78, 100) : QColor(235, 238, 245);
            ops.append(makeFillOp(QStringLiteral("rig_cloud_%1").arg(side),
                                  cloud,
                                  cloudColor,
                                  0.7,
                                  QStringLiteral("Background")));
        }
    }

    // Prop slots (deterministic dictionary; unknown props are ignored).
    for (const QString &rawProp : params.backdrop.props) {
        const QString prop = rawProp.trimmed().toLower();
        if (prop == QLatin1String("lamp") || prop == QLatin1String("lantern")) {
            KisAiStrokeOperation glow;
            glow.kind = KisAiStrokeOperation::Kind::GradientFill;
            glow.id = QStringLiteral("rig_prop_lamp_glow");
            glow.layer = QStringLiteral("FX");
            glow.isRadial = true;
            const qreal gx = params.headCenter.x() > 0.5 ? 0.14 : 0.86;
            glow.gradientCenter = QPointF(gx, 0.62);
            glow.gradientRadius = 0.10;
            glow.gradientColors = {QColor(255, 196, 110, 160), QColor(255, 196, 110, 0)};
            glow.brush.opacity = 0.8;
            glow.blendMode = QStringLiteral("screen");
            ops.append(glow);
        } else if (prop == QLatin1String("stars") || prop == QLatin1String("star")) {
            KisAiStrokeOperation stars;
            stars.kind = KisAiStrokeOperation::Kind::Particles;
            stars.id = QStringLiteral("rig_prop_stars");
            stars.layer = QStringLiteral("FX");
            stars.particleShape = QStringLiteral("star");
            stars.particleCount = 18;
            stars.bounds = QRectF(0.0, 0.0, 1.0, 0.42); // sky band only
            stars.brush.profile = QStringLiteral("neon");
            stars.brush.color = QColor(255, 250, 220, 200);
            stars.brush.opacity = 0.8;
            ops.append(stars);
        } else if (prop == QLatin1String("petals") || prop == QLatin1String("sakura")) {
            KisAiStrokeOperation petals;
            petals.kind = KisAiStrokeOperation::Kind::Particles;
            petals.id = QStringLiteral("rig_prop_petals");
            petals.layer = QStringLiteral("FX");
            petals.particleShape = QStringLiteral("petal");
            petals.particleCount = 14;
            petals.bounds = QRectF(0.0, 0.0, 1.0, 1.0);
            petals.brush.profile = QStringLiteral("watercolor");
            petals.brush.color = QColor(255, 183, 197, 190);
            petals.brush.opacity = 0.75;
            ops.append(petals);
        }
    }

    // Guard: particles must not sit on the face (negative.noParticlesOnFace
    // equivalent enforced at rig level, not left to LLM honesty).
    for (KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::Particles && op.bounds.isValid() && op.bounds.intersects(faceBox)) {
            // Shrink to the largest side band outside the face box.
            const qreal leftWidth = faceBox.left();
            const qreal rightWidth = 1.0 - faceBox.right();
            if (rightWidth >= leftWidth && rightWidth > 0.05)
                op.bounds = QRectF(faceBox.right(), 0.0, rightWidth, op.bounds.height());
            else if (leftWidth > 0.05)
                op.bounds = QRectF(0.0, 0.0, leftWidth, op.bounds.height());
            else
                op.bounds = QRectF(0.0, 0.0, 0.0, 0.0); // no safe band: skip
        }
    }

    return ops;
}

quint32 KisAiRigLibrary::partSeed(const QString &partName, quint32 baseSeed)
{
    return KisAiStrokeProgramCodec::stableSeed(partName) ^ baseSeed;
}

QVector<KisAiStrokeOperation>
KisAiRigLibrary::mountainOps(const KisAiSceneSpec &spec, const QSize &canvasSize, quint32 seed)
{
    Q_UNUSED(canvasSize);
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;
    const QString tod = spec.light.timeOfDay;

    // 1. Exponential ridge silhouette for elegant volcanic peak (Mount Fuji profile)
    const qreal xc = 0.50;
    const qreal yTop = 0.26;
    const qreal yBase = 0.64;
    const qreal halfSpan = 0.44;
    const qreal kDecay = 3.6;

    QPolygonF mountainPoly;
    mountainPoly.append(QPointF(xc - halfSpan, yBase));

    // Left slope (rising to peak)
    const int slopeSteps = 24;
    for (int i = 0; i <= slopeSteps; ++i) {
        const qreal t = qreal(i) / slopeSteps;
        const qreal x = (xc - halfSpan) + t * halfSpan;
        const qreal dist = (xc - x) / halfSpan;
        const qreal y = yBase - (yBase - yTop) * std::exp(-kDecay * dist);
        mountainPoly.append(QPointF(x, y));
    }

    // Right slope (descending to base)
    for (int i = 1; i <= slopeSteps; ++i) {
        const qreal t = qreal(i) / slopeSteps;
        const qreal x = xc + t * halfSpan;
        const qreal dist = (x - xc) / halfSpan;
        const qreal y = yBase - (yBase - yTop) * std::exp(-kDecay * dist);
        mountainPoly.append(QPointF(x, y));
    }

    mountainPoly.append(QPointF(xc + halfSpan, yBase));

    // Palette derivation based on time of day
    QColor mountainBody;
    QColor snowColor;
    QColor shadowColor;
    if (tod == QLatin1String("night")) {
        mountainBody = QColor(25, 35, 68);
        snowColor = QColor(180, 195, 225);
        shadowColor = QColor(12, 18, 40, 160);
    } else if (tod == QLatin1String("sunset")) {
        mountainBody = QColor(125, 60, 85); // Red Fuji / warm evening slope
        snowColor = QColor(255, 235, 240); // Alpenglow pink
        shadowColor = QColor(60, 25, 55, 170); // Deep violet shadow
    } else {
        mountainBody = QColor(70, 95, 140);
        snowColor = QColor(250, 252, 255);
        shadowColor = QColor(35, 50, 85, 150);
    }

    // 1. Mountain Base Mass (Flats)
    ops.append(
        makeFillOp(QStringLiteral("rig_mountain_body"), mountainPoly, mountainBody, 1.0, QStringLiteral("Flats")));

    // 2. Snow Cap with serrated fractal snowmelt ridges (Snow Crevices)
    const qreal snowDepth = (yBase - yTop) * 0.38;
    QPolygonF snowPoly;
    snowPoly.append(QPointF(xc, yTop));

    // Snow contour left to right with harmonic crevices
    const int snowSteps = 32;
    for (int i = 0; i <= snowSteps; ++i) {
        const qreal t = qreal(i) / snowSteps;
        const qreal angle = (t - 0.5) * 2.0;
        const qreal x = xc + angle * (halfSpan * 0.46);
        const qreal dist = std::abs(x - xc) / halfSpan;
        const qreal ridgeY = yBase - (yBase - yTop) * std::exp(-kDecay * dist);

        // Harmonic serration creates realistic snowmelt tongue patterns down the ravines
        const qreal harmonic = 0.30 * std::sin(t * 14.0 * M_PI) + 0.15 * std::cos(t * 28.0 * M_PI);
        const qreal ySnow = yTop + snowDepth * (0.80 + 0.35 * (dist * 2.2) + harmonic);
        const qreal clampedY = std::min(ySnow, ridgeY);
        snowPoly.append(QPointF(x, clampedY));
    }

    // Connect along the top ridge to close the snow cap
    for (int i = snowSteps; i >= 0; --i) {
        const qreal t = qreal(i) / snowSteps;
        const qreal angle = (t - 0.5) * 2.0;
        const qreal x = xc + angle * (halfSpan * 0.46);
        const qreal dist = std::abs(x - xc) / halfSpan;
        const qreal ridgeY = yBase - (yBase - yTop) * std::exp(-kDecay * dist);
        snowPoly.append(QPointF(x, ridgeY));
    }

    KisAiStrokeOperation snowOp =
        makeFillOp(QStringLiteral("rig_mountain_snow"), snowPoly, snowColor, 0.96, QStringLiteral("Flats"));
    snowOp.brush.profile = QStringLiteral("watercolor");
    snowOp.fillStyle = QStringLiteral("wash");
    ops.append(snowOp);

    // 3. Facet Shading on the eastern/shaded slope (Shading layer)
    QPolygonF shadeFacet;
    shadeFacet.append(QPointF(xc, yTop));
    for (int i = 0; i <= slopeSteps; ++i) {
        const qreal t = qreal(i) / slopeSteps;
        const qreal x = xc + t * halfSpan;
        const qreal dist = (x - xc) / halfSpan;
        const qreal y = yBase - (yBase - yTop) * std::exp(-kDecay * dist);
        shadeFacet.append(QPointF(x, y));
    }
    shadeFacet.append(QPointF(xc + 0.05, yBase));
    shadeFacet.append(QPointF(xc, yBase));

    KisAiStrokeOperation shadeOp =
        makeFillOp(QStringLiteral("rig_mountain_shade"), shadeFacet, shadowColor, 0.55, QStringLiteral("Shading"));
    shadeOp.blendMode = QStringLiteral("multiply");
    shadeOp.fillStyle = QStringLiteral("wash");
    ops.append(shadeOp);

    // 4. Soft aerial perspective haze at mountain foot (blends seamlessly into atmosphere)
    QPolygonF hazePoly;
    hazePoly.append(QPointF(xc - halfSpan * 1.1, yBase - 0.08));
    hazePoly.append(QPointF(xc + halfSpan * 1.1, yBase - 0.08));
    hazePoly.append(QPointF(xc + halfSpan * 1.1, yBase + 0.04));
    hazePoly.append(QPointF(xc - halfSpan * 1.1, yBase + 0.04));
    QColor hazeCol = tod == QLatin1String("sunset") ? QColor(245, 175, 155, 130) : QColor(210, 225, 245, 130);
    KisAiStrokeOperation hazeOp =
        makeFillOp(QStringLiteral("rig_mountain_haze"), hazePoly, hazeCol, 0.50, QStringLiteral("Flats"));
    hazeOp.brush.profile = QStringLiteral("watercolor");
    hazeOp.fillStyle = QStringLiteral("wash");
    ops.append(hazeOp);

    return ops;
}

QVector<KisAiStrokeOperation>
KisAiRigLibrary::sakuraTreeOps(const KisAiSceneSpec &spec, const QSize &canvasSize, quint32 seed)
{
    Q_UNUSED(canvasSize);
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;

    const QString tod = spec.light.timeOfDay;
    const QColor trunkColor = tod == QLatin1String("sunset") ? QColor(50, 32, 42) : QColor(36, 30, 40);
    const QColor basePetalColor = QColor(255, 185, 205);
    const QColor deepPetalColor = tod == QLatin1String("sunset") ? QColor(190, 80, 115) : QColor(165, 85, 125);
    const QColor highlightPetalColor = QColor(255, 242, 248);

    // 1. Trunk (Ribbon) rooted at bottom right, arcing gracefully toward center/upper-left
    const qreal rootX = 0.82;
    const qreal rootY = 0.95;

    KisAiStrokeOperation trunk;
    trunk.kind = KisAiStrokeOperation::Kind::Ribbon;
    trunk.id = QStringLiteral("rig_sakura_trunk");
    trunk.layer = QStringLiteral("Flats");
    trunk.brush.profile = QStringLiteral("brush");
    trunk.brush.color = trunkColor;
    trunk.widthStart = 0.052;
    trunk.widthMid = 0.034;
    trunk.widthEnd = 0.016;
    trunk.spine << QPointF(rootX, rootY) << QPointF(rootX - 0.06, 0.72) << QPointF(rootX - 0.12, 0.52)
                << QPointF(rootX - 0.16, 0.38);
    ops.append(trunk);

    // Trunk lineart contour for bark definition
    KisAiStrokeOperation trunkLine;
    trunkLine.kind = KisAiStrokeOperation::Kind::Path;
    trunkLine.id = QStringLiteral("rig_sakura_trunk_line");
    trunkLine.layer = QStringLiteral("Lineart");
    trunkLine.brush.profile = QStringLiteral("gpen");
    trunkLine.brush.color = trunkColor.darker(135);
    trunkLine.brush.size = 0.0035;
    trunkLine.points << pt(rootX, rootY, 0.9) << pt(rootX - 0.06, 0.72, 0.8) << pt(rootX - 0.12, 0.52, 0.7)
                     << pt(rootX - 0.16, 0.38, 0.5);
    ops.append(trunkLine);

    // 2. Primary & secondary branches
    struct BranchSpec {
        QPointF start;
        QPointF mid;
        QPointF end;
        qreal widthStart;
        qreal widthEnd;
    };
    const QVector<BranchSpec> branches = {
        {QPointF(rootX - 0.08, 0.64), QPointF(rootX - 0.22, 0.56), QPointF(rootX - 0.38, 0.52), 0.018, 0.007},
        {QPointF(rootX - 0.12, 0.52), QPointF(rootX - 0.28, 0.42), QPointF(rootX - 0.46, 0.40), 0.016, 0.006},
        {QPointF(rootX - 0.14, 0.44), QPointF(rootX - 0.10, 0.32), QPointF(rootX - 0.06, 0.24), 0.014, 0.005},
        {QPointF(rootX - 0.16, 0.38), QPointF(rootX - 0.32, 0.30), QPointF(rootX - 0.52, 0.28), 0.015, 0.005},
        {QPointF(rootX - 0.28, 0.42), QPointF(rootX - 0.36, 0.34), QPointF(rootX - 0.44, 0.30), 0.010, 0.004}};

    for (int b = 0; b < branches.size(); ++b) {
        const auto &bs = branches.at(b);
        KisAiStrokeOperation br;
        br.kind = KisAiStrokeOperation::Kind::Ribbon;
        br.id = QStringLiteral("rig_sakura_branch_%1").arg(b);
        br.layer = QStringLiteral("Flats");
        br.brush.profile = QStringLiteral("brush");
        br.brush.color = trunkColor;
        br.widthStart = bs.widthStart;
        br.widthMid = (bs.widthStart + bs.widthEnd) * 0.55;
        br.widthEnd = bs.widthEnd;
        br.spine << bs.start << bs.mid << bs.end;
        ops.append(br);

        KisAiStrokeOperation brLine;
        brLine.kind = KisAiStrokeOperation::Kind::Path;
        brLine.id = QStringLiteral("rig_sakura_branch_line_%1").arg(b);
        brLine.layer = QStringLiteral("Lineart");
        brLine.brush.profile = QStringLiteral("gpen");
        brLine.brush.color = trunkColor.darker(130);
        brLine.brush.size = 0.0028;
        brLine.points << pt(bs.start.x(), bs.start.y(), 0.7) << pt(bs.mid.x(), bs.mid.y(), 0.6)
                      << pt(bs.end.x(), bs.end.y(), 0.3);
        ops.append(brLine);
    }

    // 3. Volumetric Petal Masses along branch nodes
    const QVector<QPointF> clusterNodes = {QPointF(rootX - 0.38, 0.52),
                                           QPointF(rootX - 0.46, 0.40),
                                           QPointF(rootX - 0.06, 0.24),
                                           QPointF(rootX - 0.52, 0.28),
                                           QPointF(rootX - 0.32, 0.32),
                                           QPointF(rootX - 0.22, 0.36),
                                           QPointF(rootX - 0.18, 0.26),
                                           QPointF(rootX - 0.36, 0.22),
                                           QPointF(rootX - 0.48, 0.20),
                                           QPointF(rootX - 0.12, 0.18)};

    auto makeCloudPoly = [&](const QPointF &center, qreal rx, qreal ry, int seedShift) {
        QPolygonF poly;
        const int verts = 18;
        for (int v = 0; v < verts; ++v) {
            const qreal angle = (2.0 * M_PI * v) / verts;
            const qreal wobble = 0.82 + 0.18 * std::sin(angle * 3.0 + seedShift) + 0.08 * std::cos(angle * 5.0);
            poly.append(
                QPointF(center.x() + std::cos(angle) * rx * wobble, center.y() + std::sin(angle) * ry * wobble));
        }
        return poly;
    };

    for (int i = 0; i < clusterNodes.size(); ++i) {
        const QPointF &node = clusterNodes.at(i);
        const qreal rScale = 0.08 + (i % 3) * 0.02;

        // Tier 1: Deep inner shade
        QPolygonF deepPoly = makeCloudPoly(node + QPointF(0.0, 0.015), rScale * 1.1, rScale * 0.85, i * 3);
        KisAiStrokeOperation deepOp = makeFillOp(QStringLiteral("rig_sakura_deep_%1").arg(i),
                                                 deepPoly,
                                                 deepPetalColor,
                                                 0.75,
                                                 QStringLiteral("Flats"));
        deepOp.brush.profile = QStringLiteral("watercolor");
        deepOp.fillStyle = QStringLiteral("wash");
        ops.append(deepOp);

        // Tier 2: Mid vibrant bloom
        QPolygonF midPoly = makeCloudPoly(node, rScale, rScale * 0.75, i * 5 + 1);
        KisAiStrokeOperation midOp = makeFillOp(QStringLiteral("rig_sakura_mid_%1").arg(i),
                                                midPoly,
                                                basePetalColor,
                                                0.85,
                                                QStringLiteral("Flats"));
        midOp.brush.profile = QStringLiteral("watercolor");
        midOp.fillStyle = QStringLiteral("wash");
        ops.append(midOp);

        // Tier 3: Sunny top rim highlight
        QPolygonF hlPoly = makeCloudPoly(node - QPointF(0.01, 0.015), rScale * 0.72, rScale * 0.55, i * 7 + 2);
        KisAiStrokeOperation hlOp = makeFillOp(QStringLiteral("rig_sakura_hl_%1").arg(i),
                                               hlPoly,
                                               highlightPetalColor,
                                               0.70,
                                               QStringLiteral("Highlights"));
        hlOp.brush.profile = QStringLiteral("airbrush");
        hlOp.blendMode = QStringLiteral("screen");
        ops.append(hlOp);
    }

    // 4. Wind-driven floating petals drifting gracefully across the canvas
    KisAiStrokeOperation petals;
    petals.kind = KisAiStrokeOperation::Kind::Particles;
    petals.id = QStringLiteral("rig_sakura_drifting_petals");
    petals.layer = QStringLiteral("FX");
    petals.bounds = QRectF(0.05, 0.15, 0.90, 0.75);
    petals.particleShape = QStringLiteral("petal");
    petals.particleCount = 28;
    petals.brush.color = highlightPetalColor;
    petals.brush.size = 0.007;
    petals.brush.opacity = 0.80;
    ops.append(petals);

    return ops;
}

QVector<KisAiStrokeOperation>
KisAiRigLibrary::waterSurfaceOps(const KisAiSceneSpec &spec, const QSize &canvasSize, qreal horizonY, quint32 seed)
{
    Q_UNUSED(canvasSize);
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;
    const QString tod = spec.light.timeOfDay;

    // 1. Water Base Gradient (Fresnel depth: distant horizon reflects sky, near foreground shows deep water tone)
    QColor waterHorizon;
    QColor waterForeground;
    if (tod == QLatin1String("night")) {
        waterHorizon = QColor(32, 42, 75);
        waterForeground = QColor(10, 15, 32);
    } else if (tod == QLatin1String("sunset")) {
        waterHorizon = QColor(220, 130, 110); // Glowing reflection of sunset
        waterForeground = QColor(65, 35, 70); // Deep violet water depth
    } else {
        waterHorizon = QColor(160, 195, 230);
        waterForeground = QColor(40, 75, 120);
    }

    KisAiStrokeOperation waterWash;
    waterWash.kind = KisAiStrokeOperation::Kind::GradientFill;
    waterWash.id = QStringLiteral("rig_water_wash");
    waterWash.layer = QStringLiteral("Flats");
    waterWash.polygon = {QPointF(0.0, horizonY), QPointF(1.0, horizonY), QPointF(1.0, 1.0), QPointF(0.0, 1.0)};
    waterWash.gradientColors = {waterHorizon, waterForeground};
    waterWash.angleDeg = 90.0;
    waterWash.brush.profile = QStringLiteral("watercolor");
    waterWash.brush.color = waterHorizon;
    waterWash.brush.opacity = 1.0;
    waterWash.fillStyle = QStringLiteral("directional");
    ops.append(waterWash);

    // 2. Inverted Soft Mirror Reflection of Mountain / Sky (Water watercolor wash)
    const qreal xc = 0.50;
    const qreal reflSpan = 0.40;
    const qreal reflHeight = 0.22;
    QPolygonF reflPoly;
    reflPoly.append(QPointF(xc - reflSpan, horizonY));
    reflPoly.append(QPointF(xc, horizonY + reflHeight));
    reflPoly.append(QPointF(xc + reflSpan, horizonY));
    QColor reflColor = tod == QLatin1String("sunset") ? QColor(145, 65, 90, 120) : QColor(45, 65, 105, 110);
    KisAiStrokeOperation reflOp =
        makeFillOp(QStringLiteral("rig_water_mountain_refl"), reflPoly, reflColor, 0.40, QStringLiteral("Flats"));
    reflOp.brush.profile = QStringLiteral("watercolor");
    reflOp.fillStyle = QStringLiteral("wash");
    ops.append(reflOp);

    // 3. Perspective-spaced ripple specular lines
    const int rippleCount = 6;
    const QColor rippleColor = tod == QLatin1String("sunset") ? QColor(255, 215, 185, 160) : QColor(230, 245, 255, 150);

    for (int r = 0; r < rippleCount; ++r) {
        const qreal t = qreal(r + 1) / (rippleCount + 1);
        const qreal ry = horizonY + (1.0 - horizonY) * (t * t * 0.85 + 0.05);
        const qreal halfW = 0.20 + t * 0.28;
        const qreal strokeW = 0.0018 + t * 0.0035;

        QVector<KisAiStrokePoint> ripplePts;
        ripplePts << pt(xc - halfW, ry, 0.2) << pt(xc - halfW * 0.5, ry, 0.8) << pt(xc, ry, 0.9)
                  << pt(xc + halfW * 0.5, ry, 0.8) << pt(xc + halfW, ry, 0.2);

        KisAiStrokeOperation ripOp;
        ripOp.kind = KisAiStrokeOperation::Kind::Path;
        ripOp.id = QStringLiteral("rig_water_ripple_%1").arg(r);
        ripOp.layer = QStringLiteral("Highlights");
        ripOp.points = ripplePts;
        ripOp.smooth = true;
        ripOp.brush.profile = QStringLiteral("airbrush");
        ripOp.brush.color = rippleColor;
        ripOp.brush.size = strokeW;
        ripOp.brush.opacity = 0.65;
        ripOp.blendMode = QStringLiteral("screen");
        ops.append(ripOp);
    }

    return ops;
}

// =========================================================================
// V7 Bezier Head Outline & Dynamic Pose Anatomic Geometry
// =========================================================================

QPolygonF
KisAiRigLibrary::headOutlineBezier(const QPointF &headCenter, qreal headWidth, qreal headHeight, qreal tiltDeg)
{
    QPolygonF poly;
    const qreal hw = headWidth * 0.5;
    const qreal hh = headHeight * 0.5;
    const qreal rad = tiltDeg * M_PI / 180.0;
    const qreal cosR = std::cos(rad);
    const qreal sinR = std::sin(rad);

    const auto rotatePoint = [&](const QPointF &p) -> QPointF {
        if (std::abs(tiltDeg) < 1e-4)
            return p;
        const qreal dx = p.x() - headCenter.x();
        const qreal dy = p.y() - headCenter.y();
        return QPointF(headCenter.x() + dx * cosR - dy * sinR, headCenter.y() + dx * sinR + dy * cosR);
    };

    // 1. Cranium Top Dome (from right ear temple to left ear temple over crown)
    constexpr int craniumSteps = 16;
    for (int i = 0; i <= craniumSteps; ++i) {
        const qreal t = M_PI * (qreal(i) / craniumSteps);
        const qreal x = headCenter.x() + std::cos(t) * (hw * 1.02);
        const qreal y = headCenter.y() - std::sin(t) * (hh * 0.98);
        poly.append(rotatePoint(QPointF(x, y)));
    }

    // Cubic Bezier evaluator
    const auto evalCubic =
        [](const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t) -> QPointF {
        const qreal it = 1.0 - t;
        return it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
    };

    // 2. Left Cheek & Jaw: from left temple -> cheek curve -> chin
    const QPointF leftTemple(headCenter.x() - hw * 1.02, headCenter.y());
    const QPointF leftCheekCtrl(headCenter.x() - hw * 1.04, headCenter.y() + hh * 0.38);
    const QPointF leftJawCtrl(headCenter.x() - hw * 0.62, headCenter.y() + hh * 0.85);
    const QPointF chinLeft(headCenter.x() - hw * 0.14, headCenter.y() + hh * 1.00);

    constexpr int jawSteps = 10;
    for (int i = 1; i <= jawSteps; ++i) {
        const qreal t = qreal(i) / jawSteps;
        poly.append(rotatePoint(evalCubic(leftTemple, leftCheekCtrl, leftJawCtrl, chinLeft, t)));
    }

    // 3. Refined Chin Arc
    const QPointF chinTip(headCenter.x(), headCenter.y() + hh * 1.015);
    const QPointF chinRight(headCenter.x() + hw * 0.14, headCenter.y() + hh * 1.00);
    poly.append(rotatePoint(chinTip));
    poly.append(rotatePoint(chinRight));

    // 4. Right Jaw & Cheek: from chin -> right jaw curve -> right temple
    const QPointF rightJawCtrl(headCenter.x() + hw * 0.62, headCenter.y() + hh * 0.85);
    const QPointF rightCheekCtrl(headCenter.x() + hw * 1.04, headCenter.y() + hh * 0.38);
    const QPointF rightTemple(headCenter.x() + hw * 1.02, headCenter.y());

    for (int i = 1; i <= jawSteps; ++i) {
        const qreal t = qreal(i) / jawSteps;
        poly.append(rotatePoint(evalCubic(chinRight, rightJawCtrl, rightCheekCtrl, rightTemple, t)));
    }

    return poly;
}

QVector<KisAiStrokeOperation>
KisAiRigLibrary::hierarchicalHairClumpOps(const KisAiRigParameterSet &params, const QSize &canvasSize, quint32 seed)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> ops;
    const QPointF &hc = params.headCenter;
    const qreal hw = params.headWidth * 0.5;
    const qreal hh = params.headHeight * 0.5;
    const QColor hair = params.hairColor;
    const QColor hairShadow = hair.darker(135);
    const QColor hairLine = params.lineColor;

    QRandomGenerator rng(seed);

    const auto evalCubic =
        [](const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t) -> QPointF {
        const qreal it = 1.0 - t;
        return it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
    };

    // Generate 5 main volumetric hair fringe clumps across the forehead
    constexpr int clumpCount = 5;
    for (int c = 0; c < clumpCount; ++c) {
        const qreal tRoot = qreal(c) / (clumpCount - 1);
        const qreal rootX = hc.x() + (tRoot - 0.5) * (hw * 1.55);
        const qreal rootY = hc.y() - hh * 0.65 - std::sin(tRoot * M_PI) * (hh * 0.15);

        // Clump flow curvature (S-curve towards eyes/cheeks)
        const qreal sBend = (rng.generateDouble() - 0.5) * 0.04;
        const qreal tipLen = hh * (0.65 + rng.generateDouble() * 0.35);
        const qreal tipX = rootX + (tRoot - 0.5) * (hw * 0.45) + sBend;
        const qreal tipY = rootY + tipLen;
        const qreal clumpW = hw * (0.28 + rng.generateDouble() * 0.12);

        // Bezier control points for left and right ribbons
        const QPointF pL0(rootX - clumpW * 0.5, rootY);
        const QPointF pL1(rootX - clumpW * 0.55 + sBend * 0.3, rootY + tipLen * 0.35);
        const QPointF pL2(tipX - clumpW * 0.18 + sBend * 0.7, rootY + tipLen * 0.75);
        const QPointF pL3(tipX, tipY);

        const QPointF pR0(tipX, tipY);
        const QPointF pR1(tipX + clumpW * 0.18 + sBend * 0.7, rootY + tipLen * 0.75);
        const QPointF pR2(rootX + clumpW * 0.55 + sBend * 0.3, rootY + tipLen * 0.35);
        const QPointF pR3(rootX + clumpW * 0.5, rootY);

        QPolygonF clumpPoly;
        constexpr int steps = 8;
        for (int i = 0; i <= steps; ++i) {
            clumpPoly.append(evalCubic(pL0, pL1, pL2, pL3, qreal(i) / steps));
        }
        for (int i = 1; i <= steps; ++i) {
            clumpPoly.append(evalCubic(pR0, pR1, pR2, pR3, qreal(i) / steps));
        }

        // 1. Clump Cast Shadow (slightly offset downward)
        QPolygonF shadowPoly;
        for (const auto &p : clumpPoly) {
            shadowPoly.append(p + QPointF(0.002, 0.005));
        }
        const QString group = QStringLiteral("hair_clump_%1").arg(c);
        KisAiStrokeOperation shOp;
        shOp.kind = KisAiStrokeOperation::Kind::Fill;
        shOp.id = QStringLiteral("hair_clump_shadow_%1").arg(c);
        shOp.groupId = group;
        shOp.role = QStringLiteral("mass");
        shOp.layer = QStringLiteral("Shading");
        shOp.polygon = shadowPoly;
        shOp.brush.color = hairShadow;
        shOp.brush.opacity = 0.28;
        shOp.brush.profile = QStringLiteral("watercolor");
        shOp.fillStyle = QStringLiteral("wash");
        ops.append(shOp);

        // 2. Main Clump Body (Flats)
        KisAiStrokeOperation bodyOp;
        bodyOp.kind = KisAiStrokeOperation::Kind::Fill;
        bodyOp.id = QStringLiteral("hair_clump_body_%1").arg(c);
        bodyOp.groupId = group;
        bodyOp.role = QStringLiteral("mass");
        bodyOp.layer = QStringLiteral("Flats");
        bodyOp.polygon = clumpPoly;
        bodyOp.brush.color = hair;
        bodyOp.brush.opacity = 1.0;
        bodyOp.brush.profile = QStringLiteral("brush");
        bodyOp.fillStyle = QStringLiteral("contour");
        ops.append(bodyOp);

        // 3. Left and right clump contours as smooth Bezier strokes (入り抜き)
        QVector<KisAiStrokePoint> leftPts;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const QPointF pt = evalCubic(pL0, pL1, pL2, pL3, t);
            const qreal pressure = (i == 0) ? 0.35 : (i == steps ? 0.20 : (0.45 + 0.35 * std::sin(t * M_PI)));
            leftPts.append(KisAiStrokePoint(pt.x(), pt.y(), pressure));
        }
        KisAiStrokeOperation leftOp;
        leftOp.kind = KisAiStrokeOperation::Kind::Path;
        leftOp.id = QStringLiteral("hair_clump_line_%1_l").arg(c);
        leftOp.groupId = group;
        leftOp.parentId = bodyOp.id;
        leftOp.role = QStringLiteral("contour");
        leftOp.layer = QStringLiteral("Lineart");
        leftOp.points = leftPts;
        leftOp.smooth = true;
        leftOp.brush.color = hairLine;
        leftOp.brush.size = 0.0028;
        leftOp.brush.profile = QStringLiteral("gpen");
        leftOp.brush.opacity = 0.85;
        ops.append(leftOp);

        QVector<KisAiStrokePoint> rightPts;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const QPointF pt = evalCubic(pR0, pR1, pR2, pR3, t);
            const qreal pressure = (i == 0) ? 0.20 : (i == steps ? 0.35 : (0.45 + 0.35 * std::sin(t * M_PI)));
            rightPts.append(KisAiStrokePoint(pt.x(), pt.y(), pressure));
        }
        KisAiStrokeOperation rightOp;
        rightOp.kind = KisAiStrokeOperation::Kind::Path;
        rightOp.id = QStringLiteral("hair_clump_line_%1_r").arg(c);
        rightOp.groupId = group;
        rightOp.parentId = bodyOp.id;
        rightOp.role = QStringLiteral("contour");
        rightOp.layer = QStringLiteral("Lineart");
        rightOp.points = rightPts;
        rightOp.smooth = true;
        rightOp.brush.color = hairLine;
        rightOp.brush.size = 0.0028;
        rightOp.brush.profile = QStringLiteral("gpen");
        rightOp.brush.opacity = 0.85;
        ops.append(rightOp);

        // Volumetric inner strand gleam along center spine
        QVector<KisAiStrokePoint> shinePts;
        const QPointF pS0(rootX, rootY);
        const QPointF pS1(rootX + sBend * 0.2, rootY + tipLen * 0.30);
        const QPointF pS2(tipX + sBend * 0.5, rootY + tipLen * 0.65);
        const QPointF pS3(tipX, tipY - tipLen * 0.12);
        for (int i = 0; i <= 6; ++i) {
            const qreal t = qreal(i) / 6;
            const QPointF pt = evalCubic(pS0, pS1, pS2, pS3, t);
            shinePts.append(KisAiStrokePoint(pt.x(), pt.y(), 0.35 + 0.35 * std::sin(t * M_PI)));
        }
        KisAiStrokeOperation shineOp;
        shineOp.kind = KisAiStrokeOperation::Kind::Path;
        shineOp.id = QStringLiteral("hair_clump_shine_%1").arg(c);
        shineOp.groupId = group;
        shineOp.parentId = bodyOp.id;
        shineOp.role = QStringLiteral("highlight");
        shineOp.layer = QStringLiteral("Highlights");
        shineOp.points = shinePts;
        shineOp.smooth = true;
        shineOp.brush.color = QColor(255, 255, 255);
        shineOp.brush.size = 0.0022;
        shineOp.brush.profile = QStringLiteral("airbrush");
        shineOp.brush.opacity = 0.40;
        shineOp.blendMode = QStringLiteral("screen");
        ops.append(shineOp);
    }

    // Delicate flyaway wisps (loose strands adding organic liveliness)
    for (int f = 0; f < 3; ++f) {
        const qreal side = (f % 2 == 0) ? -1.0 : 1.0;
        const qreal startX = hc.x() + side * (hw * 0.85);
        const qreal startY = hc.y() - hh * 0.40 + f * 0.05;
        QVector<KisAiStrokePoint> flyPts;
        flyPts.append(KisAiStrokePoint(startX, startY, 0.2));
        flyPts.append(KisAiStrokePoint(startX + side * 0.04, startY + 0.08, 0.6));
        flyPts.append(KisAiStrokePoint(startX + side * 0.07, startY + 0.16, 0.2));

        KisAiStrokeOperation flyOp;
        flyOp.kind = KisAiStrokeOperation::Kind::Path;
        flyOp.id = QStringLiteral("hair_flyaway_%1").arg(f);
        flyOp.layer = QStringLiteral("Lineart");
        flyOp.points = flyPts;
        flyOp.smooth = true;
        flyOp.brush.color = hairLine;
        flyOp.brush.size = 0.0022;
        flyOp.brush.profile = QStringLiteral("fineliner");
        flyOp.brush.opacity = 0.70;
        ops.append(flyOp);
    }

    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::draperyFoldOps(const QPointF &origin,
                                                              const QPointF &target,
                                                              qreal widthPx,
                                                              const QColor &clothColor,
                                                              const QColor &shadowColor,
                                                              const QString &idSuffix)
{
    Q_UNUSED(clothColor);
    Q_UNUSED(widthPx);
    QVector<KisAiStrokeOperation> ops;

    const QPointF delta = target - origin;
    const qreal dist = std::hypot(delta.x(), delta.y());
    if (dist < 1e-4)
        return ops;

    // Perpendicular sag vector for catenary drape curve
    const QPointF normal(-delta.y() / dist, delta.x() / dist);
    const qreal sag = dist * 0.16;

    const auto evalCubic =
        [](const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal t) -> QPointF {
        const qreal it = 1.0 - t;
        return it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
    };

    const QPointF c1 = origin + delta * 0.32 + normal * (sag * 0.85);
    const QPointF c2 = origin + delta * 0.68 + normal * (sag * 0.95);

    // 1. Tension fold lineart (smooth cubic Bezier)
    QVector<KisAiStrokePoint> foldLine;
    constexpr int foldSteps = 8;
    for (int i = 0; i <= foldSteps; ++i) {
        const qreal t = qreal(i) / foldSteps;
        const QPointF pt = evalCubic(origin, c1, c2, target, t);
        const qreal pressure = (i == 0 || i == foldSteps) ? 0.25 : (0.45 + 0.35 * std::sin(t * M_PI));
        foldLine.append(KisAiStrokePoint(pt.x(), pt.y(), pressure));
    }

    KisAiStrokeOperation lineOp;
    lineOp.kind = KisAiStrokeOperation::Kind::Path;
    lineOp.id = idSuffix.isEmpty() ? QStringLiteral("drapery_tension_line")
                                   : QStringLiteral("drapery_tension_line_%1").arg(idSuffix);
    lineOp.layer = QStringLiteral("Lineart");
    lineOp.points = foldLine;
    lineOp.smooth = true;
    lineOp.brush.color = shadowColor.darker(120);
    lineOp.brush.size = 0.0026;
    lineOp.brush.profile = QStringLiteral("gpen");
    lineOp.brush.opacity = 0.75;
    ops.append(lineOp);

    // 2. Soft under-fold shadow (smooth ribbon polygon)
    QPolygonF foldShade;
    for (int i = 0; i <= foldSteps; ++i) {
        foldShade.append(evalCubic(origin, c1, c2, target, qreal(i) / foldSteps));
    }
    const QPointF sc2 = c2 + normal * (sag * 0.55);
    const QPointF sc1 = c1 + normal * (sag * 0.50);
    for (int i = foldSteps; i >= 0; --i) {
        foldShade.append(evalCubic(origin, sc1, sc2, target, qreal(i) / foldSteps));
    }

    KisAiStrokeOperation shOp;
    shOp.kind = KisAiStrokeOperation::Kind::Fill;
    shOp.id = idSuffix.isEmpty() ? QStringLiteral("drapery_fold_shade")
                                 : QStringLiteral("drapery_fold_shade_%1").arg(idSuffix);
    shOp.layer = QStringLiteral("Shading");
    shOp.polygon = foldShade;
    shOp.brush.color = shadowColor;
    shOp.brush.opacity = 0.32;
    shOp.brush.profile = QStringLiteral("watercolor");
    shOp.fillStyle = QStringLiteral("wash");
    ops.append(shOp);

    return ops;
}
