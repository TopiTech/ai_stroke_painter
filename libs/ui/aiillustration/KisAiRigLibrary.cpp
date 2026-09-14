/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiRigLibrary.h"

#include "KisAiStrokeProgram.h"

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
QPair<EyeAnchor, EyeAnchor> eyeAnchors(const KisAiRigParameterSet &params)
{
    const qreal spread = params.headWidth * 0.19;
    const qreal eyeY = params.headCenter.y() + params.headHeight * 0.08;
    const qreal baseW = params.headWidth * 0.17;
    const qreal baseH = params.headWidth * 0.16
        * (0.30 + 0.70 * clampRange(params.eyeLeft.aperture, 0.0, 1.0));

    EyeAnchor left;
    left.center = QPointF(params.headCenter.x() - spread, eyeY);
    left.width = baseW;
    left.height = baseH;

    EyeAnchor right = left;
    right.center = QPointF(params.headCenter.x() + spread, eyeY);
    return qMakePair(left, right);
}

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
        const qreal x = anchor.center.x() - dir * anchor.width * 0.5
            + dir * anchor.width * t;
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

    p.backdrop.timeOfDay = spec.light.timeOfDay;
    p.backdrop.weather = spec.narrative.weather.isEmpty()
        ? QStringLiteral("clear") : spec.narrative.weather;
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

    for (KisAiEyeRigParams *eye : { &p.eyeLeft, &p.eyeRight }) {
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

QVector<KisAiStrokeOperation> KisAiRigLibrary::eyePairOps(
    const KisAiRigParameterSet &params, quint32 seed)
{
    Q_UNUSED(seed);
    QVector<KisAiStrokeOperation> ops;
    const auto anchors = eyeAnchors(params);
    const QString expr = eyeExpressionFor(params);
    const QColor lashColor = darken(params.hairColor, 0.65);
    const qreal lashSize = lineWeightBase(params.lineWeight) * 1.25;

    const struct { const EyeAnchor &anchor; bool isRight; const char *side; } eyes[2] = {
        { anchors.first, false, "l" },
        { anchors.second, true, "r" },
    };

    for (const auto &eye : eyes) {
        const QString side = QString::fromLatin1(eye.side);

        if (expr == QLatin1String("closed")) {
            // Single gentle downward arc — no iris, no highlight.
            QVector<KisAiStrokePoint> pts;
            const int steps = 6;
            for (int i = 0; i <= steps; ++i) {
                const qreal t = qreal(i) / steps;
                const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.5
                    + eye.anchor.width * t;
                const qreal y = eye.anchor.center.y()
                    + std::sin(M_PI * t) * eye.anchor.height * 0.35;
                pts.append(pt(x, y, 0.4 + 0.5 * std::sin(M_PI * t)));
            }
            ops.append(makePathOp(QStringLiteral("rig_eye_%1_closed").arg(side),
                                  pts, lashColor, lashSize, 0.95));
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
                              lashColor, lashSize, 1.0));
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

    const struct { const EyeAnchor &anchor; const char *side; } eyes[2] = {
        { anchors.first, "l" },
        { anchors.second, "r" },
    };

    for (const auto &eye : eyes) {
        const bool hasLid = (std::strcmp(eye.side, "l") == 0) ? params.eyeLeft.doubleLid : params.eyeRight.doubleLid;
        if (!hasLid)
            continue;
        QVector<KisAiStrokePoint> pts;
        const int steps = 5;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.42
                + eye.anchor.width * 0.84 * t;
            const qreal y = eye.anchor.center.y() - eye.anchor.height * 0.72
                - std::sin(M_PI * t) * eye.anchor.height * 0.12;
            pts.append(pt(x, y, 0.3 + 0.35 * std::sin(M_PI * t)));
        }
        ops.append(makePathOp(QStringLiteral("rig_eye_%1_lid").arg(eye.side),
                              pts, creaseColor, creaseSize, 0.55));
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

    const struct { const EyeAnchor &anchor; const char *side; } eyes[2] = {
        { anchors.first, "l" },
        { anchors.second, "r" },
    };

    for (const auto &eye : eyes) {
        QVector<KisAiStrokePoint> pts;
        const int steps = 5;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = eye.anchor.center.x() - eye.anchor.width * 0.55
                + eye.anchor.width * 1.10 * t;
            const qreal y = eye.anchor.center.y() - eye.anchor.height * (1.15 + lift * std::sin(M_PI * t));
            pts.append(pt(x, y, 0.35 + 0.6 * std::sin(M_PI * t)));
        }
        ops.append(makePathOp(QStringLiteral("rig_brow_%1").arg(eye.side),
                              pts, browColor, browSize, 0.9));
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

    // Tiny point + short shadow; solid black noses stay impossible because the
    // color derives from skin tone at clamped strength.
    {
        QVector<KisAiStrokePoint> pts;
        pts.append(pt(noseX, noseY - s, 0.5));
        pts.append(pt(noseX, noseY, 0.9));
        pts.append(pt(noseX, noseY + s, 0.5));
        ops.append(makePathOp(QStringLiteral("rig_nose_point"), pts,
                              mixColor(params.skinTone, QColor(120, 70, 62), 0.45),
                              lineWeightBase(params.lineWeight) * 0.9, 0.8));
    }
    {
        QVector<KisAiStrokePoint> pts;
        pts.append(pt(noseX + s * 1.5, noseY + s * 0.4, 0.9));
        pts.append(pt(noseX + s * 3.0, noseY + s * 1.2, 0.25));
        ops.append(makePathOp(QStringLiteral("rig_nose_shadow"), pts,
                              shadowColor,
                              lineWeightBase(params.lineWeight) * 1.4,
                              params.nose.shadowStrength));
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
    mouth.mouthCenter = QPointF(params.headCenter.x(),
                                params.headCenter.y() + params.headHeight * 0.30);
    mouth.mouthSize = QSizeF(params.headWidth * 0.11 * params.mouth.widthScale,
                             params.headWidth * 0.05);
    mouth.mouthExpression = params.mouth.expression;
    mouth.mouthLipColor = mixColor(params.skinTone, QColor(224, 117, 125), 0.55);
    mouth.mouthHasHighlight = params.mouth.highlight;
    mouth.brush.opacity = 1.0;
    ops.append(mouth);
    return ops;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::hairHighlightOps(
    const KisAiRigParameterSet &params, quint32 seed)
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
        qreal yBase;      // relative to topY in headHeight units
        qreal xShift;     // relative to headWidth
        qreal widthScale;
        qreal opacity;
    };

    QVector<BandDef> defs;
    defs.append({ mainBand, 0.12, 0.02, 1.0, 0.60 });
    if (bands >= 2)
        defs.append({ subBand, 0.22, -0.16, 0.6, 0.40 });
    if (bands >= 3)
        defs.append({ counterBand, 0.07, -0.22, 0.45, 0.35 });

    int index = 0;
    for (const BandDef &def : defs) {
        // Each band is a screen-blended ribbon following the dome curvature.
        QVector<QPointF> spine;
        const int steps = 7;
        for (int i = 0; i <= steps; ++i) {
            const qreal t = qreal(i) / steps;
            const qreal x = params.headCenter.x() + (t - 0.5) * params.headWidth * 0.72
                + def.xShift * params.headWidth;
            const qreal y = topY + params.headHeight * def.yBase
                + std::sin(M_PI * t) * params.headHeight * 0.045;
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
    if (t.contains(QLatin1String("golden")) || t.contains(QLatin1String("sunset"))
        || t.contains(QLatin1String("dusk")) || t.contains(QLatin1String("evening")))
        return QStringLiteral("sunset");
    if (t.contains(QLatin1String("night")) || t.contains(QLatin1String("midnight"))
        || t.contains(QLatin1String("moon")) || t.contains(QLatin1String("star")))
        return QStringLiteral("night");
    if (t.contains(QLatin1String("morning")) || t.contains(QLatin1String("noon"))
        || t.contains(QLatin1String("day")))
        return QStringLiteral("day");
    return fallback;
}

QVector<KisAiStrokeOperation> KisAiRigLibrary::backdropWeatherOps(
    const KisAiRigParameterSet &params, const QSize &canvasSize, quint32 seed)
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
                cloud.append(QPointF(cx + std::cos(t) * r * wob,
                                     cy + std::sin(t) * r * 0.55 * wob));
            }
            const QColor cloudColor = tod == QLatin1String("night")
                ? QColor(70, 78, 100) : QColor(235, 238, 245);
            ops.append(makeFillOp(QStringLiteral("rig_cloud_%1").arg(side),
                                  cloud, cloudColor, 0.7, QStringLiteral("Background")));
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
            glow.gradientColors = { QColor(255, 196, 110, 160), QColor(255, 196, 110, 0) };
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
        if (op.kind == KisAiStrokeOperation::Kind::Particles
            && op.bounds.isValid()
            && op.bounds.intersects(faceBox)) {
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
