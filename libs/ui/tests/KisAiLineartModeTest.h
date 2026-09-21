/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_LINEART_MODE_TEST_H
#define KIS_AI_LINEART_MODE_TEST_H

#include <QObject>

/**
 * Lineart Mode (Pure Line Art & Manga Inking) Comprehensive Tests:
 * - Semantic prompt classification for lineart, inking, coloring book keywords
 * - Pure Lineart art direction text and zero-fill constraint checks
 * - Multi-stage Goal Mode inking phase guidance verification
 * - LayoutEngine lineartProgram generation (pristine white canvas, zero color fills, inking hierarchy)
 * - EyePairLineartOps assembly (uncolored iris contours, pupil cores, catchlight boundary rings)
 * - Automatic style routing in KisAiLayoutEngine::generateProgram
 * - Full raster rendering verification to QImage
 */
class KisAiLineartModeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testLineartPromptDetection();
    void testPureLineartArtDirection();
    void testPureLineartGoalPhases();
    void testLineartProgramGeneration();
    void testLineartEyeAssembly();
    void testGenerateProgramAutomaticLineartRouting();
    void testLineartRenderingExecution();
};

#endif // KIS_AI_LINEART_MODE_TEST_H
