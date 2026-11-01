/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiPrimitiveExpander.h"

#include "KisAiStrokeGraph.h"
#include "KisAiStrokeQualityUtils.h"

#include <QRandomGenerator>
#include <QtMath>

#include <cmath>

namespace
{
constexpr qreal PI = 3.14159265358979323846;

// Strict side detection: only full "_" separated tokens ("l"/"left",
// "r"/"right") count. Substring matching misfires on ids like
// "face_rim_light" ("_rim" contains "_r") or "eye_highlight".
bool idHasSideToken(const QString &id, const QString &shortToken, const QString &longToken)
{
    const QStringList parts = id.toLower().split(QLatin1Char('_'), Qt::SkipEmptyParts);
    return parts.contains(shortToken) || parts.contains(longToken);
}

QVector<KisAiStrokePoint>
sampleQuad(const QPointF &a, const QPointF &ctrl, const QPointF &b, int steps, qreal p0, qreal p1)
{
    QVector<KisAiStrokePoint> pts;
    pts.reserve(steps + 1);
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / qreal(steps);
        const qreal it = 1.0 - t;
        const QPointF p = it * it * a + 2.0 * it * t * ctrl + t * t * b;
        const qreal pr = p0 + (p1 - p0) * t;
        pts.append(KisAiStrokePoint(p.x(), p.y(), pr));
    }
    return pts;
}

QPolygonF ellipsePoly(const QPointF &c, qreal rx, qreal ry, int steps = 24)
{
    QPolygonF poly;
    poly.reserve(steps);
    for (int i = 0; i < steps; ++i) {
        const qreal a = 2.0 * PI * qreal(i) / qreal(steps);
        poly.append(QPointF(c.x() + std::cos(a) * rx, c.y() + std::sin(a) * ry));
    }
    return poly;
}

QPolygonF almondEyePoly(const QPointF &c, qreal w, qreal h, qreal outerSign, int steps = 28)
{
    QPolygonF poly;
    poly.reserve(steps);
    const qreal rx = w * 0.50;
    const qreal ry = h * 0.48;
    for (int i = 0; i < steps; ++i) {
        const qreal a = 2.0 * PI * qreal(i) / qreal(steps);
        const qreal cosA = std::cos(a);
        const qreal sinA = std::sin(a);
        // Subtle almond pinch at inner/outer corners and slight upward outer tilt
        const qreal cornerPinch = 1.0 - 0.12 * std::pow(cosA, 4.0);
        const qreal tiltOffset = (cosA * outerSign) * (h * 0.035);
        poly.append(QPointF(c.x() + cosA * rx, c.y() + sinA * ry * cornerPinch - tiltOffset));
    }
    return poly;
}

QPolygonF starSparklePoly(const QPointF &c, qreal rx, qreal ry)
{
    QPolygonF poly;
    constexpr int kSpikes = 4;
    poly.reserve(kSpikes * 2);
    for (int i = 0; i < kSpikes * 2; ++i) {
        const qreal a = (PI * qreal(i)) / qreal(kSpikes);
        const qreal scale = (i % 2 == 0) ? 1.0 : 0.22;
        poly.append(QPointF(c.x() + std::cos(a) * rx * scale, c.y() + std::sin(a) * ry * scale));
    }
    return poly;
}

QPolygonF sakuraPetalPoly(const QPointF &c, qreal r, qreal angleRad)
{
    QPolygonF poly;
    constexpr int kSteps = 16;
    poly.reserve(kSteps);
    const qreal cosR = std::cos(angleRad);
    const qreal sinR = std::sin(angleRad);
    for (int i = 0; i < kSteps; ++i) {
        const qreal t = 2.0 * PI * qreal(i) / qreal(kSteps);
        // Teardrop/petal contour with characteristic sakura apical cleft near t = 0
        const qreal cleft = (std::cos(t) > 0.75) ? (1.0 - 0.32 * std::pow((std::cos(t) - 0.75) / 0.25, 2.0)) : 1.0;
        const qreal lx = std::cos(t) * r * 1.15 * cleft;
        const qreal ly = std::sin(t) * r * 0.72 * (0.85 + 0.15 * std::cos(t));
        poly.append(QPointF(c.x() + lx * cosR - ly * sinR, c.y() + lx * sinR + ly * cosR));
    }
    return poly;
}

KisAiStrokeOperation makeLinearGradientFill(const QString &id,
                                            const QString &groupId,
                                            const QString &layer,
                                            const QPolygonF &poly,
                                            const QPointF &startPt,
                                            const QPointF &endPt,
                                            const QVector<QColor> &colors,
                                            qreal opacity,
                                            const QString &role,
                                            const QString &clipToId = QString(),
                                            const QString &blendMode = QStringLiteral("normal"))
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::GradientFill;
    op.id = id;
    op.groupId = groupId;
    op.role = role;
    op.layer = layer;
    op.polygon = poly;
    op.isRadial = false;
    op.fillStyle = QStringLiteral("directional");
    op.points = {KisAiStrokePoint(startPt.x(), startPt.y(), 1.0),
                 KisAiStrokePoint(endPt.x(), endPt.y(), 1.0)};
    op.gradientColors = colors;
    op.brush.color = colors.isEmpty() ? QColor(128, 128, 128) : colors.first();
    op.brush.opacity = opacity;
    op.brush.profile = QStringLiteral("auto");
    op.clipToId = clipToId;
    op.blendMode = blendMode;
    op.smooth = true;
    return op;
}

KisAiStrokeOperation makeRadialGradientFill(const QString &id,
                                            const QString &groupId,
                                            const QString &layer,
                                            const QPolygonF &poly,
                                            const QPointF &centerPt,
                                            qreal radius,
                                            const QVector<QColor> &colors,
                                            qreal opacity,
                                            const QString &role,
                                            const QString &clipToId = QString(),
                                            const QString &blendMode = QStringLiteral("normal"))
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::GradientFill;
    op.id = id;
    op.groupId = groupId;
    op.role = role;
    op.layer = layer;
    op.polygon = poly;
    op.isRadial = true;
    op.fillStyle = QStringLiteral("radial");
    op.gradientCenter = centerPt;
    op.gradientRadius = radius;
    op.gradientColors = colors;
    op.brush.color = colors.isEmpty() ? QColor(255, 255, 255) : colors.first();
    op.brush.opacity = opacity;
    op.brush.profile = QStringLiteral("auto");
    op.clipToId = clipToId;
    op.blendMode = blendMode;
    op.smooth = true;
    return op;
}

QString sideTag(const KisAiStrokeOperation &op)
{
    if (op.eyeIsRight || idHasSideToken(op.id, QStringLiteral("r"), QStringLiteral("right")))
        return QStringLiteral("r");
    return QStringLiteral("l");
}
} // namespace

bool KisAiPrimitiveExpander::isCompositeKind(KisAiStrokeOperation::Kind kind)
{
    return kind == KisAiStrokeOperation::Kind::AnimeEye || kind == KisAiStrokeOperation::Kind::AnimeMouth
        || kind == KisAiStrokeOperation::Kind::Hatch || kind == KisAiStrokeOperation::Kind::MangaLines
        || kind == KisAiStrokeOperation::Kind::Particles || kind == KisAiStrokeOperation::Kind::BezierPath
        || kind == KisAiStrokeOperation::Kind::ParametricShape || kind == KisAiStrokeOperation::Kind::FormShading
        || kind == KisAiStrokeOperation::Kind::TextureHatch;
}

