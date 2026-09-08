/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_PROGRAM_TEST_H
#define KIS_AI_STROKE_PROGRAM_TEST_H

#include <QObject>

class KisAiStrokeProgramTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSanitizeAndExtractJson();
    void testParseValidProgram();
    void testBuildChatCompletionsPayload();
    void testStrokeProgramJsonSchema();
};

#endif // KIS_AI_STROKE_PROGRAM_TEST_H
