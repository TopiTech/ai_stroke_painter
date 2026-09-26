@echo off
set PATH=C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;%PATH%
set QT_PLUGIN_PATH=C:\CraftRoot\plugins
set QT_QPA_PLATFORM=offscreen
set KRITA_NO_ASSERT_MSG=1
set QT_ASSUME_STDERR_HAS_CONSOLE=1
cd /d "%~dp0build-test"
del /q atomic-*.txt 2>nul
.\KisAiAtomicInkTest.exe -v2 -o atomic-result.txt,txt
echo EXIT=%ERRORLEVEL%
echo --- breadcrumbs ---
dir /b atomic-*.txt
if exist atomic-result.txt type atomic-result.txt
