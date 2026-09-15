# SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
# SPDX-License-Identifier: GPL-2.0-or-later

<#
.SYNOPSIS
    Runs static type checking, syntax verification, and code quality analysis on AI Stroke Painter codebase.

.DESCRIPTION
    Checks AI Stroke Painter source files (libs/ui/aiillustration) and test suites (libs/ui/tests/KisAi*)
    using clang-tidy and compile-time diagnostics from compile_commands.json.

.PARAMETER CraftRoot
    Path to KDE Craft installation root (default: C:\CraftRoot).

.PARAMETER File
    Specific file name to check (e.g. KisAiDeliberateStroke.cpp or KisAiStrokeProgramTest.cpp).
    If omitted, all AI Stroke Painter source files and tests are checked.

.PARAMETER Fix
    Instructs clang-tidy to automatically apply proposed fixes.

.PARAMETER SyntaxOnly
    Fast compilation & syntax/type-check mode using ninja/compiler without running full clang-tidy AST linters.

.PARAMETER SkipTests
    Skips checking test files in libs/ui/tests.

.PARAMETER WarningsAsErrors
    Treat all warnings as errors, causing non-zero exit code if any warning is reported.
#>

[CmdletBinding()]
param(
    [string]$CraftRoot = "C:\CraftRoot",
    [string]$File = "",
    [switch]$Fix,
    [switch]$SyntaxOnly,
    [switch]$SkipTests,
    [switch]$WarningsAsErrors
)

$ErrorActionPreference = "Continue"

$craftBin = "$CraftRoot\bin"
$craftDev = "$CraftRoot\dev-utils\bin"
$craftMingw = "$CraftRoot\mingw64\bin"
$env:PATH = "$craftBin;$craftDev;$craftMingw;" + $env:PATH

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$aiDir = Join-Path $repoRoot "libs\ui\aiillustration"
$testDir = Join-Path $repoRoot "libs\ui\tests"

# Detect compilation database
$buildDir = if (Test-Path "$repoRoot\build-ai\compile_commands.json") {
    "$repoRoot\build-ai"
} elseif (Test-Path "$repoRoot\build-test\compile_commands.json") {
    "$repoRoot\build-test"
} elseif (Test-Path "$repoRoot\compile_commands.json") {
    "$repoRoot"
} else {
    $null
}

# Ensure root compile_commands.json is synced with build-ai (and sanitized for Clang tooling)
$rootDb = Join-Path $repoRoot "compile_commands.json"
if ($buildDir -and (Test-Path "$buildDir\compile_commands.json")) {
    $srcDb = "$buildDir\compile_commands.json"
    $needSync = (-not (Test-Path $rootDb)) -or ((Get-Item $srcDb).LastWriteTime -gt (Get-Item $rootDb).LastWriteTime)
    if ($needSync) {
        try {
            $rawJson = Get-Content $srcDb -Raw
            # Strip GCC-only flags that Clang tooling (clangd, clang-tidy) cannot parse
            $cleanJson = $rawJson -replace '\s+-fext-numeric-literals', '' -replace '\s+-fno-keep-inline-dllexport', ''
            [System.IO.File]::WriteAllText($rootDb, $cleanJson)
        } catch {
            Copy-Item $srcDb $rootDb -Force -ErrorAction SilentlyContinue
        }
    }
}
$activeDbDir = if (Test-Path $rootDb) { "$repoRoot" } else { $buildDir }

# Resolve target source files
$targetFiles = [System.Collections.Generic.List[string]]::new()

