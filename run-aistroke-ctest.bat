@echo off
set "PATH=C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;%PATH%"
set "QT_PLUGIN_PATH=C:\CraftRoot\plugins"
set "QT_QPA_PLATFORM=offscreen"
set "KRITA_NO_ASSERT_MSG=1"
set "QT_ASSUME_STDERR_HAS_CONSOLE=1"

:: Clean up any hung WerFault.exe or orphaned test instances before test run
taskkill /F /IM WerFault.exe 2>nul
taskkill /F /FI "IMAGENAME eq KisAi*" 2>nul

if exist "%~dp0build-ai" (
    cd /d "%~dp0build-ai"
    ctest -L AIStroke --output-on-failure --no-tests=error --timeout 30
) else (
    cd /d "%~dp0build-test"
    cmake --build . --parallel
    ctest -L AIStroke --output-on-failure --no-tests=error --timeout 30
)
set CTEST_EXIT=%ERRORLEVEL%

:: Post-test cleanup in case a crash triggered external handlers
taskkill /F /IM WerFault.exe 2>nul

echo EXIT=%CTEST_EXIT%
exit /b %CTEST_EXIT%
