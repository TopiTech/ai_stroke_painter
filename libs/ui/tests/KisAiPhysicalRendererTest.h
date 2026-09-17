/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_PHYSICAL_RENDERER_TEST_H
#define KIS_AI_PHYSICAL_RENDERER_TEST_H

#include <QObject>

class KisAiPhysicalRendererTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSrgbToLinearAndLinearToSrgbRoundtrip();
    void testBlendMultiplyValues();
    void testBlendScreenValues();
    void testBlendOverlayValues();
    void testBlendSoftLightValues();
    void testBlendColorDodgeValues();
    void testBlendLinearBurnValues();
    void testBlendPixelOpaqueOverOpaque();
    void testBlendPixelTransparentSrc();
    void testBlendPixelTransparentDst();
    void testBlendPixelPartialOpacity();
    void testHdrFormatSupported();
    void testToLinearHdrAndToSrgbLdrRoundtrip();
    void testCompositeLayerNormal();
    void testCompositeLayerMultiply();
    void testCompositeLayerWithClipMask();
    void testCompositeGraphEvaluate();
    void testCompositeGraphFindLayer();
    void testDownsampleBox();
    void testRenderProgramToPhysicalImage();
    void testCompositeLayerMismatchedSize();
    void testBlendPixelNanAndInfProtection();
};

#endif // KIS_AI_PHYSICAL_RENDERER_TEST_H