if ($File) {
    if (Test-Path $File) {
        $targetFiles.Add((Resolve-Path $File).Path)
    } elseif (Test-Path (Join-Path $aiDir $File)) {
        $targetFiles.Add((Resolve-Path (Join-Path $aiDir $File)).Path)
    } elseif (Test-Path (Join-Path $testDir $File)) {
        $targetFiles.Add((Resolve-Path (Join-Path $testDir $File)).Path)
    } else {
        Write-Error "Target file '$File' not found in $aiDir or $testDir."
        exit 1
    }
} else {
    Get-ChildItem -Path $aiDir -Filter *.cpp | ForEach-Object { $targetFiles.Add($_.FullName) }
    if (-not $SkipTests -and (Test-Path $testDir)) {
        Get-ChildItem -Path $testDir -Filter KisAi*.cpp | ForEach-Object { $targetFiles.Add($_.FullName) }
    }
}

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " AI Stroke Painter Type & Code Quality Checker" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Target files count : $($targetFiles.Count)"
Write-Host "Build database     : $(if ($buildDir) { $buildDir } else { 'None (using fallback include flags)' })"
Write-Host "Mode               : $(if ($SyntaxOnly) { 'Fast Syntax & Type Check (Compiler/Ninja)' } else { 'Clang-Tidy Deep Static Analysis' })"
Write-Host ""

# Fast Syntax-Only mode using ninja
if ($SyntaxOnly) {
    $ninja = Get-Command "ninja.exe" -ErrorAction SilentlyContinue
    if (-not $ninja) {
        Write-Error "ninja.exe not found in PATH or $CraftRoot."
        exit 1
    }

    if (-not $buildDir) {
        Write-Error "Fast syntax-only mode requires an existing build directory with build files (e.g. build-ai)."
        exit 1
    }

    Write-Host "==> Running compiler syntax & type check via ninja..." -ForegroundColor Cyan

    $targets = @()
    if ($File) {
        # Check specific file target
        $leaf = Split-Path -Leaf $File
        $targets += "libs/ui/CMakeFiles/kritaui.dir/aiillustration/$leaf.obj"
    } else {
        # Check test targets and kritaui AI Stroke Painter objects
        $targets += "KisAiStrokeProgramTest"
        $targets += "KisAiStrokeRendererTest"
        $targets += "KisAiIllustrationRendererTest"
        $targets += "KisAiV5EngineTest"
        $targets += "KisAiV6WiringTest"
    }

    $ninjaArgs = @("-C", $buildDir) + $targets
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ninjaOutput = & $ninja.Source @ninjaArgs 2>&1
    $exitCode = $LASTEXITCODE
    $sw.Stop()

    if ($exitCode -eq 0) {
        Write-Host "==> [PASS] All type and syntax checks passed in $($sw.Elapsed.TotalSeconds.ToString('F2'))s." -ForegroundColor Green
        exit 0
    } else {
        Write-Host "==> [FAIL] Type or syntax errors detected in compiler output:" -ForegroundColor Red
        if ($ninjaOutput) {
            $ninjaOutput | ForEach-Object {
                if ($_ -match 'error:') {
                    Write-Host "  $_" -ForegroundColor Red
                } elseif ($_ -match 'warning:') {
                    Write-Host "  $_" -ForegroundColor Yellow
                } else {
                    Write-Host "  $_" -ForegroundColor Gray
                }
            }
        }
        exit 1
    }
}

# Full clang-tidy deep static analysis mode
$clangTidy = Get-Command "clang-tidy.exe" -ErrorAction SilentlyContinue
if (-not $clangTidy) {
    Write-Error "clang-tidy.exe was not found in PATH or $CraftRoot."
    exit 1
}
Write-Host "Clang-Tidy binary  : $($clangTidy.Source)"
Write-Host ""

