#requires -Version 7.0

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$craftRoot = if ([string]::IsNullOrWhiteSpace($env:CRAFT_ROOT)) { 'C:\CraftRoot' } else { $env:CRAFT_ROOT }
$buildDir = 'build-ai'
$installPrefix = Join-Path $craftRoot 'ai-stroke-painter'

# Craft/MinGW/CMake dependencies
$env:PATH = @(
    (Join-Path $craftRoot 'dev-utils\meson-venv\Scripts')
    (Join-Path $craftRoot 'bin')
    (Join-Path $craftRoot 'mingw64\bin')
    (Join-Path $craftRoot 'dev-utils\bin')
    $env:PATH
) -join ';'
$env:PKG_CONFIG_PATH = Join-Path $craftRoot 'lib\pkgconfig'

Write-Host 'Configuring the CMake build...' -ForegroundColor Cyan
cmake -S . -B $buildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_WITH_QT6=ON `
    -DALLOW_UNSTABLE=QT6 `
    -DBUILD_TESTING=ON `
    -DAI_STROKE_PAINTER_BUILD_UPSTREAM_TESTS=OFF `
    "-DCMAKE_C_COMPILER=$craftRoot\mingw64\bin\gcc.exe" `
    "-DCMAKE_CXX_COMPILER=$craftRoot\mingw64\bin\g++.exe" `
    "-DCMAKE_PREFIX_PATH=$craftRoot"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

# アプリケーション本体と AI Stroke Painter の検証テストを並列ビルドします。
# 実行ファイル名は ai-stroke-painter ですが、CMake のアプリケーションターゲット名は krita です。
Write-Host 'Building the application and AI Stroke Painter tests...' -ForegroundColor Cyan
cmake --build $buildDir `
    --target krita KisAiStrokeProgramTest KisAiStrokeRendererTest KisAiIllustrationRendererTest KisAiV5EngineTest KisAiV6WiringTest KisAiV7QualityTest KisAiQualityVectorTest KisAiPerceptualRepairerTest KisAiPhysicalRendererTest KisAiAbstractOntologyTest KisAiQualityBenchGateTest KisAiAtomicInkTest KisAiV10QualityTest KisAiStartPageTest KisAiLineartModeTest `
    --parallel

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE."
}

Write-Host "Installing to $installPrefix..." -ForegroundColor Cyan
cmake --install $buildDir --prefix $installPrefix

if ($LASTEXITCODE -ne 0) {
    throw "Installation failed with exit code $LASTEXITCODE."
}

Write-Host 'Configuration, build, and installation completed successfully.' -ForegroundColor Green
