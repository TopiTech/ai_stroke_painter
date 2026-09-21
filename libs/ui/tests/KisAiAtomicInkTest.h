/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_ATOMIC_INK_TEST_H
#define KIS_AI_ATOMIC_INK_TEST_H

#include <QObject>

class KisAiAtomicInkTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSanity();
    void testEyeExpandsToAtomicLashes();
    void testMouthExpandsToLipPaths();
    void testHatchBecomesPaths();
    void testLegacyAnimeEyeJsonStillPaints();
    void testZeroCoverageSkipped();
    void testNeedsRepairSelfIntersectionFixed();
    void testLintRunsOnStabilized();
    void testCommitLogDeterministic();
    void testSymmetricEyeRolesInterleaved();
    void testFringeIsMultipleStrokes();
    void testJawBeforeLashes();
    void testGroupCritiqueRetriesLashOnly();
    void testTStopSnapsToParent();
    void testFineLineUsesEnvelope();
    void testTaperedTipVsRoundStart();
    void testNoBowtieOnSharpCorner();
    void testAtomicStrokeRatioAfterExpand();
    void testSideTokenStrictness();
    void testZeroDensityMangaLinesExpandsToNothing();
    void testDegenerateHatchExpandsToNothing();
    void testReviewPixelsRejectsMismatchedImages();
    void testReviewPixelsMixedFormatsAndLargeRegion();
    void testPreviewCanvasParityPsnr();
    void testPhysicalPathUsesCommitter();
};

#endif // KIS_AI_ATOMIC_INK_TEST_H
