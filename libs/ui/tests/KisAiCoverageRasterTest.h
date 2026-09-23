/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_COVERAGE_RASTER_TEST_H
#define KIS_AI_COVERAGE_RASTER_TEST_H

#include <QObject>

class KisAiCoverageRasterTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSanity();
    void testNoAlphaBuildupOnSelfOverlap();
    void testStrokeInteriorAlphaUniform();
    void testSharpCornerHasNoCoverageGap();
    void testLongSegmentFlatness();
    void testTaperedTipAndRoundCap();
    void testProfileTexturesModulateNotStack();
    void testFineLineNoJoinBeading();
    void testRibbonSelfOverlapNoBuildup();
    void testCornerPoolNoBeading();
    void testSampleStrokeMismatchedPressures();
    void testPaintStrokeDegenerateSize();
    void testSingleSampleFrameComputation();
};

#endif // KIS_AI_COVERAGE_RASTER_TEST_H