int KisAiPrimitiveExpander::atomicPathCount(const QVector<KisAiStrokeOperation> &ops)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (op.kind == KisAiStrokeOperation::Kind::Path)
            ++n;
    }
    return n;
}

int KisAiPrimitiveExpander::leftoverCompositeCount(const QVector<KisAiStrokeOperation> &ops)
{
    int n = 0;
    for (const KisAiStrokeOperation &op : ops) {
        if (isCompositeKind(op.kind))
            ++n;
    }
    return n;
}

KisAiStrokeOperation KisAiPrimitiveExpander::makePath(const QString &id,
                                                      const QString &groupId,
                                                      const QString &layer,
                                                      const QVector<KisAiStrokePoint> &pts,
                                                      const QColor &color,
                                                      const QString &profile,
                                                      qreal size,
                                                      qreal opacity,
                                                      const QString &role,
                                                      const QString &parentId)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Path;
    op.id = id;
    op.groupId = groupId;
    op.parentId = parentId;
    op.role = role;
    op.layer = layer;
    op.points = pts;
    op.smooth = true;
    op.closed = false;
    op.brush.profile = profile;
    op.brush.color = color;
    op.brush.size = size;
    op.brush.opacity = opacity;
    return op;
}

KisAiStrokeOperation KisAiPrimitiveExpander::makeFill(const QString &id,
                                                      const QString &groupId,
                                                      const QString &layer,
                                                      const QPolygonF &poly,
                                                      const QColor &color,
                                                      qreal opacity,
                                                      const QString &role,
                                                      const QString &clipToId)
{
    KisAiStrokeOperation op;
    op.kind = KisAiStrokeOperation::Kind::Fill;
    op.id = id;
    op.groupId = groupId;
    op.role = role;
    op.layer = layer;
    op.polygon = poly;
    op.brush.color = color;
    op.brush.opacity = opacity;
    op.brush.profile = QStringLiteral("auto");
    op.clipToId = clipToId;
    op.fillStyle = QStringLiteral("contour");
    return op;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandEye(const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    const QString side = sideTag(op);
    const QString group = op.groupId.isEmpty() ? QStringLiteral("eye_%1").arg(side) : op.groupId;
    const QPointF c = op.eyeCenter;
    const qreal w = op.eyeSize.width();
    const qreal h = op.eyeSize.height();
    if (w < 0.004 || h < 0.004)
        return out;

    const qreal outerSign = op.eyeIsRight ? 1.0 : -1.0;
    const qreal innerSign = -outerSign;
    const QColor iris = op.eyeIrisColor.isValid() ? op.eyeIrisColor : QColor(60, 120, 240);
    const QColor deepIris = KisAiStrokeQualityUtils::calculateHueShiftedShadow(iris, QColor(20, 22, 48), 0.62);
    QColor dark = deepIris.darker(220);
    dark.setAlpha(255);
    const QColor glow = op.eyeSecondaryColor.isValid()
        ? op.eyeSecondaryColor
        : KisAiStrokeQualityUtils::calculateHueShiftedHighlight(iris, QColor(245, 255, 255), 0.58);

    const QString scleraId = QStringLiteral("%1_sclera").arg(group);
    const QPolygonF scleraPoly = almondEyePoly(c, w, h, outerSign, 28);

    // 1. Base Sclera (Keep Kind::Fill on Flats so KisAiStrokeGraph::critiqueGroup & silhouette clip find it)
    out.append(makeFill(scleraId,
                        group,
                        QStringLiteral("Flats"),
                        scleraPoly,
                        QColor(252, 252, 255),
                        1.0,
                        QStringLiteral("mass")));

    // 1b. Inner corner lacrimal caruncle (warm pink tear duct)
    const QPointF caruncleCenter(c.x() + innerSign * w * 0.42, c.y() + h * 0.03);
    out.append(makeFill(QStringLiteral("%1_caruncle").arg(group),
                        group,
                        QStringLiteral("Flats"),
                        ellipsePoly(caruncleCenter, w * 0.065, h * 0.075, 12),
                        QColor(242, 172, 180),
                        0.85,
                        QStringLiteral("mass"),
                        scleraId));

    // 1c. Sclera upper-lid cast shadow gradient
    out.append(makeLinearGradientFill(
        QStringLiteral("%1_sclera_shade").arg(group),
        group,
        QStringLiteral("Shading"),
        scleraPoly,
        QPointF(c.x(), c.y() - h * 0.48),
        QPointF(c.x(), c.y() + h * 0.12),
        {QColor(175, 182, 215, 185),
         QColor(205, 210, 232, 80),
         QColor(252, 252, 255, 0)},
        0.75,
        QStringLiteral("mass"),
        scleraId,
        QStringLiteral("multiply")));

    // 2. Iris Limbal Ring & Main Iris Base
    const QString irisId = QStringLiteral("%1_iris").arg(group);
    const QPointF irisC(c.x(), c.y() - h * 0.02);
    const qreal irisRx = w * 0.32;
    const qreal irisRy = h * 0.42;
    const QPolygonF irisPoly = ellipsePoly(irisC, irisRx, irisRy, 28);

    // Keep Kind::Fill on irisId for centroid symmetry checks and child clipToId anchoring
    out.append(makeFill(irisId,
                        group,
                        QStringLiteral("Flats"),
                        irisPoly,
                        iris,
                        1.0,
                        QStringLiteral("mass"),
                        scleraId));

    // 2b. Multi-stop Deep Volumetric Iris Gradient (clipped to irisId)
    out.append(makeLinearGradientFill(
        QStringLiteral("%1_iris_depth").arg(group),
        group,
        QStringLiteral("Flats"),
        irisPoly,
        QPointF(irisC.x(), irisC.y() - irisRy),
        QPointF(irisC.x(), irisC.y() + irisRy),
        {dark, deepIris, iris, glow},
        0.95,
        QStringLiteral("mass"),
        irisId));

    // 2c. Lower Caustics Luminescence Crescent
    const QPointF causticsC(irisC.x(), irisC.y() + irisRy * 0.35);
    out.append(makeRadialGradientFill(
        QStringLiteral("%1_iris_glow").arg(group),
        group,
        QStringLiteral("Flats"),
        ellipsePoly(QPointF(c.x(), c.y() + h * 0.10), w * 0.24, h * 0.20, 20),
        causticsC,
        qMax(w, h) * 0.24,
        {glow.lighter(120), glow, QColor(glow.red(), glow.green(), glow.blue(), 0)},
        0.88,
        QStringLiteral("mass"),
        irisId));

    // 3. Soft-Edge Volumetric Pupil
    const QPointF pupilC(c.x(), c.y() - h * 0.04);
    const qreal pupilRx = w * 0.11;
    const qreal pupilRy = h * 0.145;
    out.append(makeRadialGradientFill(
        QStringLiteral("%1_pupil").arg(group),
        group,
        QStringLiteral("Flats"),
        ellipsePoly(pupilC, pupilRx, pupilRy, 20),
        pupilC,
        qMax(pupilRx, pupilRy),
        {QColor(12, 10, 22, 255), dark, deepIris},
        1.0,
        QStringLiteral("mass"),
        irisId));

    // 4. Upper Limbal Ring Definition Arc
    QVector<KisAiStrokePoint> limbal;
    for (int i = 0; i <= 12; ++i) {
        const qreal t = PI * (0.08 + 0.84 * qreal(i) / 12.0);
        limbal.append(KisAiStrokePoint(irisC.x() + std::cos(t) * irisRx,
                                       irisC.y() - std::sin(t) * irisRy,
                                       0.55 + 0.35 * std::sin(PI * qreal(i) / 12.0)));
    }
    KisAiStrokeOperation limbalOp = makePath(QStringLiteral("%1_limbal").arg(group),
                                             group,
                                             QStringLiteral("Lineart"),
                                             limbal,
                                             dark,
                                             QStringLiteral("fineliner"),
                                             0.0016,
                                             0.88,
                                             QStringLiteral("internal"),
                                             irisId);
    limbalOp.closed = false;
    out.append(limbalOp);

    // 5. Radial Iris Striations (12 delicate fibers)
    const int striations = 12;
    for (int i = 0; i < striations; ++i) {
        const qreal angle = (PI * 0.10) + (PI * 0.80) * (qreal(i) / qreal(striations - 1));
        QVector<KisAiStrokePoint> st;
        st.append(KisAiStrokePoint(pupilC.x() + std::cos(angle) * (pupilRx * 1.05),
                                   pupilC.y() + std::sin(angle) * (pupilRy * 0.95),
                                   0.35));
        st.append(KisAiStrokePoint(irisC.x() + std::cos(angle) * (irisRx * 0.82),
                                   irisC.y() + std::sin(angle) * (irisRy * 0.78),
                                   0.70));
        KisAiStrokeOperation stOp = makePath(QStringLiteral("%1_striation_%2").arg(group).arg(i),
                                             group,
                                             QStringLiteral("Highlights"),
                                             st,
                                             (i % 2 == 0) ? glow.lighter(115) : iris.lighter(145),
                                             QStringLiteral("fineliner"),
                                             0.0009,
                                             0.52,
                                             QStringLiteral("internal"),
                                             irisId);
        stOp.clipToId = irisId;
        stOp.blendMode = QStringLiteral("screen");
        out.append(stOp);
    }

    // 6. Multi-Layered Catchlights (Bloom Halo + Primary + Secondary + Star Glint)
    const QPointF mainCatchC(c.x() - w * 0.12, c.y() - h * 0.16);
    out.append(makeRadialGradientFill(
        QStringLiteral("%1_catch_halo").arg(group),
        group,
        QStringLiteral("Highlights"),
        ellipsePoly(mainCatchC, w * 0.085, h * 0.095, 16),
        mainCatchC,
        qMax(w, h) * 0.095,
        {QColor(255, 255, 255, 190),
         QColor(225, 245, 255, 95),
         QColor(225, 245, 255, 0)},
        0.75,
        QStringLiteral("accent"),
        scleraId,
        QStringLiteral("screen")));

    out.append(makeFill(QStringLiteral("%1_catch_main").arg(group),
                        group,
                        QStringLiteral("Highlights"),
                        ellipsePoly(mainCatchC, w * 0.048, h * 0.056, 16),
                        QColor(255, 255, 255),
                        0.98,
                        QStringLiteral("accent"),
                        scleraId));

    const QPointF subCatchC(c.x() + w * 0.11, c.y() + h * 0.10);
    out.append(makeFill(QStringLiteral("%1_catch_sub").arg(group),
                        group,
                        QStringLiteral("Highlights"),
                        ellipsePoly(subCatchC, w * 0.025, h * 0.028, 12),
                        QColor(245, 252, 255),
                        0.90,
                        QStringLiteral("accent"),
                        irisId));

    if (op.eyeStyle != QLatin1String("minimal")) {
        const QPointF starC(c.x() + outerSign * w * 0.12, c.y() - h * 0.08);
        out.append(makeFill(QStringLiteral("%1_catch_star").arg(group),
                            group,
                            QStringLiteral("Highlights"),
                            starSparklePoly(starC, w * 0.042, h * 0.048),
                            QColor(255, 255, 255),
                            0.92,
                            QStringLiteral("accent"),
                            scleraId));
    }

    // 7. Upper Lash Line + Cat-Eye Wing Flick + Tapered Lash Clumps
    const QPointF lashStart(c.x() + innerSign * w * 0.46, c.y() + h * 0.02);
    const QPointF lashCtrl(c.x() + outerSign * w * 0.04, c.y() - h * 0.56);
    const QPointF lashEnd(c.x() + outerSign * w * 0.48, c.y() - h * 0.10);
    QVector<KisAiStrokePoint> upper = sampleQuad(lashStart, lashCtrl, lashEnd, 14, 0.45, 0.98);
    const QPointF flickCtrl(c.x() + outerSign * w * 0.55, c.y() - h * 0.22);
    const QPointF flickEnd(c.x() + outerSign * w * 0.60, c.y() - h * 0.31);
    upper.append(sampleQuad(lashEnd, flickCtrl, flickEnd, 5, 0.92, 0.22));
    out.append(makePath(QStringLiteral("%1_lash_upper").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        upper,
                        dark,
                        QStringLiteral("gpen"),
                        qMax<qreal>(0.0045, h * 0.095),
                        1.0,
                        QStringLiteral("contour")));

    QVector<KisAiStrokePoint> clump1 = sampleQuad(QPointF(c.x() + outerSign * w * 0.36, c.y() - h * 0.36),
                                                  QPointF(c.x() + outerSign * w * 0.46, c.y() - h * 0.47),
                                                  QPointF(c.x() + outerSign * w * 0.53, c.y() - h * 0.51),
                                                  6,
                                                  0.75,
                                                  0.18);
    out.append(makePath(QStringLiteral("%1_lash_clump_1").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        clump1,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0018,
                        0.95,
                        QStringLiteral("contour"),
                        QStringLiteral("%1_lash_upper").arg(group)));

    QVector<KisAiStrokePoint> clump2 = sampleQuad(QPointF(c.x() + outerSign * w * 0.45, c.y() - h * 0.21),
                                                  QPointF(c.x() + outerSign * w * 0.55, c.y() - h * 0.33),
                                                  QPointF(c.x() + outerSign * w * 0.62, c.y() - h * 0.35),
                                                  6,
                                                  0.68,
                                                  0.18);
    out.append(makePath(QStringLiteral("%1_lash_clump_2").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        clump2,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0015,
                        0.90,
                        QStringLiteral("contour"),
                        QStringLiteral("%1_lash_upper").arg(group)));

    // 8. Double-Eyelid Crease & Lower Lashes
    QVector<KisAiStrokePoint> crease = sampleQuad(QPointF(c.x() + innerSign * w * 0.30, c.y() - h * 0.58),
                                                  QPointF(c.x() + outerSign * w * 0.05, c.y() - h * 0.68),
                                                  QPointF(c.x() + outerSign * w * 0.38, c.y() - h * 0.53),
                                                  8,
                                                  0.35,
                                                  0.55);
    out.append(makePath(QStringLiteral("%1_crease").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        crease,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0014,
                        0.75,
                        QStringLiteral("internal")));

    QVector<KisAiStrokePoint> lower1 = sampleQuad(QPointF(c.x() + outerSign * w * 0.14, c.y() + h * 0.45),
                                                  QPointF(c.x() + outerSign * w * 0.27, c.y() + h * 0.47),
                                                  QPointF(c.x() + outerSign * w * 0.39, c.y() + h * 0.37),
                                                  6,
                                                  0.50,
                                                  0.18);
    out.append(makePath(QStringLiteral("%1_lash_lower_1").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        lower1,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0014,
                        0.72,
                        QStringLiteral("contour"),
                        scleraId));
    QVector<KisAiStrokePoint> lower2 = sampleQuad(QPointF(c.x() + outerSign * w * 0.31, c.y() + h * 0.43),
                                                  QPointF(c.x() + outerSign * w * 0.40, c.y() + h * 0.48),
                                                  QPointF(c.x() + outerSign * w * 0.45, c.y() + h * 0.52),
                                                  5,
                                                  0.45,
                                                  0.16);
    out.append(makePath(QStringLiteral("%1_lash_lower_2").arg(group),
                        group,
                        QStringLiteral("Lineart"),
                        lower2,
                        dark,
                        QStringLiteral("fineliner"),
                        0.0012,
                        0.65,
                        QStringLiteral("contour"),
                        scleraId));
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandMouth(const KisAiStrokeOperation &op,
                                                                  const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    const QString group = op.groupId.isEmpty() ? QStringLiteral("mouth") : op.groupId;
    const QPointF c = op.mouthCenter;
    const qreal w = op.mouthSize.width();
    const qreal h = op.mouthSize.height();
    if (w < 0.002 || h < 0.001)
        return out;

    const qreal halfW = w * 0.5;
    const QColor lip = op.mouthLipColor.isValid() ? op.mouthLipColor : QColor(225, 115, 125);
    const QColor ink = lip.darker(220);
    const QString expr = op.mouthExpression.toLower();
    const bool isOpen =
        (expr == QLatin1String("open_smile") || expr == QLatin1String("small_open") || expr == QLatin1String("open"));
    const bool isSmile = (expr == QLatin1String("smile") || expr == QLatin1String("open_smile"));
    const bool isCat = (expr == QLatin1String("cat_mouth"));

    if (isOpen) {
        const QString cavityId = QStringLiteral("%1_cavity").arg(group);
        const QPolygonF cavity = ellipsePoly(QPointF(c.x(), c.y() + h * 0.35), halfW * 0.82, h * 0.52, 20);
        out.append(makeLinearGradientFill(
            cavityId,
            group,
            QStringLiteral("Flats"),
            cavity,
            QPointF(c.x(), c.y() - h * 0.15),
            QPointF(c.x(), c.y() + h * 0.85),
            {lip.darker(320), lip.darker(240), lip.darker(180)},
            1.0,
            QStringLiteral("mass")));

        // Warm tongue dome clipped inside the oral cavity
        const QPointF tongueC(c.x(), c.y() + h * 0.62);
        out.append(makeRadialGradientFill(
            QStringLiteral("%1_tongue").arg(group),
            group,
            QStringLiteral("Flats"),
            ellipsePoly(tongueC, halfW * 0.62, h * 0.38, 16),
            tongueC,
            qMax(halfW, h) * 0.55,
            {lip.lighter(115), lip, lip.darker(135)},
            0.95,
            QStringLiteral("mass"),
            cavityId));
    }

    // Soft volumetric lip blush tint
    const QPolygonF lipBlush = ellipsePoly(QPointF(c.x(), c.y() + h * 0.18), halfW * 0.72, h * 0.38, 16);
    out.append(makeRadialGradientFill(
        QStringLiteral("%1_tint").arg(group),
        group,
        QStringLiteral("Flats"),
        lipBlush,
        QPointF(c.x(), c.y() + h * 0.15),
        qMax(halfW, h) * 0.65,
        {QColor(lip.red(), lip.green(), lip.blue(), 110),
         QColor(lip.red(), lip.green(), lip.blue(), 45),
         QColor(lip.red(), lip.green(), lip.blue(), 0)},
        0.55,
        QStringLiteral("wash")));

    QVector<KisAiStrokePoint> upper;
    if (isCat) {
        upper = sampleQuad(QPointF(c.x() - halfW, c.y()),
                           QPointF(c.x() - halfW * 0.5, c.y() - h * 0.4),
                           QPointF(c.x(), c.y()),
                           6,
                           0.35,
                           0.85);
        upper.append(sampleQuad(QPointF(c.x(), c.y()),
                                QPointF(c.x() + halfW * 0.5, c.y() - h * 0.4),
                                QPointF(c.x() + halfW, c.y()),
                                6,
                                0.85,
                                0.35));
    } else {
        const qreal arch = isSmile ? -h * 0.35 : (isOpen ? -h * 0.15 : 0.0);
        const QPointF left(c.x() - halfW, c.y() + (isSmile ? -h * 0.12 : 0.0));
        const QPointF right(c.x() + halfW, c.y() + (isSmile ? -h * 0.12 : 0.0));
        const QPointF peakL(c.x() - halfW * 0.25, c.y() + arch - h * 0.10);
        const QPointF centerDip(c.x(), c.y() + arch + h * 0.04);
        const QPointF peakR(c.x() + halfW * 0.25, c.y() + arch - h * 0.10);

        upper = sampleQuad(left, peakL, centerDip, 8, 0.28, 0.88);
        QVector<KisAiStrokePoint> rightHalf = sampleQuad(centerDip, peakR, right, 8, 0.88, 0.28);
        for (int i = 1; i < rightHalf.size(); ++i) {
            upper.append(rightHalf.at(i));
        }
    }
    const QString upperId = QStringLiteral("%1_upper_lip").arg(group);
    out.append(makePath(upperId,
                        group,
                        QStringLiteral("Lineart"),
                        upper,
                        ink,
                        QStringLiteral("gpen"),
                        0.0028,
                        1.0,
                        QStringLiteral("contour")));

    if (!upper.isEmpty()) {
        QVector<KisAiStrokePoint> cornerL;
        cornerL.append(KisAiStrokePoint(upper.first().pos.x() - halfW * 0.06, upper.first().pos.y() - h * 0.12, 0.25));
        cornerL.append(KisAiStrokePoint(upper.first().pos.x(), upper.first().pos.y(), 0.90));
        out.append(makePath(QStringLiteral("%1_corner_l").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            cornerL,
                            ink,
                            QStringLiteral("gpen"),
                            0.0024,
                            1.0,
                            QStringLiteral("accent"),
                            upperId));
        QVector<KisAiStrokePoint> cornerR;
        cornerR.append(KisAiStrokePoint(upper.last().pos.x(), upper.last().pos.y(), 0.90));
        cornerR.append(KisAiStrokePoint(upper.last().pos.x() + halfW * 0.06, upper.last().pos.y() - h * 0.12, 0.25));
        out.append(makePath(QStringLiteral("%1_corner_r").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            cornerR,
                            ink,
                            QStringLiteral("gpen"),
                            0.0024,
                            1.0,
                            QStringLiteral("accent"),
                            upperId));
    }

    if (!isOpen) {
        QVector<KisAiStrokePoint> lower = sampleQuad(QPointF(c.x() - halfW * 0.34, c.y() + h * 0.44),
                                                     QPointF(c.x(), c.y() + h * 0.56),
                                                     QPointF(c.x() + halfW * 0.34, c.y() + h * 0.44),
                                                     8,
                                                     0.32,
                                                     0.32);
        out.append(makePath(QStringLiteral("%1_lower_lip").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            lower,
                            lip.darker(155),
                            QStringLiteral("fineliner"),
                            0.0020,
                            0.72,
                            QStringLiteral("contour"),
                            upperId));

        // Soft under-lower-lip shadow cushion
        QPolygonF lowerShade;
        lowerShade.append(QPointF(c.x() - halfW * 0.24, c.y() + h * 0.56));
        lowerShade.append(QPointF(c.x() + halfW * 0.24, c.y() + h * 0.56));
        lowerShade.append(QPointF(c.x(), c.y() + h * 0.85));
        out.append(makeFill(QStringLiteral("%1_shadow").arg(group),
                            group,
                            QStringLiteral("Shading"),
                            lowerShade,
                            lip.darker(165),
                            0.28,
                            QStringLiteral("wash")));
    } else {
        // Open mouth lower lip contour arc
        QVector<KisAiStrokePoint> lowerOpen = sampleQuad(QPointF(c.x() - halfW * 0.52, c.y() + h * 0.72),
                                                         QPointF(c.x(), c.y() + h * 0.92),
                                                         QPointF(c.x() + halfW * 0.52, c.y() + h * 0.72),
                                                         8,
                                                         0.30,
                                                         0.30);
        out.append(makePath(QStringLiteral("%1_lower_lip").arg(group),
                            group,
                            QStringLiteral("Lineart"),
                            lowerOpen,
                            lip.darker(160),
                            QStringLiteral("fineliner"),
                            0.0020,
                            0.75,
                            QStringLiteral("contour"),
                            upperId));
    }

    if (op.mouthHasHighlight) {
        const QPointF gloss(c.x() + halfW * 0.12, c.y() + (isOpen ? h * 0.75 : h * 0.32));
        out.append(makeFill(QStringLiteral("%1_gloss").arg(group),
                            group,
                            QStringLiteral("Highlights"),
                            ellipsePoly(gloss, w * 0.040, h * 0.075, 12),
                            QColor(255, 250, 252),
                            0.85,
                            QStringLiteral("accent")));
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandHatch(const KisAiStrokeOperation &op,
                                                                  const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> out;
    if (op.polygon.size() < 3 || canvasSize.width() <= 0)
        return out;
    if (op.spacing <= 0.0)
        return out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("hatch") : op.id) : op.groupId;
    const QRectF b = op.polygon.boundingRect();
    if (b.width() <= 0.0 || b.height() <= 0.0)
        return out;
    const qreal minDim = qMax<qreal>(1.0, qMin(canvasSize.width(), canvasSize.height()));
    const qreal spacing = qMax<qreal>(0.004, op.spacing);
    const qreal radius = std::hypot(b.width(), b.height()) * 0.55;
    const int rawLines = qRound(radius * 2.0 / qMax<qreal>(0.002, spacing));
    const int numLines = qBound(2, rawLines, 48);
    const qreal effective = numLines > 0 ? qMax(spacing, radius * 2.0 / numLines) : spacing;
    const QPointF center = b.center();
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/hatch")));
    const qreal lineSize = (op.brush.sizeMode == QLatin1String("px")) ? qMax<qreal>(0.5, op.brush.size * 0.20)
                                                                      : qMax<qreal>(0.001, op.brush.size * 0.20);
    Q_UNUSED(minDim);

    auto emitPass = [&](qreal angleDeg, const QString &suffix) {
        const qreal rad = angleDeg * PI / 180.0;
        const QPointF dir(std::cos(rad), std::sin(rad));
        const QPointF norm(-std::sin(rad), std::cos(rad));
        int emitted = 0;
        for (int i = -numLines; i <= numLines && emitted < 64; ++i) {
            const qreal wobble = (rng.generateDouble() - 0.5) * effective * 0.15;
            const QPointF mid = center + norm * (i * effective);
            const QPointF p1 = mid - dir * radius + norm * wobble;
            const QPointF p2 = mid + dir * radius - norm * wobble;
            QVector<KisAiStrokePoint> pts;
            pts.append(KisAiStrokePoint(p1.x(), p1.y(), 0.50));
            pts.append(KisAiStrokePoint(p2.x(), p2.y(), 0.50));
            KisAiStrokeOperation line = makePath(QStringLiteral("%1_line_%2_%3").arg(group, suffix).arg(emitted),
                                                 group,
                                                 op.layer.isEmpty() ? QStringLiteral("Shading") : op.layer,
                                                 pts,
                                                 op.brush.color,
                                                 QStringLiteral("fineliner"),
                                                 lineSize,
                                                 qBound<qreal>(0.05, op.brush.opacity * 0.70, 1.0),
                                                 QStringLiteral("internal"));
            line.clipToId = op.id;
            line.polygon = op.polygon;
            line.brush.isEraser = op.brush.isEraser;
            line.brush.sizeMode = op.brush.sizeMode;
            out.append(line);
            ++emitted;
        }
    };

    emitPass(op.angleDeg, QStringLiteral("a"));
    if (op.crossHatch)
        emitPass(op.angleDeg + 90.0, QStringLiteral("b"));
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandMangaLines(const KisAiStrokeOperation &op,
                                                                       const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    if (op.density <= 0)
        return out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("manga") : op.id) : op.groupId;
    const QPointF center = op.gradientCenter;
    const int count = qBound(4, op.density, 64);
    const qreal jitter = qBound<qreal>(0.0, op.lineLengthJitter, 0.8);
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id + QStringLiteral("/manga")));
    for (int i = 0; i < count; ++i) {
        const qreal baseAngle = (2.0 * PI * i) / count;
        const qreal angle = baseAngle + (rng.generateDouble() - 0.5) * (2.0 * PI / count) * 0.4;
        const qreal cosA = std::cos(angle);
        const qreal sinA = std::sin(angle);
        const qreal innerDist = op.innerRadius * (1.0 + (rng.generateDouble() - 0.5) * jitter * 0.8);
        const qreal outerDist = op.outerRadius * (1.0 + (rng.generateDouble() - 0.5) * jitter);
        // Multi-point tapered path (needle tip at innerDist -> bold outer edge)
        const qreal midDist = innerDist + (outerDist - innerDist) * 0.45;
        QVector<KisAiStrokePoint> pts;
        pts.append(KisAiStrokePoint(center.x() + cosA * innerDist, center.y() + sinA * innerDist, 0.12));
        pts.append(KisAiStrokePoint(center.x() + cosA * midDist, center.y() + sinA * midDist, 0.55));
        pts.append(KisAiStrokePoint(center.x() + cosA * outerDist, center.y() + sinA * outerDist, 0.95));
        out.append(makePath(QStringLiteral("%1_ray_%2").arg(group).arg(i),
                            group,
                            op.layer.isEmpty() ? QStringLiteral("FX") : op.layer,
                            pts,
                            op.brush.color,
                            QStringLiteral("gpen"),
                            qMax<qreal>(0.0018, op.brush.size),
                            op.brush.opacity,
                            QStringLiteral("accent")));
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandParticles(const KisAiStrokeOperation &op,
                                                                      const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<KisAiStrokeOperation> out;
    if (op.particleCount <= 0)
        return out;
    const QString group = op.groupId.isEmpty() ? (op.id.isEmpty() ? QStringLiteral("particles") : op.id) : op.groupId;
    const QRectF bounds = op.bounds.isValid() ? op.bounds : QRectF(0.0, 0.0, 1.0, 1.0);
    const int count = qBound(1, op.particleCount, 60);
    const QString shape = op.particleShape.trimmed().toLower();
    const QString targetLayer = op.layer.isEmpty() ? QStringLiteral("FX") : op.layer;
    const QColor baseColor = op.brush.color.isValid() ? op.brush.color : QColor(255, 220, 235);
    QRandomGenerator rng(KisAiStrokeProgramCodec::stableSeed(op.id.isEmpty() ? QStringLiteral("particles") : op.id));

    for (int i = 0; i < count; ++i) {
        const qreal x = bounds.left() + rng.generateDouble() * bounds.width();
        const qreal y = bounds.top() + rng.generateDouble() * bounds.height();
        const qreal depthScale = 0.55 + rng.generateDouble() * 0.90;
        const qreal r = qMax<qreal>(0.003, (op.brush.size > 0.0 ? op.brush.size : 0.006) * depthScale);
        const qreal alpha = qBound<qreal>(0.08, op.brush.opacity * (0.45 + 0.50 * depthScale), 0.95);
        const QPointF pt(x, y);

        if (shape == QLatin1String("petal") || shape == QLatin1String("sakura")) {
            const qreal angleRad = rng.generateDouble() * 2.0 * PI;
            const QPolygonF petal = sakuraPetalPoly(pt, r * 1.35, angleRad);
            const QColor petalCol = (i % 3 == 0) ? baseColor.lighter(115) : baseColor;
            KisAiStrokeOperation pOp = makeFill(QStringLiteral("%1_dot_%2").arg(group).arg(i),
                                                group,
                                                targetLayer,
                                                petal,
                                                petalCol,
                                                alpha,
                                                QStringLiteral("accent"));
            pOp.fillStyle = QStringLiteral("wash");
            out.append(pOp);
        } else if (shape == QLatin1String("sparkle") || shape == QLatin1String("star")) {
            const QPolygonF star = starSparklePoly(pt, r * 1.5, r * 1.5);
            KisAiStrokeOperation sOp = makeFill(QStringLiteral("%1_dot_%2").arg(group).arg(i),
                                                group,
                                                targetLayer,
                                                star,
                                                baseColor.lighter(125),
                                                alpha,
                                                QStringLiteral("accent"));
            sOp.blendMode = QStringLiteral("screen");
            out.append(sOp);
        } else if (shape == QLatin1String("bokeh")) {
            const QPolygonF circle = ellipsePoly(pt, r * 1.8, r * 1.8, 16);
            out.append(makeRadialGradientFill(
                QStringLiteral("%1_dot_%2").arg(group).arg(i),
                group,
                targetLayer,
                circle,
                pt,
                r * 1.8,
                {QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 140),
                 QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 220),
                 QColor(baseColor.red(), baseColor.green(), baseColor.blue(), 0)},
                alpha * 0.75,
                QStringLiteral("accent"),
                QString(),
                QStringLiteral("screen")));
        } else {
            out.append(makeFill(QStringLiteral("%1_dot_%2").arg(group).arg(i),
                                group,
                                targetLayer,
                                ellipsePoly(pt, r, r, 12),
                                baseColor,
                                alpha,
                                QStringLiteral("accent")));
        }
    }
    return out;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandBezierPath(const KisAiStrokeOperation &op,
                                                                       const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    QVector<QPointF> rawPts = op.bezierControlPoints;
    if (rawPts.isEmpty()) {
        for (const KisAiStrokePoint &p : op.points) {
            rawPts.append(p.pos);
        }
    }
    if (rawPts.size() < 2) {
        return {op};
    }

    QVector<KisAiStrokePoint> sampledPts;
    const int n = rawPts.size();

    // Sample smooth cubic or piecewise quadratic segments
    if (n == 4) {
        // Single cubic Bézier: P0, P1, P2, P3
        constexpr int kSteps = 24;
        const QPointF p0 = rawPts[0];
        const QPointF p1 = rawPts[1];
        const QPointF p2 = rawPts[2];
        const QPointF p3 = rawPts[3];
        sampledPts.reserve(kSteps + 1);
        for (int i = 0; i <= kSteps; ++i) {
            const qreal t = qreal(i) / qreal(kSteps);
            const qreal it = 1.0 - t;
            const QPointF pt = it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
            const qreal pr = 0.25 + 0.65 * std::sin(t * PI);
            sampledPts.append(KisAiStrokePoint(pt.x(), pt.y(), pr));
        }
    } else if (n >= 4 && (n - 1) % 3 == 0) {
        // Multi-segment cubic Bézier: P0, C1, C2, P1, C3, C4, P2, ...
        const int numSegments = (n - 1) / 3;
        constexpr int kStepsPerSeg = 16;
        sampledPts.reserve(numSegments * kStepsPerSeg + 1);
        for (int s = 0; s < numSegments; ++s) {
            const QPointF p0 = rawPts[s * 3];
            const QPointF p1 = rawPts[s * 3 + 1];
            const QPointF p2 = rawPts[s * 3 + 2];
            const QPointF p3 = rawPts[s * 3 + 3];
            for (int i = (s == 0 ? 0 : 1); i <= kStepsPerSeg; ++i) {
                const qreal t = qreal(i) / qreal(kStepsPerSeg);
                const qreal it = 1.0 - t;
                const QPointF pt =
                    it * it * it * p0 + 3.0 * it * it * t * p1 + 3.0 * it * t * t * p2 + t * t * t * p3;
                const qreal globalT = (qreal(s) + t) / qreal(numSegments);
                const qreal pr = 0.20 + 0.70 * std::sin(globalT * PI);
                sampledPts.append(KisAiStrokePoint(pt.x(), pt.y(), pr));
            }
        }
    } else {
        // Arbitrary N control points: evaluate Catmull-Rom spline through points
        constexpr int kStepsPerSeg = 12;
        sampledPts.reserve((n - 1) * kStepsPerSeg + 1);
        for (int i = 0; i < n - 1; ++i) {
            const QPointF p0 = (i == 0) ? rawPts[0] : rawPts[i - 1];
            const QPointF p1 = rawPts[i];
            const QPointF p2 = rawPts[i + 1];
            const QPointF p3 = (i + 2 < n) ? rawPts[i + 2] : rawPts[i + 1];
            for (int s = (i == 0 ? 0 : 1); s <= kStepsPerSeg; ++s) {
                const qreal t = qreal(s) / qreal(kStepsPerSeg);
                const qreal t2 = t * t;
                const qreal t3 = t2 * t;
                const QPointF pt = 0.5
                    * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2
                       + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
                const qreal globalT = (qreal(i) + t) / qreal(n - 1);
                const qreal pr = 0.20 + 0.70 * std::sin(globalT * PI);
                sampledPts.append(KisAiStrokePoint(pt.x(), pt.y(), pr));
            }
        }
    }

    const QString group = op.groupId.isEmpty() ? KisAiStrokeGraph::inferGroupId(op) : op.groupId;
    KisAiStrokeOperation pathOp = makePath(op.id.isEmpty() ? QStringLiteral("bezier_path") : op.id,
                                           group,
                                           op.layer.isEmpty() ? QStringLiteral("Lineart") : op.layer,
                                           sampledPts,
                                           op.brush.color,
                                           op.brush.profile.isEmpty() ? QStringLiteral("gpen") : op.brush.profile,
                                           op.brush.size > 0.0 ? op.brush.size : 0.0035,
                                           op.brush.opacity > 0.0 ? op.brush.opacity : 1.0,
                                           op.role.isEmpty() ? QStringLiteral("contour") : op.role,
                                           op.parentId);
    pathOp.blendMode = op.blendMode;
    pathOp.clipToId = op.clipToId;
    return {pathOp};
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandParametricShape(const KisAiStrokeOperation &op,
                                                                           const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    const QPointF center = op.shapeCenter;
    const qreal w = op.shapeSize.isValid() && op.shapeSize.width() > 0.0
        ? op.shapeSize.width()
        : (op.shapeRadius > 0.0 ? op.shapeRadius * 2.0 : 0.10);
    const qreal h = op.shapeSize.isValid() && op.shapeSize.height() > 0.0
        ? op.shapeSize.height()
        : (op.shapeRadius > 0.0 ? op.shapeRadius * 2.0 : 0.10);
    const qreal angleRad = op.shapeAngleDeg * PI / 180.0;
    const qreal cosA = std::cos(angleRad);
    const qreal sinA = std::sin(angleRad);

    const auto rotatePt = [center, cosA, sinA](const QPointF &pt) -> QPointF {
        const qreal dx = pt.x() - center.x();
        const qreal dy = pt.y() - center.y();
        return QPointF(center.x() + dx * cosA - dy * sinA, center.y() + dx * sinA + dy * cosA);
    };

    QPolygonF poly;
    const QString type = op.shapeType.toLower().trimmed();
    if (type == QLatin1String("circle") || type == QLatin1String("ellipse") || type.isEmpty()) {
        const qreal rx = w * 0.5;
        const qreal ry = h * 0.5;
        constexpr int kSteps = 32;
        poly.reserve(kSteps);
        for (int i = 0; i < kSteps; ++i) {
            const qreal a = 2.0 * PI * qreal(i) / qreal(kSteps);
            const QPointF rawPt(center.x() + std::cos(a) * rx, center.y() + std::sin(a) * ry);
            poly.append(rotatePt(rawPt));
        }
    } else if (type == QLatin1String("rectangle") || type == QLatin1String("rect")) {
        const qreal hx = w * 0.5;
        const qreal hy = h * 0.5;
        poly.append(rotatePt(QPointF(center.x() - hx, center.y() - hy)));
        poly.append(rotatePt(QPointF(center.x() + hx, center.y() - hy)));
        poly.append(rotatePt(QPointF(center.x() + hx, center.y() + hy)));
        poly.append(rotatePt(QPointF(center.x() - hx, center.y() + hy)));
    } else if (type == QLatin1String("capsule")) {
        const qreal r = qMin(w, h) * 0.5;
        const qreal hx = (w > h) ? (w * 0.5 - r) : 0.0;
        const qreal hy = (h > w) ? (h * 0.5 - r) : 0.0;
        constexpr int kHalfSteps = 16;
        if (w >= h) {
            for (int i = 0; i <= kHalfSteps; ++i) {
                const qreal a = -PI * 0.5 + PI * qreal(i) / qreal(kHalfSteps);
                poly.append(rotatePt(QPointF(center.x() + hx + std::cos(a) * r, center.y() + std::sin(a) * r)));
            }
            for (int i = 0; i <= kHalfSteps; ++i) {
                const qreal a = PI * 0.5 + PI * qreal(i) / qreal(kHalfSteps);
                poly.append(rotatePt(QPointF(center.x() - hx + std::cos(a) * r, center.y() + std::sin(a) * r)));
            }
        } else {
            for (int i = 0; i <= kHalfSteps; ++i) {
                const qreal a = 0.0 + PI * qreal(i) / qreal(kHalfSteps);
                poly.append(rotatePt(QPointF(center.x() + std::cos(a) * r, center.y() + hy + std::sin(a) * r)));
            }
            for (int i = 0; i <= kHalfSteps; ++i) {
                const qreal a = PI + PI * qreal(i) / qreal(kHalfSteps);
                poly.append(rotatePt(QPointF(center.x() + std::cos(a) * r, center.y() - hy + std::sin(a) * r)));
            }
        }
    } else if (type == QLatin1String("star")) {
        constexpr int kPoints = 5;
        const qreal rOuter = qMax(w, h) * 0.5;
        const qreal rInner = rOuter * 0.45;
        for (int i = 0; i < kPoints * 2; ++i) {
            const qreal a = -PI * 0.5 + PI * qreal(i) / qreal(kPoints);
            const qreal r = (i % 2 == 0) ? rOuter : rInner;
            poly.append(rotatePt(QPointF(center.x() + std::cos(a) * r, center.y() + std::sin(a) * r)));
        }
    } else {
        const qreal rx = w * 0.5;
        const qreal ry = h * 0.5;
        constexpr int kSteps = 16;
        for (int i = 0; i < kSteps; ++i) {
            const qreal a = 2.0 * PI * qreal(i) / qreal(kSteps);
            poly.append(rotatePt(QPointF(center.x() + std::cos(a) * rx, center.y() + std::sin(a) * ry)));
        }
    }

    QVector<KisAiStrokeOperation> result;
    const QString group = op.groupId.isEmpty() ? KisAiStrokeGraph::inferGroupId(op) : op.groupId;
    const QString baseId = op.id.isEmpty() ? QStringLiteral("shape") : op.id;

    if (op.shapeFilled) {
        KisAiStrokeOperation fillOp = makeFill(baseId,
                                               group,
                                               op.layer.isEmpty() ? QStringLiteral("Flats") : op.layer,
                                               poly,
                                               op.brush.color,
                                               op.brush.opacity > 0.0 ? op.brush.opacity : 1.0,
                                               QStringLiteral("shape_fill"),
                                               op.clipToId);
        fillOp.blendMode = op.blendMode;
        result.append(fillOp);
    }

    if (op.brush.size > 0.001 || op.layer == QLatin1String("Lineart")) {
        QVector<KisAiStrokePoint> strokePts;
        strokePts.reserve(poly.size() + 1);
        for (const QPointF &pt : poly) {
            strokePts.append(KisAiStrokePoint(pt.x(), pt.y(), 0.8));
        }
        if (!poly.isEmpty()) {
            strokePts.append(KisAiStrokePoint(poly.first().x(), poly.first().y(), 0.8));
        }
        KisAiStrokeOperation lineOp = makePath(baseId + QStringLiteral("_contour"),
                                               group,
                                               op.layer.isEmpty() ? QStringLiteral("Lineart") : op.layer,
                                               strokePts,
                                               op.brush.color,
                                               QStringLiteral("gpen"),
                                               op.brush.size > 0.0 ? op.brush.size : 0.003,
                                               op.brush.opacity > 0.0 ? op.brush.opacity : 1.0,
                                               QStringLiteral("contour"),
                                               baseId);
        lineOp.closed = true;
        result.append(lineOp);
    }

    return result.isEmpty() ? QVector<KisAiStrokeOperation>{op} : result;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandFormShading(const KisAiStrokeOperation &op,
                                                                       const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    if (op.polygon.size() < 3) {
        return {op};
    }

    const QPointF lightPos = op.lightSourcePos.isNull() ? QPointF(0.25, 0.15) : op.lightSourcePos;
    const QRectF bounds = op.polygon.boundingRect();
    const QPointF polyCenter = bounds.center();
    QPointF lightDir = polyCenter - lightPos;
    const qreal dist = std::sqrt(lightDir.x() * lightDir.x() + lightDir.y() * lightDir.y());
    if (dist > 1.0e-5) {
        lightDir /= dist;
    } else {
        lightDir = QPointF(0.5, 0.866);
    }

    const qreal shadowOffsetMag = qBound<qreal>(0.005, op.featherWidth > 0.0 ? op.featherWidth : 0.02, 0.08);
    const QPointF offset = lightDir * shadowOffsetMag;

    QPolygonF coreShadowPoly;
    coreShadowPoly.reserve(op.polygon.size());
    for (const QPointF &pt : op.polygon) {
        const QPointF fromCenter = pt - polyCenter;
        const qreal dot = fromCenter.x() * lightDir.x() + fromCenter.y() * lightDir.y();
        if (dot > 0.0) {
            coreShadowPoly.append(pt);
        } else {
            coreShadowPoly.append(polyCenter + fromCenter * 0.4 + offset);
        }
    }

    const QString group = op.groupId.isEmpty() ? KisAiStrokeGraph::inferGroupId(op) : op.groupId;
    const QString baseId = op.id.isEmpty() ? QStringLiteral("form_shading") : op.id;

    KisAiStrokeOperation shadowOp =
        makeFill(baseId,
                 group,
                 op.layer.isEmpty() ? QStringLiteral("Shading") : op.layer,
                 coreShadowPoly.size() >= 3 ? coreShadowPoly : op.polygon,
                 op.brush.color,
                 qBound<qreal>(0.05,
                               op.shadingIntensity * (op.brush.opacity > 0.0 ? op.brush.opacity : 1.0),
                               1.0),
                 QStringLiteral("form_shadow"),
                 op.clipToId);
    shadowOp.blendMode = (op.blendMode.isEmpty() || op.blendMode == QLatin1String("normal"))
        ? QStringLiteral("multiply")
        : op.blendMode;
    shadowOp.fillStyle = QStringLiteral("wash");
    shadowOp.style = QStringLiteral("wash");
    return {shadowOp};
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandTextureHatch(const KisAiStrokeOperation &op,
                                                                        const QSize &canvasSize)
{
    Q_UNUSED(canvasSize);
    if (op.polygon.size() < 3) {
        return {op};
    }

    const QRectF bounds = op.polygon.boundingRect();
    const qreal spacing = qBound<qreal>(0.004, op.spacing > 0.0 ? op.spacing : 0.012, 0.08);
    const qreal angleRad = op.angleDeg * PI / 180.0;
    const qreal cosA = std::cos(angleRad);
    const qreal sinA = std::sin(angleRad);

    QVector<KisAiStrokeOperation> result;
    const QString group = op.groupId.isEmpty() ? KisAiStrokeGraph::inferGroupId(op) : op.groupId;
    const QString baseId = op.id.isEmpty() ? QStringLiteral("tex_hatch") : op.id;
    const QColor color = op.brush.color.isValid() ? op.brush.color : QColor(30, 20, 40);
    const qreal brushSize = op.brush.size > 0.0 ? op.brush.size : 0.0018;

    const qreal diag = std::sqrt(bounds.width() * bounds.width() + bounds.height() * bounds.height());
    const QPointF center = bounds.center();

    int lineIdx = 0;
    for (qreal d = -diag * 0.6; d <= diag * 0.6; d += spacing) {
        const QPointF pMid(center.x() + d * -sinA, center.y() + d * cosA);
        const QPointF p0(pMid.x() - diag * 0.6 * cosA, pMid.y() - diag * 0.6 * sinA);
        const QPointF p1(pMid.x() + diag * 0.6 * cosA, pMid.y() + diag * 0.6 * sinA);

        const qreal clampedX0 = qBound(bounds.left(), p0.x(), bounds.right());
        const qreal clampedY0 = qBound(bounds.top(), p0.y(), bounds.bottom());
        const qreal clampedX1 = qBound(bounds.left(), p1.x(), bounds.right());
        const qreal clampedY1 = qBound(bounds.top(), p1.y(), bounds.bottom());

        if (std::abs(clampedX1 - clampedX0) > 1.0e-4 || std::abs(clampedY1 - clampedY0) > 1.0e-4) {
            QVector<KisAiStrokePoint> pts;
            pts.append(KisAiStrokePoint(clampedX0, clampedY0, 0.7));
            pts.append(KisAiStrokePoint(clampedX1, clampedY1, 0.7));

            KisAiStrokeOperation lineOp = makePath(QStringLiteral("%1_%2").arg(baseId).arg(lineIdx++),
                                                   group,
                                                   op.layer.isEmpty() ? QStringLiteral("Shading") : op.layer,
                                                   pts,
                                                   color,
                                                   QStringLiteral("fineliner"),
                                                   brushSize,
                                                   op.brush.opacity > 0.0 ? op.brush.opacity : 0.8,
                                                   QStringLiteral("hatch_line"),
                                                   baseId);
            lineOp.clipToId = op.clipToId;
            result.append(lineOp);
        }
        if (result.size() >= 80)
            break;
    }

    return result.isEmpty() ? QVector<KisAiStrokeOperation>{op} : result;
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expand(const KisAiStrokeOperation &op, const QSize &canvasSize)
{
    switch (op.kind) {
    case KisAiStrokeOperation::Kind::AnimeEye:
        return expandEye(op, canvasSize);
    case KisAiStrokeOperation::Kind::AnimeMouth:
        return expandMouth(op, canvasSize);
    case KisAiStrokeOperation::Kind::Hatch:
        return expandHatch(op, canvasSize);
    case KisAiStrokeOperation::Kind::MangaLines:
        return expandMangaLines(op, canvasSize);
    case KisAiStrokeOperation::Kind::Particles:
        return expandParticles(op, canvasSize);
    case KisAiStrokeOperation::Kind::BezierPath:
        return expandBezierPath(op, canvasSize);
    case KisAiStrokeOperation::Kind::ParametricShape:
        return expandParametricShape(op, canvasSize);
    case KisAiStrokeOperation::Kind::FormShading:
        return expandFormShading(op, canvasSize);
    case KisAiStrokeOperation::Kind::TextureHatch:
        return expandTextureHatch(op, canvasSize);
    default: {
        KisAiStrokeOperation annotated = op;
        if (annotated.groupId.isEmpty())
            annotated.groupId = KisAiStrokeGraph::inferGroupId(op);
        if (annotated.parentId.isEmpty())
            annotated.parentId = KisAiStrokeGraph::inferParentId(op);
        if (annotated.role.isEmpty() || annotated.role == QLatin1String("auto"))
            annotated.role = KisAiStrokeGraph::inferRole(op);
        return {annotated};
    }
    }
}

QVector<KisAiStrokeOperation> KisAiPrimitiveExpander::expandAll(const QVector<KisAiStrokeOperation> &ops,
                                                                const QSize &canvasSize)
{
    QVector<KisAiStrokeOperation> out;
    out.reserve(ops.size() * 4);
    for (const KisAiStrokeOperation &op : ops)
        out.append(expand(op, canvasSize));
    return out;
}
