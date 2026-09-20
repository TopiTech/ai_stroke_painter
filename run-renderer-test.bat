@echo off
set PATH=C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;%PATH%
set QT_PLUGIN_PATH=C:\CraftRoot\plugins
set QT_QPA_PLATFORM=offscreen
cd /d "%~dp0build-test"
.\KisAiStrokeRendererTest.exe -v2 -o renderer-result.txt,txt
echo EXIT=%ERRORLEVEL%
if exist renderer-result.txt type renderer-result.txt
