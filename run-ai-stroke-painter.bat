@echo off
if "%CRAFT_ROOT%"=="" set "CRAFT_ROOT=C:\CraftRoot"
set "PACKAGE_BIN=%CRAFT_ROOT%\ai-stroke-painter\bin"
set "PATH=%PACKAGE_BIN%;%CRAFT_ROOT%\bin;%CRAFT_ROOT%\mingw64\bin;%PATH%"
set "QT_PLUGIN_PATH=%CRAFT_ROOT%\plugins"
set "QT_QPA_PLATFORM_PLUGIN_PATH=%CRAFT_ROOT%\plugins\platforms"

start "" "%PACKAGE_BIN%\ai-stroke-painter.exe" %*