$includeArgs = @(
    "-I$repoRoot\libs\ui",
    "-I$repoRoot\libs\ui\aiillustration",
    "-I$repoRoot\build-ai\libs\ui",
    "-I$repoRoot\libs\global",
    "-I$repoRoot\libs\command",
    "-I$repoRoot\libs\resources",
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

$passedFiles = 0
$warnedFiles = 0
$failedFiles = 0
$totalErrors = 0
$totalWarnings = 0
$errorDetails = [System.Collections.Generic.List[string]]::new()

foreach ($filePath in $targetFiles) {
    $fileName = Split-Path -Leaf $filePath
    Write-Host "Checking $fileName... " -NoNewline

    $tidyArgs = @(
        $filePath,
        "--config-file=$repoRoot\.clang-tidy",
        "--quiet"
    )

    if ($Fix) {
        $tidyArgs += "--fix"
    }

    if ($activeDbDir) {
        $tidyArgs += "-p"
        $tidyArgs += $activeDbDir
        $tidyArgs += "--extra-arg=-Qunused-arguments"
        $tidyArgs += "--extra-arg=-Wno-unknown-warning-option"
        $tidyArgs += "--extra-arg=-Wno-unused-command-line-argument"
    } else {
        $tidyArgs += "--"
        $tidyArgs += $includeArgs
    }

    $rawOutput = & $clangTidy.Source @tidyArgs 2>&1
    $exitCode = $LASTEXITCODE

    $fileErrors = 0
    $fileWarnings = 0
    $filteredLines = [System.Collections.Generic.List[string]]::new()

    if ($rawOutput) {
        foreach ($line in $rawOutput) {
            $lineStr = "$line"
            if ($lineStr -match 'error:\s+unknown argument:') {
                # GCC driver flag incompatibility with Clang, ignore
                continue
            }
            if ($lineStr -match 'error:\s+' -or $lineStr -match 'fatal error:\s+' -or $lineStr -match 'clang-diagnostic-error') {
                $fileErrors++
                $filteredLines.Add("  [ERROR] $lineStr")
            } elseif ($lineStr -match 'warning:\s+') {
                $fileWarnings++
                $filteredLines.Add("  [WARN]  $lineStr")
            }
        }
    }

    $totalErrors += $fileErrors
    $totalWarnings += $fileWarnings

    if ($fileErrors -gt 0) {
        $failedFiles++
        Write-Host "[FAIL] ($fileErrors error(s), $fileWarnings warning(s))" -ForegroundColor Red
        foreach ($errLine in $filteredLines) {
            if ($errLine -match '\[ERROR\]') {
                Write-Host $errLine -ForegroundColor Red
            }
        }
        $errorDetails.Add("$($fileName): $($fileErrors) error(s)")
    } elseif ($fileWarnings -gt 0) {
        $warnedFiles++
        Write-Host "[WARN] ($fileWarnings warning(s))" -ForegroundColor Yellow
        # Only show preview of first few warnings to keep terminal readable
        $previewCount = [Math]::Min($filteredLines.Count, 3)
        for ($i = 0; $i -lt $previewCount; $i++) {
            Write-Host $filteredLines[$i] -ForegroundColor DarkGray
        }
        if ($filteredLines.Count -gt 3) {
            Write-Host "  ... and $($filteredLines.Count - 3) more warning(s)" -ForegroundColor DarkGray
        }
    } else {
        $passedFiles++
        Write-Host "[PASS]" -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Type Check Summary" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Checked Files : $($targetFiles.Count)"
Write-Host "Passed Files  : $passedFiles" -ForegroundColor Green
Write-Host "Warned Files  : $warnedFiles" -ForegroundColor Yellow
Write-Host "Failed Files  : $failedFiles" -ForegroundColor Red
Write-Host "Total Errors  : $totalErrors" -ForegroundColor $(if ($totalErrors -gt 0) { "Red" } else { "Green" })
Write-Host "Total Warnings: $totalWarnings" -ForegroundColor $(if ($totalWarnings -gt 0) { "Yellow" } else { "Green" })

if ($totalErrors -gt 0) {
    Write-Host "`nErrors were detected in the following files:" -ForegroundColor Red
    foreach ($detail in $errorDetails) {
        Write-Host " - $detail" -ForegroundColor Red
    }
    exit 1
}

if ($WarningsAsErrors -and $totalWarnings -gt 0) {
    Write-Host "`nWarnings treated as errors (-WarningsAsErrors enabled)." -ForegroundColor Red
    exit 1
}

Write-Host "`n==> Type and quality check completed successfully." -ForegroundColor Green
exit 0
