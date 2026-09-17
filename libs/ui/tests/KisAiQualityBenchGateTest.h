/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_QUALITY_BENCH_GATE_TEST_H
#define KIS_AI_QUALITY_BENCH_GATE_TEST_H

#include <QObject>

class KisAiQualityBenchGateTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testGoldenSetJsonValid();
    void testBenchmarkEvaluationExistingPrompts();
    void testBenchmarkEvaluationAbstractPrompts();
    void testBenchmarkEvaluationAsymmetryPrompts();
    void testBenchmarkEvaluationComplexPrompts();
    void testBenchmarkEvaluationBoundaryPrompts();
    void testBenchmarkFullGatePass();
};

#endif // KIS_AI_QUALITY_BENCH_GATE_TEST_H
