/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiStrokeTypeChecker.h"
#include "KisAiStrokeProgram.h"

#include <QColor>
#include <QRegularExpression>
#include <cmath>

namespace
{
// 座標系フィールドへの安全クランプ。
// parseProgramJson() は「maxCoord > 1.5 ならピクセル座標」と判定してから
// キャンバス寸法で正規化する。この段階で [-0.5, 1.5] へ潰してしまうと判定が
// 永遠に発動せず、ピクセル座標のストロークが (1.5, 1.5) → 角に固まって全消滅する。
// 1e300 等の巨大値だけを弾き、最終的な [0,1] クランプは refineForRendering() に委ねる。
constexpr qreal kMaxSafeCoordinate = 1.0e7;
} // namespace

QString KisAiStrokeTypeCheckReport::summary() const
{
    return QStringLiteral("TypeCheck: totalOps=%1, coerced=%2, errors=%3, valid=%4")
        .arg(totalCheckedOperations)
        .arg(coercedValues)
        .arg(typeErrors)
        .arg(isValid ? QStringLiteral("true") : QStringLiteral("false"));
}

bool KisAiStrokeTypeChecker::coerceToNumber(const QJsonValue &val, qreal *outVal)
{
    if (!outVal)
        return false;

    if (val.isDouble()) {
        *outVal = val.toDouble();
        return std::isfinite(*outVal);
    }

    if (val.isString()) {
        QString s = val.toString().trimmed();
        if (s.compare(QLatin1String("nan"), Qt::CaseInsensitive) == 0) {
            *outVal = 0.0;
            return true;
        }
        if (s.compare(QLatin1String("infinity"), Qt::CaseInsensitive) == 0
            || s.compare(QLatin1String("+infinity"), Qt::CaseInsensitive) == 0) {
            *outVal = 1.0;
            return true;
        }
        if (s.compare(QLatin1String("-infinity"), Qt::CaseInsensitive) == 0) {
            *outVal = -1.0;
            return true;
        }
        if (s.startsWith(QLatin1Char('.'))) {
            s.prepend(QLatin1Char('0'));
        }
        if (s.endsWith(QLatin1Char('.'))) {
            s.append(QLatin1Char('0'));
        }
        bool ok = false;
        const double d = s.toDouble(&ok);
        if (ok && std::isfinite(d)) {
            *outVal = d;
            return true;
        }
    }

    if (val.isBool()) {
        *outVal = val.toBool() ? 1.0 : 0.0;
        return true;
    }

    return false;
}

bool KisAiStrokeTypeChecker::coerceToBool(const QJsonValue &val, bool *outVal)
{
    if (!outVal)
        return false;

    if (val.isBool()) {
        *outVal = val.toBool();
        return true;
    }

    if (val.isDouble()) {
        *outVal = (val.toDouble() != 0.0);
        return true;
    }

    if (val.isString()) {
        const QString s = val.toString().trimmed().toLower();
        if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes") || s == QLatin1String("on")) {
            *outVal = true;
            return true;
        }
        if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no") || s == QLatin1String("off")) {
            *outVal = false;
            return true;
        }
    }

    return false;
}

bool KisAiStrokeTypeChecker::coerceToString(const QJsonValue &val, QString *outVal)
{
    if (!outVal)
        return false;

    if (val.isString()) {
        *outVal = val.toString();
        return true;
    }
    if (val.isDouble()) {
        *outVal = QString::number(val.toDouble());
        return true;
    }
    if (val.isBool()) {
        *outVal = val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
        return true;
    }
    return false;
}

bool KisAiStrokeTypeChecker::isValidColorString(const QString &str)
{
    QString trimmed = str.trimmed();
    if (trimmed.isEmpty())
        return false;

    if (trimmed.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
        static const QRegularExpression rgbRegex(
            QStringLiteral(R"(rgba?\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)(?:\s*,\s*([\d.]+))?\s*\))"),
            QRegularExpression::CaseInsensitiveOption);
        if (rgbRegex.match(trimmed).hasMatch()) {
            return true;
        }
    }

    if (!trimmed.startsWith(QLatin1Char('#'))) {
        const int len = trimmed.length();
        if (len == 3 || len == 4 || len == 6 || len == 8) {
            static const QRegularExpression hexNoPrefix(QStringLiteral("^[0-9a-fA-F]+$"));
            if (hexNoPrefix.match(trimmed).hasMatch()) {
                trimmed = QLatin1Char('#') + trimmed;
            }
        }
    }

    if (trimmed.startsWith(QLatin1Char('#'))) {
        const int len = trimmed.length();
        if (len == 4 || len == 5 || len == 7 || len == 9) {
            static const QRegularExpression hexRe(QStringLiteral("^#[0-9a-fA-F]+$"));
            return hexRe.match(trimmed).hasMatch();
        }
    }

    // QColor::fromString() is Qt 6.4+; the QColor(QString) constructor has the
    // same semantics and exists on every supported Qt version.
    return QColor::isValidColorName(trimmed) || QColor(trimmed).isValid();
}

