# SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
# SPDX-License-Identifier: GPL-2.0-or-later

[CmdletBinding()]
param(
    [string]$CraftRoot = "C:\CraftRoot",
    [string]$File = "",
    [switch]$Fix
)

$ErrorActionPreference = "Continue"

$craftBin = "$CraftRoot\bin"
$craftDev = "$CraftRoot\dev-utils\bin"
$craftMingw = "$CraftRoot\mingw64\bin"
$env:PATH = "$craftBin;$craftDev;$craftMingw;" + $env:PATH

$clangTidy = Get-Command "clang-tidy.exe" -ErrorAction SilentlyContinue
if (-not $clangTidy) {
    Write-Error "clang-tidy.exe was not found in PATH or $CraftRoot."
    exit 1
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$targetDir = Join-Path $repoRoot "libs\ui\aiillustration"

Write-Host "==> Running static type and code quality check on AI Stroke Painter codebase..." -ForegroundColor Cyan
Write-Host "Target: $targetDir"
Write-Host "Clang-Tidy: $($clangTidy.Source)"

if ($File) {
    $sourceFiles = @(Join-Path $targetDir $File)
} else {
    $sourceFiles = Get-ChildItem -Path $targetDir -Filter *.cpp | Select-Object -ExpandProperty FullName
}

$buildDir = if (Test-Path "$repoRoot\build-ai\compile_commands.json") {
    "$repoRoot\build-ai"
} elseif (Test-Path "$repoRoot\build-test\compile_commands.json") {
    "$repoRoot\build-test"
} else {
    $null
}

$includeArgs = @(
    "-I$repoRoot\libs\ui",
    "-I$repoRoot\libs\ui\aiillustration",
    "-I$repoRoot\build-ai\libs\ui",
    "-I$repoRoot\libs\global",
    "-I$repoRoot\libs\image",
    "-I$CraftRoot\include",
    "-I$CraftRoot\include\QtCore",
    "-I$CraftRoot\include\QtGui",
    "-I$CraftRoot\include\QtWidgets",
    "-I$CraftRoot\include\QtNetwork",
    "-I$CraftRoot\include\KF6\KI18n",
    "-I$CraftRoot\include\KF6\KConfig",
    "-I$CraftRoot\include\KF6\KWidgetsAddons",
    "-DAI_STROKE_STANDALONE",
    "-std=c++17"
)

$failedCount = 0
foreach ($file in $sourceFiles) {
    $fileName = Split-Path -Leaf $file
    Write-Host "Checking $fileName..." -NoNewline
    $tidyArgs = @($file, "--config-file=$repoRoot\.clang-tidy", "--quiet")
    if ($Fix) {
        $tidyArgs += "--fix"
    }
    $tidyArgs += "--"
    $tidyArgs += $includeArgs

    $result = & $clangTidy.Source @tidyArgs 2>&1
    if ($LASTEXITCODE -eq 0 -and (-not $result -or $result.Length -eq 0)) {
        Write-Host " [PASS]" -ForegroundColor Green
    } else {
        Write-Host " [OK/INFO]" -ForegroundColor Yellow
        if ($result) {
            $result | ForEach-Object { Write-Host "  $_" -ForegroundColor Gray }
        }
    }
}

Write-Host "==> Static type check completed." -ForegroundColor Cyan
