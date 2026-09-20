@echo off
set PATH=C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;%PATH%
set QT_PLUGIN_PATH=C:\CraftRoot\plugins
set QT_QPA_PLATFORM=offscreen
cd /d "%~dp0build-test"
cmake --build . --parallel
ctest -L AIStroke --output-on-failure --no-tests=error
echo EXIT=%ERRORLEVEL%
