@echo off
set PATH=C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;%PATH%
set QT_PLUGIN_PATH=C:\CraftRoot\plugins
set QT_QPA_PLATFORM=offscreen
set KRITA_NO_ASSERT_MSG=1
set QT_ASSUME_STDERR_HAS_CONSOLE=1
cd /d "%~dp0build-test"
.\KisAiStrokeRendererTest.exe testRenderHatchOperation testHatchErasersAreShapeBounded testPhase2ModernHighFidelityAnimeEye -v1 -o hatch-fail.txt,txt
echo EXIT=%ERRORLEVEL%
type hatch-fail.txt