bool KisAiStrokeTypeChecker::checkBrushObject(QJsonObject *brushObj, QString *outError, int *coercedCount)
{
    Q_UNUSED(outError);
    if (!brushObj)
        return false;

    // profile: string
    if (brushObj->contains(QStringLiteral("profile"))) {
        const QJsonValue pVal = brushObj->value(QStringLiteral("profile"));
        if (!pVal.isString()) {
            QString coerced;
            if (coerceToString(pVal, &coerced)) {
                (*brushObj)[QStringLiteral("profile")] = coerced.toLower();
                if (coercedCount) ++(*coercedCount);
            } else {
                (*brushObj)[QStringLiteral("profile")] = QStringLiteral("auto");
                if (coercedCount) ++(*coercedCount);
            }
        }
    } else {
        (*brushObj)[QStringLiteral("profile")] = QStringLiteral("auto");
    }

    // color: valid color string
    if (brushObj->contains(QStringLiteral("color"))) {
        const QJsonValue cVal = brushObj->value(QStringLiteral("color"));
        QString cStr;
        if (coerceToString(cVal, &cStr) && isValidColorString(cStr)) {
            QString normalized = cStr.trimmed();
            if (normalized.startsWith(QLatin1String("rgb"), Qt::CaseInsensitive)) {
                const QColor parsed = KisAiStrokeProgramCodec::parseColor(normalized);
                normalized = parsed.name();
            } else if (!normalized.startsWith(QLatin1Char('#')) && (normalized.length() == 3 || normalized.length() == 4 || normalized.length() == 6 || normalized.length() == 8)) {
                normalized = QLatin1Char('#') + normalized;
            }
            if (!cVal.isString() || normalized != cVal.toString()) {
                (*brushObj)[QStringLiteral("color")] = normalized;
                if (coercedCount) ++(*coercedCount);
            }
        } else {
            // Check if color was passed as array [r, g, b]
            if (cVal.isArray()) {
                const QJsonArray arr = cVal.toArray();
                if (arr.size() >= 3) {
                    qreal r = 0, g = 0, b = 0;
                    coerceToNumber(arr.at(0), &r);
                    coerceToNumber(arr.at(1), &g);
                    coerceToNumber(arr.at(2), &b);
                    const int ir = (r <= 1.0 && g <= 1.0 && b <= 1.0) ? qRound(r * 255.0) : qRound(r);
                    const int ig = (r <= 1.0 && g <= 1.0 && b <= 1.0) ? qRound(g * 255.0) : qRound(g);
                    const int ib = (r <= 1.0 && g <= 1.0 && b <= 1.0) ? qRound(b * 255.0) : qRound(b);
                    (*brushObj)[QStringLiteral("color")] = QColor(qBound(0, ir, 255), qBound(0, ig, 255), qBound(0, ib, 255)).name();
                    if (coercedCount) ++(*coercedCount);
                } else {
                    (*brushObj)[QStringLiteral("color")] = QStringLiteral("#232323");
                    if (coercedCount) ++(*coercedCount);
                }
            } else {
                (*brushObj)[QStringLiteral("color")] = QStringLiteral("#232323");
                if (coercedCount) ++(*coercedCount);
            }
        }
    } else {
        (*brushObj)[QStringLiteral("color")] = QStringLiteral("#232323");
    }

    // size: positive number in (0.0, 1.0]
    if (brushObj->contains(QStringLiteral("size"))) {
        const QJsonValue sVal = brushObj->value(QStringLiteral("size"));
        qreal sz = 0.008;
        if (!sVal.isDouble() || sVal.toDouble() <= 0.0 || sVal.toDouble() > 1.0) {
            if (coerceToNumber(sVal, &sz) && sz > 0.0) {
                // opacity と同様に上限を clamp: 5.0 のような巨大値が残ると
                // レンダラーが画面全体を覆うストロークを描く。
                (*brushObj)[QStringLiteral("size")] = qBound(0.0005, sz, 1.0);
                if (coercedCount) ++(*coercedCount);
            } else {
                (*brushObj)[QStringLiteral("size")] = 0.008;
                if (coercedCount) ++(*coercedCount);
            }
        }
    } else {
        (*brushObj)[QStringLiteral("size")] = 0.008;
    }

    // opacity: number in [0.0, 1.0]
    if (brushObj->contains(QStringLiteral("opacity"))) {
        const QJsonValue oVal = brushObj->value(QStringLiteral("opacity"));
        qreal op = 1.0;
        if (!oVal.isDouble()) {
            if (coerceToNumber(oVal, &op)) {
                (*brushObj)[QStringLiteral("opacity")] = qBound(0.0, op, 1.0);
                if (coercedCount) ++(*coercedCount);
            } else {
                (*brushObj)[QStringLiteral("opacity")] = 1.0;
                if (coercedCount) ++(*coercedCount);
            }
        } else {
            const qreal valD = oVal.toDouble();
            if (valD < 0.0 || valD > 1.0) {
                (*brushObj)[QStringLiteral("opacity")] = qBound(0.0, valD, 1.0);
                if (coercedCount) ++(*coercedCount);
            }
        }
    }

    // is_eraser: bool
    if (brushObj->contains(QStringLiteral("is_eraser"))) {
        const QJsonValue eVal = brushObj->value(QStringLiteral("is_eraser"));
        if (!eVal.isBool()) {
            bool b = false;
            if (coerceToBool(eVal, &b)) {
                (*brushObj)[QStringLiteral("is_eraser")] = b;
                if (coercedCount) ++(*coercedCount);
            } else {
                (*brushObj)[QStringLiteral("is_eraser")] = false;
                if (coercedCount) ++(*coercedCount);
            }
        }
    }

    return true;
}

