/*
 * SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_AI_IMAGE_GUIDED_SCENE_TEST_H
#define KIS_AI_IMAGE_GUIDED_SCENE_TEST_H

#include <QObject>

class KisAiImageGuidedSceneTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testImagePlacementMatchesCenteredKeepAspectRatio();
    void testNoMaskNeverInventsGeometry();
    void testOddLetterboxPlacementMatchesImageLayer();
    void testLowConfidenceAndInvalidMaskNeverInventsGeometry();
    void testRectangleBoundaryInNonSquareCanvas();
    void testHolesAreNotContours();
    void testAlphaMaskAndBoundaryAtImageEdge();
    void testTextureEdgesDoNotBecomeStrokes();
    void testDisconnectedMaskNeedsSplit();
    void testRepeatIsDeterministic();
};

#endif // KIS_AI_IMAGE_GUIDED_SCENE_TEST_H
