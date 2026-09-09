/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_STROKE_RENDERER_TEST_H
#define KIS_AI_STROKE_RENDERER_TEST_H

#include <QObject>

class KisAiStrokeRendererTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCatmullRomSpline();
    void testRenderProgramToImage();
    void testClippingMaskToFlats();
    void testRenderGradientOpacity();
    void testRenderParticleBrush();
    void testRenderAirbrushDynamics();
    void testRenderHatchOperation();
    void testRenderRadialGradient();
    void testRenderZeroDimensionsFallback();
    void testUnnormalizedLayerRendering();
};

#endif // KIS_AI_STROKE_RENDERER_TEST_H
