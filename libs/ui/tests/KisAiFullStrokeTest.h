/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_FULL_STROKE_TEST_H
#define KIS_AI_FULL_STROKE_TEST_H

#include <QObject>

class KisAiFullStrokeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testTextOnlyChatPayload();
    void testSceneValidationAndBudget();
    void testImageEndpointRejected();
    void testMissingProgramRejected();
};

#endif // KIS_AI_FULL_STROKE_TEST_H
