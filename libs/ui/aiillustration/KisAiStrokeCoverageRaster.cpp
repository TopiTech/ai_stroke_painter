/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeCoverageRaster.h"

#include "KisAiStrokeQualityUtils.h"

#include <QImage>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRandomGenerator>

#include <cmath>
#include <functional>

using KisAiStrokeCoverageRaster::StrokeSample;

namespace
{
constexpr qreal PI = 3.14159265358979323846;

qreal knotInterval(const QPointF &a, const QPointF &b)
{
    // alpha=0.5 (centripetal): (squared distance)^(alpha/2).
    const QPointF delta = b - a;
    return qMax<qreal>(1.0e-4, std::pow(delta.x() * delta.x() + delta.y() * delta.y(), 0.25));
}

QPointF interpolateAtKnot(const QPointF &a, const QPointF &b, qreal ta, qreal tb, qreal t)
{
    const qreal denominator = tb - ta;
    if (qAbs(denominator) < 1.0e-8)
        return a;
    return a * ((tb - t) / denominator) + b * ((t - ta) / denominator);
}

QPointF centripetalPoint(const QPointF &p0, const QPointF &p1, const QPointF &p2, const QPointF &p3, qreal u)
{
    const qreal t0 = 0.0;
    const qreal t1 = t0 + knotInterval(p0, p1);
    const qreal t2 = t1 + knotInterval(p1, p2);
    const qreal t3 = t2 + knotInterval(p2, p3);
    const qreal t = t1 + qBound<qreal>(0.0, u, 1.0) * (t2 - t1);

    const QPointF a1 = interpolateAtKnot(p0, p1, t0, t1, t);
    const QPointF a2 = interpolateAtKnot(p1, p2, t1, t2, t);
    const QPointF a3 = interpolateAtKnot(p2, p3, t2, t3, t);
    const QPointF b1 = interpolateAtKnot(a1, a2, t0, t2, t);
    const QPointF b2 = interpolateAtKnot(a2, a3, t1, t3, t);
    return interpolateAtKnot(b1, b2, t1, t2, t);
}

QColor grayLevel(qreal factor)
{
    const int g = qBound(0, qRound(qBound<qreal>(0.0, factor, 1.0) * 255.0), 255);
    return QColor(g, g, g);
}

void computeFrames(const QVector<StrokeSample> &samples,
                   bool closed,
                   QVector<QPointF> &tangents,
                   QVector<QPointF> &normals)
{
    const int n = samples.size();
    tangents.resize(n);
    normals.resize(n);
    // at(1) 参照を含むため 1点未満では接線計算不能 (paintStroke は n==0 のみ弾く)。
    if (n < 2) {
        if (n == 1) {
            tangents[0] = QPointF(1.0, 0.0);
            normals[0] = QPointF(0.0, 1.0);
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        QPointF t;
        if (closed && n > 2) {
            t = samples.at((i + 1) % n).pos - samples.at((i - 1 + n) % n).pos;
        } else if (i == 0) {
            t = samples.at(1).pos - samples.at(0).pos;
        } else if (i == n - 1) {
            t = samples.at(n - 1).pos - samples.at(n - 2).pos;
        } else {
            t = samples.at(i + 1).pos - samples.at(i - 1).pos;
        }
        const qreal len = std::hypot(t.x(), t.y());
        if (len > 1.0e-5) {
            t /= len;
        } else {
            t = QPointF(1.0, 0.0);
        }
        tangents[i] = t;
        normals[i] = QPointF(-t.y(), t.x());
    }
}

QVector<QPointF> chainPoints(const QVector<StrokeSample> &samples, const QVector<QPointF> &normals, bool left)
{
    QVector<QPointF> chain;
    chain.reserve(samples.size());
    for (int i = 0; i < samples.size(); ++i) {
        const qreal halfW = qMax<qreal>(0.35, samples.at(i).width * 0.5);
        chain.append(left ? samples.at(i).pos + normals.at(i) * halfW : samples.at(i).pos - normals.at(i) * halfW);
    }
    return chain;
}

void drawChainRim(QPainter &target,
                  const QVector<StrokeSample> &samples,
                  const QVector<QPointF> &normals,
                  const QBrush &rimBrush,
                  qreal penWidth)
{
    const QVector<QPointF> left = chainPoints(samples, normals, true);
    const QVector<QPointF> right = chainPoints(samples, normals, false);
    target.setBrush(Qt::NoBrush);
    target.setPen(QPen(rimBrush, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (left.size() >= 2) {
        target.drawPolyline(left);
    }
    if (right.size() >= 2) {
        target.drawPolyline(right);
    }
    // Rim seams at joins are healed with small cap disks (idempotent under Lighten).
    target.setPen(Qt::NoPen);
    for (const StrokeSample &s : samples) {
        const qreal r = qMax<qreal>(0.3, s.width * 0.25);
        target.drawEllipse(s.pos, r, r);
    }
}

} // namespace

KisAiStrokeCoverageRaster::TextureStyle KisAiStrokeCoverageRaster::textureStyleForProfile(const QString &profile)
{
    const QString p = profile.toLower();
    if (p == QLatin1String("airbrush"))
        return KisAiStrokeCoverageRaster::TextureStyle::Airbrush;
    if (p == QLatin1String("watercolor"))
        return KisAiStrokeCoverageRaster::TextureStyle::Watercolor;
    if (p == QLatin1String("brush"))
        return KisAiStrokeCoverageRaster::TextureStyle::Bristle;
    if (p == QLatin1String("pencil"))
        return KisAiStrokeCoverageRaster::TextureStyle::Pencil;
    if (p == QLatin1String("charcoal"))
        return KisAiStrokeCoverageRaster::TextureStyle::Charcoal;
    if (p == QLatin1String("crayon"))
        return KisAiStrokeCoverageRaster::TextureStyle::Crayon;
    if (p == QLatin1String("marker"))
        return KisAiStrokeCoverageRaster::TextureStyle::Marker;
    if (p == QLatin1String("neon"))
        return KisAiStrokeCoverageRaster::TextureStyle::Neon;
    if (p == QLatin1String("splatter"))
        return KisAiStrokeCoverageRaster::TextureStyle::Splatter;
    if (p == QLatin1String("stipple"))
        return KisAiStrokeCoverageRaster::TextureStyle::Stipple;
    if (p == QLatin1String("feathering"))
        return KisAiStrokeCoverageRaster::TextureStyle::Feathering;
    return KisAiStrokeCoverageRaster::TextureStyle::Solid;
}

QVector<StrokeSample> KisAiStrokeCoverageRaster::sampleStroke(const QVector<QPointF> &scaledPts,
                                                              const QVector<qreal> &pressures,
                                                              bool closed,
                                                              bool smooth,
                                                              const KisAiStrokeBrush &brush,
                                                              const QSize &workingSize,
                                                              int supersampleScale)
{
    QVector<StrokeSample> samples;
    const int n = scaledPts.size();
    if (n < 2) {
        return samples;
    }

    const int segments = closed ? n : (n - 1);

    qreal totalLen = 0.0;
    for (int i = 0; i < segments; ++i) {
        const QPointF &a = scaledPts.at(i);
        const QPointF &b = scaledPts.at((i + 1) % n);
        totalLen += std::hypot(b.x() - a.x(), b.y() - a.y());
    }
    const qreal segLenAvg = segments > 0 ? totalLen / segments : 1.0;

    samples.reserve(segments * 12 + 2);
    const qreal flatnessPx = 0.15 * qMax(1, supersampleScale);

    const auto safePressure = [&](int idx) -> qreal {
        if (idx >= 0 && idx < pressures.size()) {
            return pressures.at(idx);
        }
        return 0.8;
    };

    for (int i = 0; i < segments; ++i) {
        QPointF p0, p1, p2, p3;
        qreal pr1, pr2;

        if (closed) {
            p0 = scaledPts.at((i - 1 + n) % n);
            p1 = scaledPts.at(i);
            pr1 = safePressure(i);
            p2 = scaledPts.at((i + 1) % n);
            pr2 = safePressure((i + 1) % n);
            p3 = scaledPts.at((i + 2) % n);
        } else {
            p1 = scaledPts.at(i);
            pr1 = safePressure(i);
            p2 = scaledPts.at(i + 1);
            pr2 = safePressure(i + 1);
            p0 = (i > 0) ? scaledPts.at(i - 1) : (p1 + (p1 - p2));
            p3 = (i + 2 < n) ? scaledPts.at(i + 2) : (p2 + (p2 - p1));
        }

        const qreal segmentLength = std::hypot(p2.x() - p1.x(), p2.y() - p1.y());

        const auto widthAt = [&](qreal u) {
            // Smoothstep is monotone, so malformed pressure anchors cannot
            // create negative widths or bulges between samples.
            const qreal pressureT = smooth ? u * u * (3.0 - 2.0 * u) : u;
            const qreal p = pr1 + (pr2 - pr1) * pressureT;

            const qreal globalT = (qreal(i) + u) / qreal(segments);
            const qreal taper = KisAiStrokeQualityUtils::calculateTaper(globalT, brush.profile, closed);

            const qreal speedRatio = segLenAvg > 1.0e-6 ? segmentLength / segLenAvg : 1.0;
            const qreal speedGain = qBound<qreal>(0.88, 1.0 + (1.0 - qMin<qreal>(speedRatio, 2.0)) * 0.10, 1.08);

            qreal width = KisAiStrokeQualityUtils::effectiveWidthPx(brush, p * taper, workingSize, supersampleScale)
                * speedGain;

            if (brush.profile.compare(QLatin1String("calligraphy"), Qt::CaseInsensitive) == 0) {
                const QPointF tangent = (i < segments - 1) ? (scaledPts.at(i + 1) - scaledPts.at(i)) : (p2 - p1);
                width = KisAiStrokeQualityUtils::calculateCalligraphyWidth(tangent, width, 45.0, 0.20);
            }
            return width;
        };

        const auto pushSample = [&](qreal u) {
            const QPointF position = (smooth && n >= 3) ? centripetalPoint(p0, p1, p2, p3, u) : p1 + (p2 - p1) * u;
            samples.append({position, widthAt(u)});
        };

        // V10: flatness-driven subdivision — each span is split recursively
        // until its chord midpoint deviation in BOTH position and width drops
        // below budget (depth capped at 7 => max 128 chords per span). Width
        // participates so the taper profile survives perfectly straight spans.
        QVector<qreal> us;
        us.reserve(16);
        us.append(0.0);
        const auto curveAt = [&](qreal u) {
            return (smooth && n >= 3) ? centripetalPoint(p0, p1, p2, p3, u) : p1 + (p2 - p1) * u;
        };
        std::function<void(qreal, qreal, int)> refine = [&](qreal ua, qreal ub, int depth) {
            if (depth >= 7) {
                return;
            }
            const qreal um = (ua + ub) * 0.5;
            const QPointF a = curveAt(ua);
            const QPointF b = curveAt(ub);
            const QPointF m = curveAt(um);
            const QPointF chordMid = (a + b) * 0.5;
            const bool posDrifts = std::hypot(m.x() - chordMid.x(), m.y() - chordMid.y()) > flatnessPx * 0.5;
            const bool widthDrifts = qAbs(widthAt(um) - (widthAt(ua) + widthAt(ub)) * 0.5) > 0.25;
            if (!posDrifts && !widthDrifts) {
                return;
            }
            refine(ua, um, depth + 1);
            us.append(um);
            refine(um, ub, depth + 1);
        };
        refine(0.0, 1.0, 0);

        for (const qreal u : us) {
            pushSample(u);
        }
    }

    if (!closed) {
        const qreal endTaper = KisAiStrokeQualityUtils::calculateTaper(1.0, brush.profile, false);
        samples.append({scaledPts.last(),
                        KisAiStrokeQualityUtils::effectiveWidthPx(brush,
                                                                  safePressure(n - 1) * endTaper,
                                                                  workingSize,
                                                                  supersampleScale)});
    }

    return samples;
}

void KisAiStrokeCoverageRaster::addStrokeCoverageShapes(QPainter &target,
                                                        const QVector<StrokeSample> &samples,
                                                        bool closed,
                                                        bool cornerFillets)
{
    const int n = samples.size();
    if (n < 2) {
        if (n == 1) {
            target.drawEllipse(samples.first().pos, qMax<qreal>(0.3, samples.first().width * 0.5),
                               qMax<qreal>(0.3, samples.first().width * 0.5));
        }
        return;
    }

    QVector<QPointF> tangents;
    QVector<QPointF> normals;
    computeFrames(samples, closed, tangents, normals);

    // Segment quads keep the sides straight between samples.
    const int segCount = closed ? n : (n - 1);
    for (int i = 0; i < segCount; ++i) {
        const int j = (i + 1) % n;
        const qreal hw0 = qMax<qreal>(0.35, samples.at(i).width * 0.5);
        const qreal hw1 = qMax<qreal>(0.35, samples.at(j).width * 0.5);
        QPolygonF quad;
        quad.reserve(4);
        quad << samples.at(i).pos + normals.at(i) * hw0 << samples.at(j).pos + normals.at(j) * hw1
             << samples.at(j).pos - normals.at(j) * hw1 << samples.at(i).pos - normals.at(i) * hw0;
        target.drawPolygon(quad);
    }

    // Round joins + caps: one sweep disk per sample. Union is idempotent under
    // Lighten, so AA fringes and shared quad edges never accumulate.
    for (const StrokeSample &s : samples) {
        const qreal r = qMax<qreal>(0.3, s.width * 0.5);
        target.drawEllipse(s.pos, r, r);
    }

    if (cornerFillets && n >= 3) {
        const int stepInterval = qMax(1, n / 32);
        for (int i = stepInterval; i < n - stepInterval; i += stepInterval) {
            const QPolygonF fillet = KisAiStrokeQualityUtils::generateCornerInkingPolygon(
                samples.at(i - stepInterval).pos,
                samples.at(i).pos,
                samples.at(i + stepInterval).pos,
                samples.at(i).width);
            if (fillet.size() >= 3) {
                target.drawPolygon(fillet);
            }
        }
    }
}

void KisAiStrokeCoverageRaster::paintStroke(QPainter &painter,
                                            const QVector<StrokeSample> &samples,
                                            bool closed,
                                            const KisAiStrokeBrush &brush,
                                            const StrokeTexture &texture,
                                            const QColor &color,
                                            const QSize &workingSize,
                                            int supersampleScale)
{
    const int n = samples.size();
    if (n == 0 || color.alpha() <= 0 || workingSize.width() <= 0 || workingSize.height() <= 0) {
        return;
    }

    qreal maxW = 0.0;
    qreal sumW = 0.0;
    for (const StrokeSample &s : samples) {
        maxW = qMax(maxW, s.width);
        sumW += s.width;
    }
    const qreal avgW = sumW / n;

    qreal radiusFactor = 0.75;
    switch (texture.style) {
    case TextureStyle::Airbrush:
        radiusFactor = 1.9;
        break;
    case TextureStyle::Neon:
        radiusFactor = 1.7;
        break;
    case TextureStyle::Splatter:
        radiusFactor = 3.3;
        break;
    case TextureStyle::Stipple:
        radiusFactor = 1.2;
        break;
    default:
        radiusFactor = 0.9;
        break;
    }

    QRectF boundsF;
    for (const StrokeSample &s : samples) {
        boundsF = boundsF.united(QRectF(s.pos.x() - maxW * radiusFactor,
                                       s.pos.y() - maxW * radiusFactor,
                                       maxW * radiusFactor * 2.0,
                                       maxW * radiusFactor * 2.0));
    }
    if (texture.style == TextureStyle::Splatter) {
        boundsF.adjust(-maxW * 1.5, -maxW * 1.5, maxW * 1.5, maxW * 1.5);
    }
    boundsF.adjust(-2.0, -2.0, 2.0, 2.0);
    QRect bounds = boundsF.toAlignedRect();
    if (!bounds.isValid() || bounds.isEmpty()) {
        return;
    }
    bounds = bounds.intersected(QRect(-2, -2, workingSize.width() + 4, workingSize.height() + 4));
    if (bounds.isEmpty()) {
        return;
    }

    QImage mask(bounds.size(), QImage::Format_ARGB32);
    mask.fill(Qt::black);

    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.translate(-bounds.topLeft());
        mp.setCompositionMode(QPainter::CompositionMode_Lighten);

        const auto fillShapes = [&](qreal level) {
            mp.setPen(Qt::NoPen);
            mp.setBrush(grayLevel(level));
            addStrokeCoverageShapes(mp, samples, closed, texture.cornerFillets);
        };

        switch (texture.style) {
        case TextureStyle::Airbrush: {
            // Smooth radial falloff tube; overlapping gradient disks combine
            // with max, so the falloff never accumulates along the run.
            mp.setPen(Qt::NoPen);
            for (const StrokeSample &s : samples) {
                const qreal r = qMax<qreal>(0.5, s.width * 1.8);
                QRadialGradient grad(s.pos, r);
                grad.setColorAt(0.0, grayLevel(1.0));
                grad.setColorAt(0.25, grayLevel(1.0));
                grad.setColorAt(0.30, grayLevel(0.35));
                grad.setColorAt(0.55, grayLevel(0.12));
                grad.setColorAt(0.92, grayLevel(0.12));
                grad.setColorAt(1.0, grayLevel(0.0));
                mp.setBrush(grad);
                mp.drawEllipse(s.pos, r, r);
            }
            break;
        }
        case TextureStyle::Watercolor: {
            fillShapes(0.65);
            QVector<QPointF> tangents;
            QVector<QPointF> normals;
            computeFrames(samples, closed, tangents, normals);
            drawChainRim(mp, samples, normals, grayLevel(0.92), qMax<qreal>(0.8, avgW * 0.12));
            QRandomGenerator grain(texture.seed ^ 0x70617065u);
            mp.setPen(Qt::NoPen);
            for (const StrokeSample &s : samples) {
                if (grain.generateDouble() > 0.35) {
                    continue;
                }
                const qreal spread = (grain.generateDouble() - 0.5) * s.width;
                const qreal gr = qMax<qreal>(0.4, s.width * 0.08);
                mp.setBrush(grayLevel(0.685));
                mp.drawEllipse(s.pos + QPointF(spread, spread * 0.6), gr, gr);
            }
            break;
        }
        case TextureStyle::Bristle: {
            fillShapes(0.72);
            QVector<KisAiStrokePoint> spine;
            spine.reserve(n);
            for (const StrokeSample &s : samples) {
                spine.append(KisAiStrokePoint(s.pos.x(), s.pos.y(), 0.8));
            }
            const auto strands =
                KisAiStrokeQualityUtils::generateBristleStrands(spine, 5, avgW * 0.40, texture.seed ^ 0x62726973u);
            mp.setBrush(Qt::NoBrush);
            mp.setPen(QPen(grayLevel(0.82), qMax<qreal>(0.6, avgW * 0.15), Qt::SolidLine, Qt::RoundCap));
            for (const QVector<QPointF> &strand : strands) {
                if (strand.size() >= 2) {
                    mp.drawPolyline(strand);
                }
            }
            break;
        }
        case TextureStyle::Pencil: {
            fillShapes(0.78);
            QRandomGenerator grain(texture.seed ^ 0x70656e63u);
            mp.setBrush(Qt::NoBrush);
            const QPen filamentPen(grayLevel(0.84),
                                   qMax<qreal>(0.45, avgW * 0.18),
                                   Qt::SolidLine,
                                   Qt::RoundCap);
            QVector<QPointF> tangents;
            QVector<QPointF> normals;
            computeFrames(samples, closed, tangents, normals);
            for (int pass = 0; pass < 3; ++pass) {
                const qreal offset = (grain.generateDouble() - 0.5) * avgW * 0.45;
                QPolygonF filamentPath;
                filamentPath.reserve(n);
                for (int i = 0; i < n; ++i) {
                    const qreal jitter = (grain.generateDouble() - 0.5) * 0.35;
                    filamentPath.append(samples.at(i).pos + normals.at(i) * (offset + jitter));
                }
                mp.setPen(filamentPen);
                if (filamentPath.size() >= 2) {
                    mp.drawPolyline(filamentPath);
                }
            }
            break;
        }
        case TextureStyle::Charcoal: {
            fillShapes(0.60);
            QRandomGenerator rng(texture.seed ^ 0x63686172u);
            mp.setPen(Qt::NoPen);
            for (const StrokeSample &s : samples) {
                for (int k = 0; k < 4; ++k) {
                    const qreal rx = (rng.generateDouble() - 0.5) * s.width * 1.1;
                    const qreal ry = (rng.generateDouble() - 0.5) * s.width * 1.1;
                    const qreal rDot = qMax<qreal>(0.4, s.width * 0.12 * (0.4 + rng.generateDouble() * 0.6));
                    mp.setBrush(grayLevel(0.60 + rng.generateDouble() * 0.5 * 0.40));
                    mp.drawEllipse(s.pos + QPointF(rx, ry), rDot, rDot);
                }
            }
            break;
        }
        case TextureStyle::Crayon: {
            fillShapes(0.65);
            QRandomGenerator grain(texture.seed ^ 0x63726179u);
            mp.setPen(Qt::NoPen);
            for (const StrokeSample &s : samples) {
                for (int d = 0; d < 3; ++d) {
                    const qreal rx = (grain.generateDouble() - 0.5) * s.width;
                    const qreal ry = (grain.generateDouble() - 0.5) * s.width;
                    const qreal dotR = qMax<qreal>(0.5, s.width * 0.15 * (0.5 + grain.generateDouble() * 0.5));
                    mp.setBrush(grayLevel(0.65 + (0.3 + grain.generateDouble() * 0.5) * 0.35));
                    mp.drawEllipse(s.pos + QPointF(rx, ry), dotR, dotR);
                }
            }
            break;
        }
        case TextureStyle::Marker: {
            fillShapes(0.72);
            QVector<QPointF> tangents;
            QVector<QPointF> normals;
            computeFrames(samples, closed, tangents, normals);
            drawChainRim(mp, samples, normals, grayLevel(0.83), qMax<qreal>(1.0, avgW * 0.25));
            break;
        }
        case TextureStyle::Neon: {
            // Aura and halo as soft gradient disks (max-combined), then the tube.
            mp.setPen(Qt::NoPen);
            for (const StrokeSample &s : samples) {
                const qreal r = qMax<qreal>(0.6, s.width * 1.6);
                QRadialGradient grad(s.pos, r);
                grad.setColorAt(0.0, grayLevel(0.45));
                grad.setColorAt(0.53, grayLevel(0.45));
                grad.setColorAt(0.88, grayLevel(0.18));
                grad.setColorAt(1.0, grayLevel(0.0));
                mp.setBrush(grad);
                mp.drawEllipse(s.pos, r, r);
            }
            fillShapes(1.0);
            break;
        }
        case TextureStyle::Splatter: {
            fillShapes(1.0);
            QRandomGenerator rng(texture.seed ^ 0x73706c74u);
            mp.setPen(Qt::NoPen);
            const int splatterCount = qBound(6, n * 2, 80);
            for (int s = 0; s < splatterCount; ++s) {
                const StrokeSample &sample = samples.at(rng.bounded(n));
                const qreal dist = sample.width * (0.8 + rng.generateDouble() * 2.2);
                const qreal angle = rng.generateDouble() * 2.0 * PI;
                const QPointF dropPos = sample.pos + QPointF(std::cos(angle) * dist, std::sin(angle) * dist);
                const qreal dropR = qMax<qreal>(0.6, sample.width * (0.08 + rng.generateDouble() * 0.20));
                mp.setBrush(grayLevel(0.5 + rng.generateDouble() * 0.5));
                mp.drawEllipse(dropPos, dropR, dropR);
            }
            break;
        }
        case TextureStyle::Stipple: {
            QRandomGenerator rng(texture.seed ^ 0x73746970u);
            mp.setPen(Qt::NoPen);
            const int dotCount = qBound(8, n * 2, 300);
            for (int i = 0; i < dotCount; ++i) {
                const StrokeSample &s = samples.at(rng.bounded(n));
                const qreal spread = (rng.generateDouble() - 0.5) * s.width * 1.6;
                const qreal r = qMax<qreal>(0.4, s.width * 0.22 * (0.6 + rng.generateDouble() * 0.6));
                mp.setBrush(grayLevel(0.4 + rng.generateDouble() * 0.6));
                mp.drawEllipse(s.pos + QPointF(spread, spread * 0.8), r, r);
            }
            break;
        }
        case TextureStyle::Feathering: {
            QRandomGenerator rng(texture.seed ^ 0x66656174u);
            mp.setBrush(Qt::NoBrush);
            for (int s = 0; s < 3; ++s) {
                const qreal lateral = (rng.generateDouble() - 0.5) * avgW * 0.7;
                QPolygonF strandPath;
                strandPath.reserve(n);
                for (int i = 0; i < n; ++i) {
                    const qreal jitter = (rng.generateDouble() - 0.5) * 0.35;
                    strandPath.append(samples.at(i).pos + QPointF(lateral + jitter, jitter));
                }
                mp.setPen(QPen(grayLevel(0.42), qMax<qreal>(0.5, avgW * 0.45), Qt::SolidLine, Qt::RoundCap));
                if (strandPath.size() >= 2) {
                    mp.drawPolyline(strandPath);
                }
            }
            break;
        }
        case TextureStyle::Solid:
        default:
            fillShapes(1.0);
            break;
        }
    }

    QImage tile(bounds.size(), QImage::Format_ARGB32);
    const int cr = color.red();
    const int cg = color.green();
    const int cb = color.blue();
    const int ca = color.alpha();
    for (int y = 0; y < bounds.height(); ++y) {
        const QRgb *m = reinterpret_cast<const QRgb *>(mask.constScanLine(y));
        QRgb *t = reinterpret_cast<QRgb *>(tile.scanLine(y));
        for (int x = 0; x < bounds.width(); ++x) {
            const int cov = qRed(m[x]);
            if (cov <= 0 || ca <= 0) {
                t[x] = 0;
                continue;
            }
            t[x] = qRgba(cr, cg, cb, (ca * cov + 127) / 255);
        }
    }
    tile = tile.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // CompositionMode_Clear zeroes whole spans regardless of source alpha, so
    // a tile blit would erase the entire bbox. DestinationOut keeps the erase
    // shaped by the stroke coverage.
    const QPainter::CompositionMode mode = painter.compositionMode();
    if (mode == QPainter::CompositionMode_Clear) {
        painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
        painter.drawImage(bounds.topLeft(), tile);
        painter.setCompositionMode(mode);
    } else {
        painter.drawImage(bounds.topLeft(), tile);
    }

    if (texture.style == TextureStyle::Neon) {
        // White-hot filament is a second color, so it gets its own single composite.
        QVector<StrokeSample> coreSamples = samples;
        for (StrokeSample &s : coreSamples) {
            s.width = qMax<qreal>(1.0, s.width * 0.28);
        }
        StrokeTexture coreTexture;
        coreTexture.style = TextureStyle::Solid;
        coreTexture.seed = texture.seed;
        QColor coreColor(255, 255, 255);
        coreColor.setAlpha(qBound(0, qRound(color.alphaF() * 0.90 * 255.0), 255));
        paintStroke(painter, coreSamples, closed, brush, coreTexture, coreColor, workingSize, supersampleScale);
    }
}
