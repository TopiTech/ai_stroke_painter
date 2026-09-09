/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisAiTestUtils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>

QByteArray KisAiTestUtils::createMockChatResponse(
    const QString &assistantContent,
    const QString &model,
    const QString &finishReason)
{
    QJsonObject messageObj;
    messageObj[QStringLiteral("role")] = QStringLiteral("assistant");
    messageObj[QStringLiteral("content")] = assistantContent;

    QJsonObject choiceObj;
    choiceObj[QStringLiteral("index")] = 0;
    choiceObj[QStringLiteral("message")] = messageObj;
    choiceObj[QStringLiteral("finish_reason")] = finishReason;

    QJsonObject rootObj;
    rootObj[QStringLiteral("id")] = QStringLiteral("chatcmpl-mock12345");
    rootObj[QStringLiteral("object")] = QStringLiteral("chat.completion");
    rootObj[QStringLiteral("model")] = model;
    rootObj[QStringLiteral("choices")] = QJsonArray{choiceObj};

    return QJsonDocument(rootObj).toJson(QJsonDocument::Indented);
}

QVector<QByteArray> KisAiTestUtils::createMockSseChunks(
    const QStringList &tokens,
    const QString &model)
{
    QVector<QByteArray> chunks;
    chunks.reserve(tokens.size() + 1);

    for (const QString &tok : tokens) {
        QJsonObject deltaObj;
        deltaObj[QStringLiteral("content")] = tok;

        QJsonObject choiceObj;
        choiceObj[QStringLiteral("index")] = 0;
        choiceObj[QStringLiteral("delta")] = deltaObj;

        QJsonObject rootObj;
        rootObj[QStringLiteral("id")] = QStringLiteral("chatcmpl-mock-sse");
        rootObj[QStringLiteral("model")] = model;
        rootObj[QStringLiteral("choices")] = QJsonArray{choiceObj};

        QByteArray line = "data: " + QJsonDocument(rootObj).toJson(QJsonDocument::Compact) + "\n\n";
        chunks.append(line);
    }

    chunks.append(QByteArrayLiteral("data: [DONE]\n\n"));
    return chunks;
}

QString KisAiTestUtils::createSampleProgramJson(SampleProgramType type)
{
    switch (type) {
    case SampleProgramType::Minimal:
        return QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"prompt\": \"A minimalist test line\",\n"
            "  \"title\": \"Minimal Line\",\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"line_1\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[0.2, 0.2, 0.8], [0.8, 0.8, 0.8]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#1a1a1a\", \"size\": 0.005, \"opacity\": 1.0}\n"
            "    }\n"
            "  ]\n"
            "}"
        );

    case SampleProgramType::CharacterPortrait:
        return QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"prompt\": \"Anime girl portrait with blue eyes\",\n"
            "  \"title\": \"Girl Portrait\",\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"gradient_fill\",\n"
            "      \"id\": \"bg_gradient\",\n"
            "      \"layer\": \"Background\",\n"
            "      \"polygon\": [[0.0,0.0],[1.0,0.0],[1.0,1.0],[0.0,1.0]],\n"
            "      \"colors\": [\"#e0e7ff\", \"#fef3c7\"],\n"
            "      \"angle_deg\": 90,\n"
            "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#e0e7ff\"}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"fill\",\n"
            "      \"id\": \"face_base\",\n"
            "      \"layer\": \"Flats\",\n"
            "      \"polygon\": [[0.35,0.30],[0.65,0.30],[0.60,0.70],[0.50,0.80],[0.40,0.70]],\n"
            "      \"brush\": {\"profile\": \"brush\", \"color\": \"#fff1e6\"}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"fill\",\n"
            "      \"id\": \"face_shadow\",\n"
            "      \"layer\": \"Shading\",\n"
            "      \"polygon\": [[0.38,0.45],[0.62,0.45],[0.55,0.72],[0.45,0.72]],\n"
            "      \"brush\": {\"profile\": \"brush\", \"color\": \"#e2c2b3\", \"opacity\": 0.6}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"eye_l\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[0.40,0.48,0.9],[0.45,0.46,0.9],[0.48,0.49,0.7]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#1a1024\", \"size\": 0.004}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"path\",\n"
            "      \"id\": \"eye_r\",\n"
            "      \"layer\": \"Lineart\",\n"
            "      \"points\": [[0.52,0.49,0.7],[0.55,0.46,0.9],[0.60,0.48,0.9]],\n"
            "      \"brush\": {\"profile\": \"gpen\", \"color\": \"#1a1024\", \"size\": 0.004}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"particles\",\n"
            "      \"id\": \"eye_glint\",\n"
            "      \"layer\": \"Highlights\",\n"
            "      \"bounds\": [0.42, 0.47, 0.58, 0.51],\n"
            "      \"count\": 4,\n"
            "      \"shape\": \"sparkle\",\n"
            "      \"brush\": {\"profile\": \"neon\", \"color\": \"#ffffff\"}\n"
            "    }\n"
            "  ]\n"
            "}"
        );

    case SampleProgramType::LandscapeWashes:
        return QStringLiteral(
            "{\n"
            "  \"schema_version\": 2,\n"
            "  \"prompt\": \"Mountain sunset landscape\",\n"
            "  \"title\": \"Sunset Mountains\",\n"
            "  \"operations\": [\n"
            "    {\n"
            "      \"kind\": \"gradient_fill\",\n"
            "      \"id\": \"sky\",\n"
            "      \"layer\": \"Background\",\n"
            "      \"polygon\": [[0.0,0.0],[1.0,0.0],[1.0,0.7],[0.0,0.7]],\n"
            "      \"colors\": [\"#ff7e5f\", \"#feb47b\"],\n"
            "      \"angle_deg\": 90,\n"
            "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#ff7e5f\"}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"fill\",\n"
            "      \"id\": \"distant_mountain\",\n"
            "      \"layer\": \"Flats\",\n"
            "      \"polygon\": [[0.0,0.55],[0.3,0.35],[0.6,0.50],[1.0,0.40],[1.0,0.8],[0.0,0.8]],\n"
            "      \"brush\": {\"profile\": \"brush\", \"color\": \"#4b3869\"}\n"
            "    },\n"
            "    {\n"
            "      \"kind\": \"ribbon\",\n"
            "      \"id\": \"river\",\n"
            "      \"layer\": \"Highlights\",\n"
            "      \"spine\": [[0.45,0.6],[0.50,0.75],[0.40,0.95]],\n"
            "      \"width_start\": 0.01,\n"
            "      \"width_mid\": 0.03,\n"
            "      \"width_end\": 0.08,\n"
            "      \"brush\": {\"profile\": \"watercolor\", \"color\": \"#ffd166\"}\n"
            "    }\n"
            "  ]\n"
            "}"
        );

    case SampleProgramType::BrokenTypesToCoerce:
        return QStringLiteral(
            "{\n"
            "  \"schema_version\": \"2\",\n"
            "  \"prompt\": \"Types needing coercion\",\n"
            "  \"title\": \"Coercion Test\",\n"
            "  \"strokes\": [\n"
            "    {\n"
            "      \"type\": \"stroke\",\n"
            "      \"id\": \"\",\n"
            "      \"layer\": \"flats\",\n"
            "      \"points\": [{\"x\": \"0.15\", \"y\": \"0.25\", \"pressure\": \"0.9\"}, {\"x\": 0.85, \"y\": 0.75}],\n"
            "      \"brush\": {\"profile\": \"GPEN\", \"color\": [255, 0, 128], \"size\": \"0.012\", \"opacity\": \"0.95\", \"is_eraser\": \"false\"}\n"
            "    }\n"
            "  ]\n"
            "}"
        );

    case SampleProgramType::MixedMultiLayer:
    default:
        return createSampleProgramJson(SampleProgramType::CharacterPortrait);
    }
}