bool KisAiStrokeTypeChecker::checkPointsArray(QJsonArray *pointsArray, QString *outError, int *coercedCount)
{
    Q_UNUSED(outError);
    if (!pointsArray)
        return false;

    QJsonArray normalized;

    for (int i = 0; i < pointsArray->size(); ++i) {
        const QJsonValue ptVal = pointsArray->at(i);

        // Format 1: [x, y] or [x, y, pressure] or [x, y, pressure, timeMs]
        if (ptVal.isArray()) {
            const QJsonArray arr = ptVal.toArray();
            if (arr.size() >= 2) {
                qreal x = 0.0, y = 0.0, p = 0.8;
                bool okX = coerceToNumber(arr.at(0), &x);
                bool okY = coerceToNumber(arr.at(1), &y);
                bool okP = (arr.size() >= 3) ? coerceToNumber(arr.at(2), &p) : true;

                if (okX && okY) {
                    // 座標は安全クランプのみ (正規化/ピクセル判定は parseProgramJson 側)。
                    QJsonArray normPt;
                    normPt.append(qBound(-kMaxSafeCoordinate, x, kMaxSafeCoordinate));
                    normPt.append(qBound(-kMaxSafeCoordinate, y, kMaxSafeCoordinate));
                    normPt.append(qBound(0.0, okP ? p : 0.8, 1.0));
                    normalized.append(normPt);

                    if (!arr.at(0).isDouble() || !arr.at(1).isDouble() || (arr.size() >= 3 && !arr.at(2).isDouble())) {
                        if (coercedCount) ++(*coercedCount);
                    }
                } else {
                    if (coercedCount) ++(*coercedCount);
                }
            }
            continue;
        }

        // Format 2: {"x": 0.1, "y": 0.2, "pressure": 0.8}
        if (ptVal.isObject()) {
            const QJsonObject ptObj = ptVal.toObject();
            const QJsonValue vx = ptObj.contains(QStringLiteral("x")) ? ptObj.value(QStringLiteral("x")) : ptObj.value(QStringLiteral("pos_x"));
            const QJsonValue vy = ptObj.contains(QStringLiteral("y")) ? ptObj.value(QStringLiteral("y")) : ptObj.value(QStringLiteral("pos_y"));
            const QJsonValue vp = ptObj.value(QStringLiteral("pressure"));

            qreal x = 0.0, y = 0.0, p = 0.8;
            if (coerceToNumber(vx, &x) && coerceToNumber(vy, &y)) {
                // Always route pressure through coerceToNumber: it is the only
                // path that rejects NaN/Inf, and a raw vp.isDouble() check would
                // let a non-finite JSON number through to the width math.
                if (!coerceToNumber(vp, &p)) {
                    p = 0.8;
                }
                QJsonArray normPt;
                normPt.append(qBound(-kMaxSafeCoordinate, x, kMaxSafeCoordinate));
                normPt.append(qBound(-kMaxSafeCoordinate, y, kMaxSafeCoordinate));
                normPt.append(qBound(0.0, p, 1.0));
                normalized.append(normPt);
                if (coercedCount) ++(*coercedCount);
            }
            continue;
        }

        if (coercedCount) ++(*coercedCount);
    }

    *pointsArray = normalized;
    return !pointsArray->isEmpty();
}

bool KisAiStrokeTypeChecker::checkPolygonArray(QJsonArray *polygonArray, QString *outError, int *coercedCount)
{
    Q_UNUSED(outError);
    if (!polygonArray)
        return false;

    QJsonArray normalized;

    for (int i = 0; i < polygonArray->size(); ++i) {
        const QJsonValue ptVal = polygonArray->at(i);

        if (ptVal.isArray()) {
            const QJsonArray arr = ptVal.toArray();
            if (arr.size() >= 2) {
                qreal x = 0.0, y = 0.0;
                if (coerceToNumber(arr.at(0), &x) && coerceToNumber(arr.at(1), &y)) {
                    QJsonArray normPt;
                    normPt.append(qBound(-kMaxSafeCoordinate, x, kMaxSafeCoordinate));
                    normPt.append(qBound(-kMaxSafeCoordinate, y, kMaxSafeCoordinate));
                    normalized.append(normPt);

                    if (!arr.at(0).isDouble() || !arr.at(1).isDouble()) {
                        if (coercedCount) ++(*coercedCount);
                    }
                }
            }
            continue;
        }

        if (ptVal.isObject()) {
            const QJsonObject ptObj = ptVal.toObject();
            const QJsonValue vx = ptObj.contains(QStringLiteral("x")) ? ptObj.value(QStringLiteral("x")) : ptObj.value(QStringLiteral("pos_x"));
            const QJsonValue vy = ptObj.contains(QStringLiteral("y")) ? ptObj.value(QStringLiteral("y")) : ptObj.value(QStringLiteral("pos_y"));
            qreal x = 0.0, y = 0.0;
            if (coerceToNumber(vx, &x) && coerceToNumber(vy, &y)) {
                QJsonArray normPt;
                normPt.append(qBound(-kMaxSafeCoordinate, x, kMaxSafeCoordinate));
                normPt.append(qBound(-kMaxSafeCoordinate, y, kMaxSafeCoordinate));
                normalized.append(normPt);
                if (coercedCount) ++(*coercedCount);
            }
            continue;
        }

        if (coercedCount) ++(*coercedCount);
    }

    *polygonArray = normalized;
    return !polygonArray->isEmpty();
}

