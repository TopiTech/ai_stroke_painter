/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PERCEPTUAL_REPAIRER_TEST_H
#define KIS_AI_PERCEPTUAL_REPAIRER_TEST_H

#include <QObject>

class KisAiPerceptualRepairerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDiagnoseEmptyProgram();
    void testDiagnoseFlatsHole();
    void testAutoRepairFlatsHole();
    void testDiagnoseShadingOverSpill();
    void testDiagnoseHatchOnFace();
    void testAutoRepairHatchOnFace();
    void testDiagnoseAsymmetryEye();
    void testConsentRequiredForAsymmetry();
    void testDiagnoseColorBanding();
    void testDiagnoseLineartThicknessJitter();
    void testPlanIssueCounts();
    void testApplyAutoFixesOnly();
    void testApplyWithConsent();
    void testAutoRepairConvenience();
    void testCleanProgramProducesZeroIssues();
    void testMultipleDropOpStability();
    void testColorBandingOnLargeImage();
    void testHatchOnFaceDetectsPointsOnlyHatch();
    void testColorBandingSkipsSharpLineart();
    void testJitterFixTargetsOnlyJitteryOps();
};

#endif // KIS_AI_PERCEPTUAL_REPAIRER_TEST_H