QString KisAiTestUtils::corruptJson(const QString &validJson, CorruptionType type)
{
    QString result = validJson;

    switch (type) {
    case CorruptionType::TruncatedMidway: {
        const int cutPoint = result.length() * 3 / 4;
        result = result.left(cutPoint);
        break;
    }
    case CorruptionType::MissingQuotesOnKeys:
        result.replace(QStringLiteral("\"kind\":"), QStringLiteral("kind:"));
        result.replace(QStringLiteral("\"layer\":"), QStringLiteral("layer:"));
        result.replace(QStringLiteral("\"points\":"), QStringLiteral("points:"));
        break;

    case CorruptionType::StrayComments:
        result.replace(QStringLiteral("\"operations\": ["),
                       QStringLiteral("// List of strokes\n\"operations\": [ /* operations array */"));
        break;

    case CorruptionType::FullWidthCharacters:
        result.replace(QLatin1Char('{'), QChar(0xFF5B)); // ｛
        result.replace(QLatin1Char('}'), QChar(0xFF5D)); // ｝
        result.replace(QLatin1Char(':'), QChar(0xFF1A)); // ：
        result.replace(QLatin1Char(','), QChar(0xFF0C)); // ，
        break;

    case CorruptionType::UnescapedControlCharacters:
        result.replace(QStringLiteral("\"Anime girl portrait with blue eyes\""),
                       QStringLiteral("\"Anime girl portrait\nwith\tblue eyes\""));
        break;

    case CorruptionType::NonStandardNumbers:
        result.replace(QStringLiteral("0.8"), QStringLiteral(".8"));
        result.replace(QStringLiteral("0.9"), QStringLiteral("0.900t"));
        break;

    case CorruptionType::MissingCommasBetweenObjects:
        result.replace(QStringLiteral("},\n    {"), QStringLiteral("}\n    {"));
        break;

    case CorruptionType::TrailingCommas:
        result.replace(QStringLiteral("0.8]]"), QStringLiteral("0.8],]"));
        result.replace(QStringLiteral("}\n  ]"), QStringLiteral("},\n  ]"));
        break;
    }

    return result;
}

bool KisAiTestUtils::verifyProgramStructure(
    const KisAiStrokeProgram &program,
    int minOperations,
    qreal minCompletionScore,
    QString *outFailureReason)
{
    if (program.operations.size() < minOperations) {
        if (outFailureReason) {
            *outFailureReason = QStringLiteral("Operations count (%1) is less than expected minimum (%2)")
                .arg(program.operations.size()).arg(minOperations);
        }
        return false;
    }

    if (program.completionScore < minCompletionScore) {
        if (outFailureReason) {
            *outFailureReason = QStringLiteral("Completion score (%1) is below threshold (%2)")
                .arg(program.completionScore).arg(minCompletionScore);
        }
        return false;
    }

    for (int i = 0; i < program.operations.size(); ++i) {
        const auto &op = program.operations.at(i);
        if (op.kind == KisAiStrokeOperation::Kind::Unknown) {
            if (outFailureReason) {
                *outFailureReason = QStringLiteral("Operation at index %1 has Unknown kind").arg(i);
            }
            return false;
        }
    }

    return true;
}