bool KisAiStrokeTypeChecker::checkAndCoerceOperation(
    QJsonObject *opObj,
    int opIndex,
    KisAiStrokeTypeCheckReport *report)
{
    if (!opObj)
        return false;

    // 1. Kind normalization
    QString kindStr;
    if (opObj->contains(QStringLiteral("kind"))) {
        kindStr = opObj->value(QStringLiteral("kind")).toString().trimmed().toLower();
    } else if (opObj->contains(QStringLiteral("type"))) {
        kindStr = opObj->value(QStringLiteral("type")).toString().trimmed().toLower();
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    }

    // Map common synonyms
    if (kindStr == QLatin1String("stroke") || kindStr == QLatin1String("line") || kindStr == QLatin1String("curve") || kindStr == QLatin1String("contour")) {
        kindStr = QStringLiteral("path");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("gradient") || kindStr == QLatin1String("gradientfill")) {
        kindStr = QStringLiteral("gradient_fill");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("mangalines") || kindStr == QLatin1String("speed_lines") || kindStr == QLatin1String("focus_lines") || kindStr == QLatin1String("speed") || kindStr == QLatin1String("focus")) {
        kindStr = QStringLiteral("manga_lines");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("particle") || kindStr == QLatin1String("scatter") || kindStr == QLatin1String("sparkle")) {
        kindStr = QStringLiteral("particles");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("band") || kindStr == QLatin1String("taper")) {
        kindStr = QStringLiteral("ribbon");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("color_fill") || kindStr == QLatin1String("solid_fill") || kindStr == QLatin1String("polygon")) {
        kindStr = QStringLiteral("fill");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("anime_eye") || kindStr == QLatin1String("anime_eyes") || kindStr == QLatin1String("eye") || kindStr == QLatin1String("eye_feature") || kindStr == QLatin1String("eyes")) {
        kindStr = QStringLiteral("anime_eye");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("cloth_drapery") || kindStr == QLatin1String("drapery") || kindStr == QLatin1String("drapery_fold")) {
        kindStr = QStringLiteral("cloth_drapery");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("hair_flow_cluster") || kindStr == QLatin1String("hair_flow") || kindStr == QLatin1String("hair_cluster")) {
        kindStr = QStringLiteral("hair_flow_cluster");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("hand_gesture") || kindStr == QLatin1String("hand")) {
        kindStr = QStringLiteral("hand_gesture");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    } else if (kindStr == QLatin1String("dynamic_pose") || kindStr == QLatin1String("flow_line")) {
        kindStr = QStringLiteral("dynamic_pose");
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    }

    // If kind still empty, infer from geometry
    if (kindStr.isEmpty()) {
        if (opObj->contains(QStringLiteral("points")) || opObj->contains(QStringLiteral("pts"))) {
            kindStr = QStringLiteral("path");
        } else if (opObj->contains(QStringLiteral("spine"))) {
            kindStr = QStringLiteral("ribbon");
        } else if (opObj->contains(QStringLiteral("polygon")) || opObj->contains(QStringLiteral("poly"))) {
            kindStr = opObj->contains(QStringLiteral("colors")) ? QStringLiteral("gradient_fill") : QStringLiteral("fill");
        } else if (opObj->contains(QStringLiteral("bounds")) || opObj->contains(QStringLiteral("rect")) || opObj->contains(QStringLiteral("box")) || opObj->contains(QStringLiteral("particle_shape")) || opObj->contains(QStringLiteral("particle_count"))) {
            kindStr = QStringLiteral("particles");
        } else if (opObj->contains(QStringLiteral("inner_radius")) || opObj->contains(QStringLiteral("outer_radius")) || opObj->contains(QStringLiteral("density"))) {
            kindStr = QStringLiteral("manga_lines");
        } else if (opObj->contains(QStringLiteral("iris_color")) || opObj->contains(QStringLiteral("eye_center")) || opObj->contains(QStringLiteral("eye_style"))) {
            kindStr = QStringLiteral("anime_eye");
        } else {
            kindStr = QStringLiteral("path");
        }
        (*opObj)[QStringLiteral("kind")] = kindStr;
        if (report) ++report->coercedValues;
    }

    // 2. ID validation
    if (!opObj->contains(QStringLiteral("id")) && opObj->contains(QStringLiteral("name"))) {
        (*opObj)[QStringLiteral("id")] = opObj->value(QStringLiteral("name"));
        if (report) ++report->coercedValues;
    }
    if (!opObj->contains(QStringLiteral("id")) || !opObj->value(QStringLiteral("id")).isString()
        || opObj->value(QStringLiteral("id")).toString().trimmed().isEmpty()) {
        (*opObj)[QStringLiteral("id")] = QStringLiteral("op_%1_%2").arg(opIndex).arg(kindStr);
        if (report) ++report->coercedValues;
    }

    // 3. Layer normalization
    if (!opObj->contains(QStringLiteral("layer")) && opObj->contains(QStringLiteral("layer_name"))) {
        (*opObj)[QStringLiteral("layer")] = opObj->value(QStringLiteral("layer_name"));
        if (report) ++report->coercedValues;
    }
    if (opObj->contains(QStringLiteral("layer"))) {
        const QJsonValue lVal = opObj->value(QStringLiteral("layer"));
        if (!lVal.isString()) {
            QString lStr;
            coerceToString(lVal, &lStr);
            (*opObj)[QStringLiteral("layer")] = lStr.isEmpty() ? QStringLiteral("Lineart") : lStr;
            if (report) ++report->coercedValues;
        }
    } else {
        (*opObj)[QStringLiteral("layer")] = QStringLiteral("Lineart");
        if (report) ++report->coercedValues;
    }

    // 4. Brush object validation
    int brushCoerced = 0;
    if (opObj->contains(QStringLiteral("brush")) && opObj->value(QStringLiteral("brush")).isObject()) {
        QJsonObject bObj = opObj->value(QStringLiteral("brush")).toObject();
        checkBrushObject(&bObj, nullptr, &brushCoerced);
        (*opObj)[QStringLiteral("brush")] = bObj;
    } else {
        QJsonObject bObj;
        checkBrushObject(&bObj, nullptr, &brushCoerced);
        (*opObj)[QStringLiteral("brush")] = bObj;
    }
    if (report) report->coercedValues += brushCoerced;

    // 5. Geometry validation per kind
    if (kindStr == QLatin1String("path")) {
        if (!opObj->contains(QStringLiteral("points")) && opObj->contains(QStringLiteral("pts"))) {
            (*opObj)[QStringLiteral("points")] = opObj->value(QStringLiteral("pts"));
            if (report) ++report->coercedValues;
        }
        int ptCoerced = 0;
        QJsonArray pts = opObj->value(QStringLiteral("points")).toArray();
        if (!checkPointsArray(&pts, nullptr, &ptCoerced) || pts.isEmpty()) {
            if (report) {
                ++report->typeErrors;
                report->errorMessages.append(QStringLiteral("Operation %1 (path): points array is empty or invalid").arg(opIndex));
            }
            return false;
        }
        (*opObj)[QStringLiteral("points")] = pts;
        if (report) report->coercedValues += ptCoerced;
    } else if (kindStr == QLatin1String("gradient_fill")) {
        // Gradient fill may have points (start/end direction points) or polygon
        if (!opObj->contains(QStringLiteral("points")) && opObj->contains(QStringLiteral("pts"))) {
            (*opObj)[QStringLiteral("points")] = opObj->value(QStringLiteral("pts"));
            if (report) ++report->coercedValues;
        }
        if (!opObj->contains(QStringLiteral("polygon")) && opObj->contains(QStringLiteral("poly"))) {
            (*opObj)[QStringLiteral("polygon")] = opObj->value(QStringLiteral("poly"));
            if (report) ++report->coercedValues;
        }
        if (!opObj->contains(QStringLiteral("center")) && opObj->contains(QStringLiteral("center_pt"))) {
            (*opObj)[QStringLiteral("center")] = opObj->value(QStringLiteral("center_pt"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("points"))) {
            int ptCoerced = 0;
            QJsonArray pts = opObj->value(QStringLiteral("points")).toArray();
            if (checkPointsArray(&pts, nullptr, &ptCoerced)) {
                (*opObj)[QStringLiteral("points")] = pts;
                if (report) report->coercedValues += ptCoerced;
            }
        }
        if (opObj->contains(QStringLiteral("polygon"))) {
            int polyCoerced = 0;
            QJsonArray poly = opObj->value(QStringLiteral("polygon")).toArray();
            if (checkPolygonArray(&poly, nullptr, &polyCoerced) && poly.size() >= 3) {
                (*opObj)[QStringLiteral("polygon")] = poly;
                if (report) report->coercedValues += polyCoerced;
            }
        }
    } else if (kindStr == QLatin1String("fill") || kindStr == QLatin1String("hatch")) {
        if (!opObj->contains(QStringLiteral("polygon"))) {
            if (opObj->contains(QStringLiteral("poly"))) {
                (*opObj)[QStringLiteral("polygon")] = opObj->value(QStringLiteral("poly"));
                if (report) ++report->coercedValues;
            } else if (opObj->contains(QStringLiteral("points"))) {
                (*opObj)[QStringLiteral("polygon")] = opObj->value(QStringLiteral("points"));
                if (report) ++report->coercedValues;
            } else if (opObj->contains(QStringLiteral("pts"))) {
                (*opObj)[QStringLiteral("polygon")] = opObj->value(QStringLiteral("pts"));
                if (report) ++report->coercedValues;
            }
        }
        int polyCoerced = 0;
        QJsonArray poly = opObj->value(QStringLiteral("polygon")).toArray();
        if (!checkPolygonArray(&poly, nullptr, &polyCoerced) || poly.size() < 3) {
            if (report) {
                ++report->typeErrors;
                report->errorMessages.append(QStringLiteral("Operation %1 (%2): polygon requires at least 3 valid points").arg(opIndex).arg(kindStr));
            }
            return false;
        }
        (*opObj)[QStringLiteral("polygon")] = poly;
        if (report) report->coercedValues += polyCoerced;
    } else if (kindStr == QLatin1String("ribbon")) {
        if (!opObj->contains(QStringLiteral("spine"))) {
            if (opObj->contains(QStringLiteral("points"))) {
                (*opObj)[QStringLiteral("spine")] = opObj->value(QStringLiteral("points"));
                if (report) ++report->coercedValues;
            } else if (opObj->contains(QStringLiteral("pts"))) {
                (*opObj)[QStringLiteral("spine")] = opObj->value(QStringLiteral("pts"));
                if (report) ++report->coercedValues;
            }
        }
        int spineCoerced = 0;
        QJsonArray spine = opObj->value(QStringLiteral("spine")).toArray();
        if (!checkPolygonArray(&spine, nullptr, &spineCoerced) || spine.size() < 2) {
            if (report) {
                ++report->typeErrors;
                report->errorMessages.append(QStringLiteral("Operation %1 (ribbon): spine requires at least 2 points").arg(opIndex));
            }
            return false;
        }
        (*opObj)[QStringLiteral("spine")] = spine;
        if (report) report->coercedValues += spineCoerced;
    } else if (kindStr == QLatin1String("particles")) {
        if (!opObj->contains(QStringLiteral("shape")) && opObj->contains(QStringLiteral("particle_shape"))) {
            (*opObj)[QStringLiteral("shape")] = opObj->value(QStringLiteral("particle_shape"));
            if (report) ++report->coercedValues;
        }
        if (!opObj->contains(QStringLiteral("count")) && opObj->contains(QStringLiteral("particle_count"))) {
            (*opObj)[QStringLiteral("count")] = opObj->value(QStringLiteral("particle_count"));
            if (report) ++report->coercedValues;
        }
        if (!opObj->contains(QStringLiteral("bounds"))) {
            if (opObj->contains(QStringLiteral("rect"))) {
                (*opObj)[QStringLiteral("bounds")] = opObj->value(QStringLiteral("rect"));
                if (report) ++report->coercedValues;
            } else if (opObj->contains(QStringLiteral("box"))) {
                (*opObj)[QStringLiteral("bounds")] = opObj->value(QStringLiteral("box"));
                if (report) ++report->coercedValues;
            }
        }
        if (opObj->contains(QStringLiteral("bounds"))) {
            const QJsonValue bVal = opObj->value(QStringLiteral("bounds"));
            if (bVal.isArray()) {
                QJsonArray bArr = bVal.toArray();
                bool validBounds = (bArr.size() >= 4);
                for (int i = 0; i < qMin(4, bArr.size()); ++i) {
                    qreal n = 0.0;
                    if (!coerceToNumber(bArr.at(i), &n)) {
                        validBounds = false;
                        break;
                    }
                    // Checker output feeds the renderer directly on the
                    // checker-only path, so clamp to a huge-but-finite value
                    // instead of letting 1e300-scale values through. The
                    // pixel/normalized decision stays with parseProgramJson().
                    bArr[i] = qBound(-kMaxSafeCoordinate, n, kMaxSafeCoordinate);
                }
                if (validBounds) {
                    (*opObj)[QStringLiteral("bounds")] = bArr;
                } else {
                    (*opObj)[QStringLiteral("bounds")] = QJsonArray({0.1, 0.1, 0.9, 0.9});
                    if (report) ++report->coercedValues;
                }
            } else {
                (*opObj)[QStringLiteral("bounds")] = QJsonArray({0.1, 0.1, 0.9, 0.9});
                if (report) ++report->coercedValues;
            }
        }
        if (opObj->contains(QStringLiteral("count"))) {
            qreal cnt = 16;
            if (coerceToNumber(opObj->value(QStringLiteral("count")), &cnt)) {
                // qRound on a value beyond int range is UB (typically saturates
                // to INT_MIN, which qBound would then mis-clamp); clamp the
                // double first.
                (*opObj)[QStringLiteral("count")] = qBound(1, qRound(qBound(0.0, cnt, 1.0e9)), 256);
            } else {
                (*opObj)[QStringLiteral("count")] = 16;
                if (report) ++report->coercedValues;
            }
        }
    } else if (kindStr == QLatin1String("manga_lines")) {
        if (!opObj->contains(QStringLiteral("center")) && opObj->contains(QStringLiteral("center_pt"))) {
            (*opObj)[QStringLiteral("center")] = opObj->value(QStringLiteral("center_pt"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("center"))) {
            const QJsonValue cVal = opObj->value(QStringLiteral("center"));
            if (cVal.isArray()) {
                QJsonArray cArr = cVal.toArray();
                if (cArr.size() >= 2) {
                    qreal centerX = 0.5;
                    qreal centerY = 0.5;
                    coerceToNumber(cArr.at(0), &centerX);
                    coerceToNumber(cArr.at(1), &centerY);
                    // Same normalized-domain allowance as the anime-eye center.
                    (*opObj)[QStringLiteral("center")] =
                        QJsonArray({qBound(-kMaxSafeCoordinate, centerX, kMaxSafeCoordinate),
                                    qBound(-kMaxSafeCoordinate, centerY, kMaxSafeCoordinate)});
                }
            }
        }
        if (!opObj->contains(QStringLiteral("inner_radius")) && opObj->contains(QStringLiteral("inner"))) {
            (*opObj)[QStringLiteral("inner_radius")] = opObj->value(QStringLiteral("inner"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("inner_radius"))) {
            qreal inR = 0.15;
            if (coerceToNumber(opObj->value(QStringLiteral("inner_radius")), &inR)) {
                (*opObj)[QStringLiteral("inner_radius")] = qBound(0.0, inR, kMaxSafeCoordinate);
            } else {
                (*opObj)[QStringLiteral("inner_radius")] = 0.15;
                if (report) ++report->coercedValues;
            }
        }
        if (!opObj->contains(QStringLiteral("outer_radius")) && opObj->contains(QStringLiteral("outer"))) {
            (*opObj)[QStringLiteral("outer_radius")] = opObj->value(QStringLiteral("outer"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("outer_radius"))) {
            qreal outR = 0.70;
            if (coerceToNumber(opObj->value(QStringLiteral("outer_radius")), &outR)) {
                (*opObj)[QStringLiteral("outer_radius")] = qBound(0.01, outR, kMaxSafeCoordinate);
            } else {
                (*opObj)[QStringLiteral("outer_radius")] = 0.70;
                if (report) ++report->coercedValues;
            }
        }
        if (!opObj->contains(QStringLiteral("line_length_jitter")) && opObj->contains(QStringLiteral("jitter"))) {
            (*opObj)[QStringLiteral("line_length_jitter")] = opObj->value(QStringLiteral("jitter"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("line_length_jitter"))) {
            qreal jit = 0.20;
            if (coerceToNumber(opObj->value(QStringLiteral("line_length_jitter")), &jit)) {
                (*opObj)[QStringLiteral("line_length_jitter")] = qBound(0.0, jit, 1.0);
            } else {
                (*opObj)[QStringLiteral("line_length_jitter")] = 0.20;
                if (report) ++report->coercedValues;
            }
        }
        if (opObj->contains(QStringLiteral("density"))) {
            qreal dens = 48;
            if (coerceToNumber(opObj->value(QStringLiteral("density")), &dens)) {
                // Clamp the double before qRound to avoid the INT_MIN saturation.
                (*opObj)[QStringLiteral("density")] = qBound(4, qRound(qBound(0.0, dens, 1.0e9)), 120);
            }
        }
    } else if (kindStr == QLatin1String("anime_eye")) {
        // Validate center [cx, cy]
        if (!opObj->contains(QStringLiteral("center")) && opObj->contains(QStringLiteral("eye_center"))) {
            (*opObj)[QStringLiteral("center")] = opObj->value(QStringLiteral("eye_center"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("center"))) {
            const QJsonValue cVal = opObj->value(QStringLiteral("center"));
            if (cVal.isArray() && cVal.toArray().size() >= 2) {
                QJsonArray cArr = cVal.toArray();
                qreal cx = 0.5, cy = 0.5;
                coerceToNumber(cArr.at(0), &cx);
                coerceToNumber(cArr.at(1), &cy);
                (*opObj)[QStringLiteral("center")] = QJsonArray({qBound(-kMaxSafeCoordinate, cx, kMaxSafeCoordinate),
                                                                 qBound(-kMaxSafeCoordinate, cy, kMaxSafeCoordinate)});
            } else {
                (*opObj)[QStringLiteral("center")] = QJsonArray({0.5, 0.5});
                if (report) ++report->coercedValues;
            }
        } else {
            (*opObj)[QStringLiteral("center")] = QJsonArray({0.5, 0.5});
            if (report) ++report->coercedValues;
        }

        // Validate size [w, h]
        if (!opObj->contains(QStringLiteral("size")) && opObj->contains(QStringLiteral("eye_size"))) {
            (*opObj)[QStringLiteral("size")] = opObj->value(QStringLiteral("eye_size"));
            if (report) ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("size"))) {
            const QJsonValue sVal = opObj->value(QStringLiteral("size"));
            if (sVal.isArray() && sVal.toArray().size() >= 2) {
                QJsonArray sArr = sVal.toArray();
                qreal ew = 0.10, eh = 0.12;
                coerceToNumber(sArr.at(0), &ew);
                coerceToNumber(sArr.at(1), &eh);
                (*opObj)[QStringLiteral("size")] =
                    QJsonArray({qBound(0.0, ew, kMaxSafeCoordinate), qBound(0.0, eh, kMaxSafeCoordinate)});
            } else if (sVal.isDouble() || sVal.isString()) {
                qreal esz = 0.10;
                coerceToNumber(sVal, &esz);
                (*opObj)[QStringLiteral("size")] =
                    QJsonArray({qBound(0.0, esz, kMaxSafeCoordinate), qBound(0.0, esz * 1.2, kMaxSafeCoordinate)});
            } else {
                (*opObj)[QStringLiteral("size")] = QJsonArray({0.10, 0.12});
                if (report) ++report->coercedValues;
            }
        } else {
            (*opObj)[QStringLiteral("size")] = QJsonArray({0.10, 0.12});
            if (report) ++report->coercedValues;
        }

        // Validate iris_color & secondary_color
        if (!opObj->contains(QStringLiteral("iris_color")) && opObj->contains(QStringLiteral("color"))) {
            (*opObj)[QStringLiteral("iris_color")] = opObj->value(QStringLiteral("color"));
            if (report) ++report->coercedValues;
        }
    } else if (kindStr == QLatin1String("anime_mouth")) {
        // スキーマ・システムプロンプトが推奨する kind。ここで落とすと LLM の口が
        // 黙って消えるので、エイリアス正規化と座標クランプだけ行い parseProgramJson へ渡す。
        if (!opObj->contains(QStringLiteral("center")) && opObj->contains(QStringLiteral("mouth_center"))) {
            (*opObj)[QStringLiteral("center")] = opObj->value(QStringLiteral("mouth_center"));
            if (report)
                ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("center"))) {
            const QJsonValue cVal = opObj->value(QStringLiteral("center"));
            QJsonArray cArr = cVal.toArray();
            qreal cx = 0.0, cy = 0.0;
            if (cArr.size() >= 2 && coerceToNumber(cArr.at(0), &cx) && coerceToNumber(cArr.at(1), &cy)) {
                cArr[0] = qBound(-kMaxSafeCoordinate, cx, kMaxSafeCoordinate);
                cArr[1] = qBound(-kMaxSafeCoordinate, cy, kMaxSafeCoordinate);
                (*opObj)[QStringLiteral("center")] = cArr;
            } else {
                // 不正な型は削除し、parseProgramJson の既定値フォールバックに委ねる。
                opObj->remove(QStringLiteral("center"));
                if (report)
                    ++report->coercedValues;
            }
        }
        if (!opObj->contains(QStringLiteral("size")) && opObj->contains(QStringLiteral("mouth_size"))) {
            (*opObj)[QStringLiteral("size")] = opObj->value(QStringLiteral("mouth_size"));
            if (report)
                ++report->coercedValues;
        }
        if (opObj->contains(QStringLiteral("size"))) {
            const QJsonValue sVal = opObj->value(QStringLiteral("size"));
            QJsonArray sArr = sVal.toArray();
            qreal mw = 0.0, mh = 0.0;
            if (sArr.size() >= 2 && coerceToNumber(sArr.at(0), &mw) && coerceToNumber(sArr.at(1), &mh)) {
                sArr[0] = qBound(0.0, mw, kMaxSafeCoordinate);
                sArr[1] = qBound(0.0, mh, kMaxSafeCoordinate);
                (*opObj)[QStringLiteral("size")] = sArr;
            } else {
                opObj->remove(QStringLiteral("size"));
                if (report)
                    ++report->coercedValues;
            }
        }
        // expression / lip_color / has_highlight は parseProgramJson 側で型強制される。
    } else if (kindStr == QLatin1String("cloth_drapery") || kindStr == QLatin1String("hair_flow_cluster")
               || kindStr == QLatin1String("hand_gesture") || kindStr == QLatin1String("dynamic_pose")) {
        // High-level macro primitives: geometry is expanded downstream in KisAiStrokeProgramCodec
    } else {
        if (report) {
            ++report->typeErrors;
            report->errorMessages.append(QStringLiteral("Operation %1: unknown or unsupported kind '%2'").arg(opIndex).arg(kindStr));
        }
        return false;
    }

    return true;
}

bool KisAiStrokeTypeChecker::checkAndCoerceProgram(
    QJsonObject *programObject,
    KisAiStrokeTypeCheckReport *report)
{
    if (!programObject)
        return false;

    if (report) {
        report->isValid = true;
        report->totalCheckedOperations = 0;
        report->coercedValues = 0;
        report->typeErrors = 0;
        report->errorMessages.clear();
        report->warnings.clear();
    }

    // schema_version
    if (programObject->contains(QStringLiteral("schema_version"))) {
        const QJsonValue svVal = programObject->value(QStringLiteral("schema_version"));
        if (!svVal.isDouble()) {
            qreal schemaVersionVal = 2.0;
            // Range-check before the int conversion: a string like "1e300" passes
            // coerceToNumber's finiteness test but overflows static_cast<int>
            // (UB), after which the codec would see an out-of-range version and
            // reject the whole program. Unsupported versions fall back to 2.
            if (coerceToNumber(svVal, &schemaVersionVal)
                && schemaVersionVal >= 1.0 && schemaVersionVal <= 2.0) {
                (*programObject)[QStringLiteral("schema_version")] = static_cast<int>(schemaVersionVal);
                if (report) ++report->coercedValues;
            } else {
                (*programObject)[QStringLiteral("schema_version")] = 2;
                if (report) ++report->coercedValues;
            }
        }
    } else {
        (*programObject)[QStringLiteral("schema_version")] = 2;
    }

    // Look for operations array under various possible names
    QJsonArray ops;
    QString opsKey = QStringLiteral("operations");
    if (programObject->contains(QStringLiteral("operations")) && programObject->value(QStringLiteral("operations")).isArray()) {
        ops = programObject->value(QStringLiteral("operations")).toArray();
    } else if (programObject->contains(QStringLiteral("strokes")) && programObject->value(QStringLiteral("strokes")).isArray()) {
        opsKey = QStringLiteral("strokes");
        ops = programObject->value(QStringLiteral("strokes")).toArray();
        (*programObject)[QStringLiteral("operations")] = ops;
        if (report) ++report->coercedValues;
    } else if (programObject->contains(QStringLiteral("ops")) && programObject->value(QStringLiteral("ops")).isArray()) {
        opsKey = QStringLiteral("ops");
        ops = programObject->value(QStringLiteral("ops")).toArray();
        (*programObject)[QStringLiteral("operations")] = ops;
        if (report) ++report->coercedValues;
    }

    if (ops.isEmpty()) {
        if (report) {
            report->isValid = false;
            ++report->typeErrors;
            report->errorMessages.append(QStringLiteral("No valid operations array found in root object"));
        }
        return false;
    }

    QJsonArray validatedOps;

    for (int i = 0; i < ops.size(); ++i) {
        const QJsonValue item = ops.at(i);
        if (!item.isObject()) {
            if (report) {
                ++report->typeErrors;
                report->warnings.append(QStringLiteral("Skipping non-object item at index %1 in operations").arg(i));
            }
            continue;
        }

        QJsonObject opObj = item.toObject();
        if (checkAndCoerceOperation(&opObj, i, report)) {
            validatedOps.append(opObj);
        }
    }

    if (report) {
        report->totalCheckedOperations = static_cast<int>(ops.size());
    }

    (*programObject)[QStringLiteral("operations")] = validatedOps;
    const bool success = !validatedOps.isEmpty();
    if (report) {
        report->isValid = success;
    }
    return success;
}
