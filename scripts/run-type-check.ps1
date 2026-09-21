# SPDX-FileCopyrightText: 2026 AI Stroke Painter contributors
# SPDX-License-Identifier: GPL-2.0-or-later

<#
.SYNOPSIS
    High-performance static type checking, syntax verification, and code quality analysis on AI Stroke Painter codebase.

.DESCRIPTION
    Checks AI Stroke Painter source files (libs/ui/aiillustration) and test suites (libs/ui/tests/KisAi*)
    using parallel clang-tidy, intelligent caching, header diagnostics, and compile-time diagnostics.

.PARAMETER File
    Specific file name or path to check (e.g. KisAiDeliberateStroke.cpp or KisAiStrokeProgramTest.cpp).
    If omitted, all target files are checked.

.PARAMETER CraftRoot
    Path to KDE Craft installation root (default: C:\CraftRoot).

.PARAMETER Jobs
    Number of parallel worker processes for Clang-Tidy (default: min(logical cores, 8)).

.PARAMETER Fix
    Instructs clang-tidy to automatically apply proposed fixes.

.PARAMETER SyntaxOnly
    Fast compilation & syntax/type-check mode using ninja/compiler without running full clang-tidy AST linters.
    Compiles individual .obj targets directly to eliminate link overhead.

.PARAMETER SkipTests
    Skips checking test files in libs/ui/tests.

.PARAMETER ChangedOnly
    Only check files modified or untracked in Git workspace.

.PARAMETER StagedOnly
    Only check files staged in Git index.

.PARAMETER Deep
    Enables deep static analysis (including clang-analyzer-* symbolic execution).
    By default, fast AST & type safety analysis is used for rapid feedback.

.PARAMETER NoCache
    Bypasses cache and forces re-analysis of all matching files.

.PARAMETER ClearCache
    Clears cached analysis results and exits.

.PARAMETER SummaryOnly
    Suppresses detailed warning previews and only outputs final summary and errors.

.PARAMETER WarningsAsErrors
    Treat all warnings as errors, causing non-zero exit code if any warning is reported.
#>

[CmdletBinding(DefaultParameterSetName = "Standard")]
param(
    [Parameter(Position = 0)]
    [string]$File = "",

    [string]$CraftRoot = "C:\CraftRoot",

    [int]$Jobs = 0,
    [switch]$Fix,
    [switch]$SyntaxOnly,
    [switch]$SkipTests,
    [switch]$ChangedOnly,
    [switch]$StagedOnly,
    [switch]$Deep,
    [switch]$NoCache,
    [switch]$ClearCache,
    [switch]$SummaryOnly,
    [switch]$WarningsAsErrors
)

$ErrorActionPreference = "Continue"

# -----------------------------------------------------------------------------
# 1. Environment & Paths Configuration
# -----------------------------------------------------------------------------
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$cacheDir = Join-Path $repoRoot ".cache"
$cacheFile = Join-Path $cacheDir "typecheck-cache.json"

if ($ClearCache) {
    if (Test-Path $cacheFile) {
        Remove-Item -Path $cacheFile -Force
        Write-Host "==> Cleared type check cache: $cacheFile" -ForegroundColor Green
    } else {
        Write-Host "==> No cache file found to clear." -ForegroundColor Gray
    }
    exit 0
}

# Configure PATH for Craft toolchain
$craftBin = "$CraftRoot\bin"
$craftDev = "$CraftRoot\dev-utils\bin"
$craftMingw = "$CraftRoot\mingw64\bin"
$craftMesonScripts = "$CraftRoot\dev-utils\meson-venv\Scripts"
$env:PATH = "$craftBin;$craftDev;$craftMingw;$craftMesonScripts;" + $env:PATH

$aiDir = Join-Path $repoRoot "libs\ui\aiillustration"
$testDir = Join-Path $repoRoot "libs\ui\tests"

# Parallel jobs auto-detection
if ($Jobs -le 0) {
    $cpuCores = [Environment]::ProcessorCount
    $Jobs = [Math]::Max(1, [Math]::Min($cpuCores, 8))
}

if ($Fix -and $Jobs -gt 1) {
    Write-Host "==> [-Fix enabled] Forcing Jobs = 1 to prevent concurrent write collisions on shared headers." -ForegroundColor Yellow
    $Jobs = 1
}

# -----------------------------------------------------------------------------
# 2. Compilation Database Detection & Sync
# -----------------------------------------------------------------------------
$buildDir = if (Test-Path "$repoRoot\build-ai\compile_commands.json") {
    "$repoRoot\build-ai"
} elseif (Test-Path "$repoRoot\build-test\compile_commands.json") {
    "$repoRoot\build-test"
} elseif (Test-Path "$repoRoot\compile_commands.json") {
    "$repoRoot"
} else {
    $null
}

$rootDb = Join-Path $repoRoot "compile_commands.json"
if ($buildDir -and (Test-Path "$buildDir\compile_commands.json")) {
    $srcDb = "$buildDir\compile_commands.json"
    $needSync = (-not (Test-Path $rootDb)) -or ((Get-Item $srcDb).LastWriteTimeUtc -gt (Get-Item $rootDb).LastWriteTimeUtc)
    if ($needSync) {
        Write-Host "==> Synchronizing & sanitizing compile_commands.json for Clang tooling..." -ForegroundColor DarkGray
        try {
            $rawJson = [System.IO.File]::ReadAllText($srcDb)
            # Strip GCC-only flags that Clang tooling (clangd, clang-tidy) cannot parse
            $cleanJson = $rawJson -replace '\s+-fext-numeric-literals', '' `
                                  -replace '\s+-fno-keep-inline-dllexport', '' `
                                  -replace '\s+-mthreads', ''
            [System.IO.File]::WriteAllText($rootDb, $cleanJson)
        } catch {
            Copy-Item $srcDb $rootDb -Force -ErrorAction SilentlyContinue
        }
    }
}
$activeDbDir = if (Test-Path $rootDb) { "$repoRoot" } else { $buildDir }

# -----------------------------------------------------------------------------
# 3. Target Source Files Resolution
# -----------------------------------------------------------------------------
$allAiFiles = [System.Collections.Generic.List[string]]::new()
if (Test-Path $aiDir) {
    Get-ChildItem -Path $aiDir -Filter *.cpp | ForEach-Object { $allAiFiles.Add($_.FullName) }
}

$allTestFiles = [System.Collections.Generic.List[string]]::new()
if (-not $SkipTests -and (Test-Path $testDir)) {
    Get-ChildItem -Path $testDir -Filter KisAi*.cpp | ForEach-Object { $allTestFiles.Add($_.FullName) }
}

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
} elseif ($ChangedOnly -or $StagedOnly) {
    $gitArgs = if ($StagedOnly) { @("diff", "--name-only", "--cached") } else { @("status", "--porcelain") }
    $gitOut = & git @gitArgs 2>&1
    $changedPaths = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)

    if ($gitOut) {
        foreach ($line in $gitOut) {
            $rawPath = if ($StagedOnly) { "$line".Trim() } else { "$line".Substring(3).Trim() }
            if ($rawPath -match '->') {
                $rawPath = ($rawPath -split '->')[-1].Trim()
            }
            if ($rawPath) {
                $absPath = Join-Path $repoRoot $rawPath
                $changedPaths.Add($absPath) | Out-Null
            }
        }
    }

    # If any header was changed, include corresponding cpp or all ai cpp
    foreach ($p in $changedPaths) {
        if ($p -match '\.h$') {
            $stem = [System.IO.Path]::GetFileNameWithoutExtension($p)
            $candidateCpp = Join-Path $aiDir "$stem.cpp"
            if (Test-Path $candidateCpp) {
                $targetFiles.Add((Resolve-Path $candidateCpp).Path)
            }
            $candidateTest = Join-Path $testDir "$stem.cpp"
            if (Test-Path $candidateTest) {
                $targetFiles.Add((Resolve-Path $candidateTest).Path)
            }
        } elseif ($p -match '\.cpp$') {
            if ($allAiFiles.Contains($p) -or $allTestFiles.Contains($p)) {
                $targetFiles.Add($p)
            }
        }
    }

    if ($targetFiles.Count -eq 0) {
        Write-Host "==> No changed AI Stroke Painter source files detected in Git." -ForegroundColor Yellow
        exit 0
    }
} else {
    $targetFiles.AddRange($allAiFiles)
    $targetFiles.AddRange($allTestFiles)
}

# Deduplicate
$targetFiles = [System.Collections.Generic.List[string]]::new([System.Linq.Enumerable]::Distinct($targetFiles))

Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " AI Stroke Painter Type & Code Quality Checker" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Target files count : $($targetFiles.Count)"
Write-Host "Build database     : $(if ($activeDbDir) { $activeDbDir } else { 'None (using fallback include flags)' })"
Write-Host "Analysis Mode      : $(if ($SyntaxOnly) { 'Fast Syntax & Type Check (Ninja/Compiler)' } elseif ($Deep) { 'Clang-Tidy Deep Symbolic Analysis' } else { 'Clang-Tidy High-Speed AST & Type Quality' })"
Write-Host "Concurrency        : $(if ($SyntaxOnly) { 'Managed by Ninja' } else { "$Jobs parallel worker(s)" })"
Write-Host "Caching            : $(if ($NoCache) { 'Disabled (-NoCache)' } else { 'Enabled' })"
Write-Host ""

# -----------------------------------------------------------------------------
# 4. Fast Syntax-Only Mode (Compiler/Ninja)
# -----------------------------------------------------------------------------
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

    Write-Host "==> Resolving ninja compile targets..." -ForegroundColor Cyan
    $availableTargets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $rawNinjaTargets = & $ninja.Source "-C" $buildDir "-t" "targets" "all" 2>&1
    if ($rawNinjaTargets) {
        foreach ($tLine in $rawNinjaTargets) {
            $tName = ("$tLine" -split ":")[0].Trim()
            if ($tName) {
                $availableTargets.Add($tName) | Out-Null
            }
        }
    }

    $targets = [System.Collections.Generic.List[string]]::new()

    foreach ($file in $targetFiles) {
        $leaf = Split-Path -Leaf $file
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($leaf)
        $resolved = $null

        if ($file -like "*libs\ui\aiillustration*") {
            $candidate = "libs/ui/CMakeFiles/kritaui.dir/aiillustration/$leaf.obj"
            if ($availableTargets.Contains($candidate)) {
                $resolved = $candidate
            }
        } elseif ($file -like "*libs\ui\tests*") {
            $candidate1 = "libs/ui/tests/CMakeFiles/$stem.dir/$leaf.obj"
            if ($availableTargets.Contains($candidate1)) {
                $resolved = $candidate1
            } else {
                # Look for matching obj target (e.g. KisAiTestUtils.cpp.obj)
                foreach ($t in $availableTargets) {
                    if ($t.EndsWith("/$leaf.obj", [System.StringComparison]::OrdinalIgnoreCase) -or $t.EndsWith("\$leaf.obj", [System.StringComparison]::OrdinalIgnoreCase)) {
                        $resolved = $t
                        break
                    }
                }
                # Fallback to test executable/phony target
                if (-not $resolved -and $availableTargets.Contains($stem)) {
                    $resolved = $stem
                }
            }
        }

        if ($resolved) {
            if (-not $targets.Contains($resolved)) {
                $targets.Add($resolved)
            }
        } else {
            Write-Host "  [WARN] No ninja target found for $leaf, skipping direct object compilation." -ForegroundColor Yellow
        }
    }

    Write-Host "==> Compiling $($targets.Count) object target(s) via ninja (-k 0)..." -ForegroundColor Cyan
    $ninjaArgs = @("-C", $buildDir, "-k", "0") + $targets
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ninjaOutput = & $ninja.Source @ninjaArgs 2>&1
    $exitCode = $LASTEXITCODE
    $sw.Stop()

    if ($exitCode -eq 0) {
        Write-Host "==> [PASS] All $($targets.Count) targets passed type and syntax checks in $($sw.Elapsed.TotalSeconds.ToString('F2'))s." -ForegroundColor Green
        exit 0
    } else {
        Write-Host "==> [FAIL] Type or syntax errors detected in compiler output:" -ForegroundColor Red
        if ($ninjaOutput) {
            $ninjaOutput | ForEach-Object {
                $l = "$_"
                if ($l -match 'error:') {
                    Write-Host "  $l" -ForegroundColor Red
                } elseif ($l -match 'warning:') {
                    Write-Host "  $l" -ForegroundColor Yellow
                } elseif ($l -match 'FAILED:') {
                    Write-Host "  $l" -ForegroundColor Magenta
                } else {
                    Write-Host "  $l" -ForegroundColor Gray
                }
            }
        }
        exit 1
    }
}

# -----------------------------------------------------------------------------
# 5. Clang-Tidy Deep / Fast Static Analysis
# -----------------------------------------------------------------------------
$clangTidy = Get-Command "clang-tidy.exe" -ErrorAction SilentlyContinue
if (-not $clangTidy) {
    Write-Error "clang-tidy.exe was not found in PATH or $CraftRoot."
    exit 1
}
Write-Host "Clang-Tidy binary  : $($clangTidy.Source)"
Write-Host ""

# Load Cache
$cacheData = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::OrdinalIgnoreCase)
$clangTidyConfigHash = ""
$clangTidyConfigFile = Join-Path $repoRoot ".clang-tidy"
if (Test-Path $clangTidyConfigFile) {
    $clangTidyConfigHash = (Get-FileHash -Path $clangTidyConfigFile -Algorithm SHA256).Hash
}

if (-not $NoCache -and (Test-Path $cacheFile)) {
    try {
        $rawCache = Get-Content -Path $cacheFile -Raw -ErrorAction SilentlyContinue | ConvertFrom-Json
        if ($rawCache.configHash -eq $clangTidyConfigHash -and $rawCache.isDeep -eq [bool]$Deep) {
            foreach ($entry in $rawCache.entries.PSObject.Properties) {
                $cacheData[$entry.Name] = $entry.Value
            }
        }
    } catch {
        $cacheData = [System.Collections.Generic.Dictionary[string, object]]::new([System.StringComparer]::OrdinalIgnoreCase)
    }
}

# Clang Extra Arguments for enhanced type safety & GCC compatibility
$extraArgs = @(
    "--extra-arg=-Qunused-arguments",
    "--extra-arg=-Wno-unknown-warning-option",
    "--extra-arg=-Wno-unused-command-line-argument",
    "--extra-arg=-Wconversion",
    "--extra-arg=-Wsign-conversion",
    "--extra-arg=-Wfloat-conversion",
    "--extra-arg=-Wnull-dereference",
    "--extra-arg=-Wshadow",
    "--extra-arg=-Wuninitialized"
)

# Header filter to diagnose project headers while excluding external noise
$headerFilterArg = "--header-filter=(aiillustration/.*|tests/KisAi.*)"

# Checks filter definition
$checksArg = if ($Deep) {
    $null # Use complete .clang-tidy definition as configured
} else {
    # High-speed checks: AST linting, type checks, modernize, bugprone, performance, readability, excluding heavy symbolic analyzers
    "--checks=-*,clang-diagnostic-*,bugprone-*,cert-*,cppcoreguidelines-*,modernize-*,performance-*,readability-*,portability-*,misc-*,-clang-analyzer-*,-readability-magic-numbers,-readability-named-parameter,-modernize-use-trailing-return-type,-cppcoreguidelines-avoid-magic-numbers,-readability-redundant-member-init,-readability-else-after-return,-readability-implicit-bool-conversion,-cppcoreguidelines-owning-memory,-cppcoreguidelines-pro-bounds-pointer-arithmetic,-cppcoreguidelines-pro-type-vararg,-readability-uppercase-literal-suffix,-bugprone-easily-swappable-parameters"
}

# Fallback include arguments when compilation database is absent
$fallbackIncludes = @(
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

# Prepare items to analyze (evaluating cache)
$itemsToProcess = [System.Collections.Generic.List[psobject]]::new()
$cachedResults = [System.Collections.Generic.List[psobject]]::new()

foreach ($filePath in $targetFiles) {
    $fileItem = Get-Item $filePath
    $fileHash = (Get-FileHash -Path $filePath -Algorithm SHA256).Hash
    $cacheKey = $filePath

    if (-not $NoCache -and $cacheData.ContainsKey($cacheKey)) {
        $cached = $cacheData[$cacheKey]
        if ($cached.hash -eq $fileHash) {
            $cachedResults.Add([PSCustomObject]@{
                FilePath      = $filePath
                FileName      = $fileItem.Name
                IsCached      = $true
                Errors        = [int]$cached.errors
                Warnings      = [int]$cached.warnings
                ErrorLines    = @($cached.errorLines)
                WarningLines  = @($cached.warningLines)
                CheckCounts   = $cached.checkCounts
                Duration      = 0.0
            })
            continue
        }
    }

    $itemsToProcess.Add([PSCustomObject]@{
        FilePath = $filePath
        FileName = $fileItem.Name
        FileHash = $fileHash
    })
}

Write-Host "Files to analyze   : $($itemsToProcess.Count)"
Write-Host "Cached clean files : $($cachedResults.Count)"
Write-Host ""

$swTotal = [System.Diagnostics.Stopwatch]::StartNew()
$processedResults = [System.Collections.Generic.List[psobject]]::new()

if ($itemsToProcess.Count -gt 0) {
    Write-Host "==> Starting parallel analysis ($Jobs worker threads)..." -ForegroundColor Cyan

    $tidyBin = $clangTidy.Source
    $cfgFile = $clangTidyConfigFile
    $dbDir = $activeDbDir

    # Execute in parallel using PowerShell 7 ForEach-Object -Parallel and stream results live
    $totalToProcess = $itemsToProcess.Count
    $completedCount = 0

    $itemsToProcess | ForEach-Object -ThrottleLimit $Jobs -Parallel {
        $item = $_
        $filePath = $item.FilePath
        $fileName = $item.FileName
        $fHash = $item.FileHash

        $tidyArgs = [System.Collections.Generic.List[string]]::new()
        $tidyArgs.Add($filePath)
        $tidyArgs.Add("--config-file=$using:cfgFile")
        $tidyArgs.Add("--quiet")
        $tidyArgs.Add($using:headerFilterArg)

        if ($using:checksArg) {
            $tidyArgs.Add($using:checksArg)
        }

        if ($using:Fix) {
            $tidyArgs.Add("--fix")
        }

        if ($using:dbDir) {
            $tidyArgs.Add("-p")
            $tidyArgs.Add($using:dbDir)
            foreach ($ea in $using:extraArgs) {
                $tidyArgs.Add($ea)
            }
        } else {
            $tidyArgs.Add("--")
            foreach ($inc in $using:fallbackIncludes) {
                $tidyArgs.Add($inc)
            }
        }

        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $rawOutput = & $using:tidyBin @tidyArgs 2>&1
        $sw.Stop()

        $fileErrors = 0
        $fileWarnings = 0
        $errLines = [System.Collections.Generic.List[string]]::new()
        $warnLines = [System.Collections.Generic.List[string]]::new()
        $checkCounts = [System.Collections.Generic.Dictionary[string, int]]::new()

        if ($rawOutput) {
            foreach ($line in $rawOutput) {
                $lineStr = "$line".Trim()
                if (-not $lineStr) { continue }
                if ($lineStr -match 'error:\s+unknown argument:') {
                    # Clang compatibility ignore
                    continue
                }
                if ($lineStr -match '([0-9]+)\s+warnings?\s+and\s+([0-9]+)\s+errors?\s+generated\.') {
                    continue
                }
                if ($lineStr -match '([0-9]+)\s+warnings?\s+generated\.') {
                    continue
                }
                if ($lineStr -match '^Error while processing') {
                    continue
                }

                if ($lineStr -match 'error:\s+' -or $lineStr -match 'fatal error:\s+' -or $lineStr -match '\[clang-diagnostic-error\]') {
                    $fileErrors++
                    $errLines.Add($lineStr)
                } elseif ($lineStr -match 'warning:\s+') {
                    $fileWarnings++
                    $warnLines.Add($lineStr)

                    if ($lineStr -match '\[([a-zA-Z0-9\-_]+)\]$') {
                        $cName = $Matches[1]
                        if (-not $checkCounts.ContainsKey($cName)) {
                            $checkCounts[$cName] = 0
                        }
                        $checkCounts[$cName]++
                    }
                }
            }
        }

        # Convert dictionary to PSCustomObject for cross-runspace serialization
        $countsObj = [PSCustomObject]@{}
        foreach ($kv in $checkCounts.GetEnumerator()) {
            $countsObj | Add-Member -MemberType NoteProperty -Name $kv.Key -Value $kv.Value
        }

        [PSCustomObject]@{
            FilePath     = $filePath
            FileName     = $fileName
            FileHash     = $fHash
            IsCached     = $false
            Errors       = $fileErrors
            Warnings     = $fileWarnings
            ErrorLines   = @($errLines)
            WarningLines = @($warnLines)
            CheckCounts  = $countsObj
            Duration     = $sw.Elapsed.TotalSeconds
        }
    } | ForEach-Object {
        $r = $_
        $completedCount++
        $statusStr = if ($r.Errors -gt 0) { "FAIL ($($r.Errors))" } elseif ($r.Warnings -gt 0) { "WARN ($($r.Warnings))" } else { "PASS" }
        $color = if ($r.Errors -gt 0) { "Red" } elseif ($r.Warnings -gt 0) { "Yellow" } else { "Green" }
        Write-Host ("  [{0,2}/{1,2}] ({2,4:F1}s) {3,-32} : {4}" -f $completedCount, $totalToProcess, $r.Duration, $r.FileName, $statusStr) -ForegroundColor $color
        $processedResults.Add($r)
    }
}

$swTotal.Stop()

# Combine cached & fresh results and sort deterministically
$allResults = @(@($cachedResults) + @($processedResults) | Sort-Object -Property FileName)

# -----------------------------------------------------------------------------
# 6. Presentation & Diagnostics Reporting
# -----------------------------------------------------------------------------
Write-Host "==> Diagnostics Results:" -ForegroundColor Cyan

$passedCount = 0
$warnedCount = 0
$failedCount = 0
$totalErrors = 0
$totalWarnings = 0
$globalCheckCounts = [System.Collections.Generic.Dictionary[string, int]]::new()
$failedSummary = [System.Collections.Generic.List[string]]::new()

$newCacheEntries = @{}

foreach ($res in $allResults) {
    $fn = $res.FileName
    $errs = $res.Errors
    $warns = $res.Warnings
    $durStr = if ($res.IsCached) { "cached" } else { "$($res.Duration.ToString('F1'))s" }

    $totalErrors += $errs
    $totalWarnings += $warns

    # Update cache entry
    if (-not $res.IsCached -and $res.FileHash) {
        $newCacheEntries[$res.FilePath] = @{
            hash         = $res.FileHash
            errors       = $errs
            warnings     = $warns
            errorLines   = $res.ErrorLines
            warningLines = $res.WarningLines
            checkCounts  = $res.CheckCounts
        }
    } elseif ($res.IsCached -and $cacheData.ContainsKey($res.FilePath)) {
        $newCacheEntries[$res.FilePath] = $cacheData[$res.FilePath]
    }

    # Aggregate check categories
    if ($res.CheckCounts) {
        foreach ($prop in $res.CheckCounts.PSObject.Properties) {
            $name = $prop.Name
            $val = [int]$prop.Value
            if (-not $globalCheckCounts.ContainsKey($name)) {
                $globalCheckCounts[$name] = 0
            }
            $globalCheckCounts[$name] += $val
        }
    }

    if ($errs -gt 0) {
        $failedCount++
        Write-Host "  [FAIL] $fn ($errs error(s), $warns warning(s)) [$durStr]" -ForegroundColor Red
        $failedSummary.Add("$fn : $errs error(s)")
        foreach ($e in $res.ErrorLines) {
            Write-Host "         [ERROR] $e" -ForegroundColor Red
        }
    } elseif ($warns -gt 0) {
        $warnedCount++
        Write-Host "  [WARN] $fn ($warns warning(s)) [$durStr]" -ForegroundColor Yellow
        if (-not $SummaryOnly) {
            $previewCount = [Math]::Min($res.WarningLines.Count, 2)
            for ($i = 0; $i -lt $previewCount; $i++) {
                Write-Host "         $($res.WarningLines[$i])" -ForegroundColor DarkGray
            }
            if ($res.WarningLines.Count -gt 2) {
                Write-Host "         ... and $($res.WarningLines.Count - 2) more warning(s)" -ForegroundColor DarkGray
            }
        }
    } else {
        $passedCount++
        Write-Host "  [PASS] $fn [$durStr]" -ForegroundColor Green
    }
}

# Save Cache
if (-not $NoCache) {
    try {
        if (-not (Test-Path $cacheDir)) {
            New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
        }
        $cachePayload = @{
            version    = 1
            configHash = $clangTidyConfigHash
            isDeep     = [bool]$Deep
            entries    = $newCacheEntries
        }
        $cacheJson = $cachePayload | ConvertTo-Json -Depth 5
        [System.IO.File]::WriteAllText($cacheFile, $cacheJson)
    } catch {
        # Non-fatal if cache save fails
    }
}

# -----------------------------------------------------------------------------
# 7. Summary & Quality Metrics
# -----------------------------------------------------------------------------
Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host " Type Check Summary" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "Total Target Files  : $($allResults.Count)"
Write-Host "Freshly Analyzed    : $($itemsToProcess.Count)"
Write-Host "Cache Hit Files     : $($cachedResults.Count)" -ForegroundColor $(if ($cachedResults.Count -gt 0) { "Cyan" } else { "Gray" })
Write-Host "Passed Files        : $passedCount" -ForegroundColor Green
Write-Host "Warned Files        : $warnedCount" -ForegroundColor $(if ($warnedCount -gt 0) { "Yellow" } else { "Green" })
Write-Host "Failed Files        : $failedCount" -ForegroundColor $(if ($failedCount -gt 0) { "Red" } else { "Green" })
Write-Host "Total Errors        : $totalErrors" -ForegroundColor $(if ($totalErrors -gt 0) { "Red" } else { "Green" })
Write-Host "Total Warnings      : $totalWarnings" -ForegroundColor $(if ($totalWarnings -gt 0) { "Yellow" } else { "Green" })
Write-Host "Analysis Duration   : $($swTotal.Elapsed.TotalSeconds.ToString('F2'))s"

# Top warning categories
if ($globalCheckCounts.Count -gt 0) {
    Write-Host ""
    Write-Host "Top Warning Categories:" -ForegroundColor DarkCyan
    $sortedChecks = $globalCheckCounts.GetEnumerator() | Sort-Object -Property Value -Descending | Select-Object -First 7
    foreach ($entry in $sortedChecks) {
        Write-Host ("  {0,-48} : {1,4}" -f $entry.Key, $entry.Value) -ForegroundColor Gray
    }
}

if ($totalErrors -gt 0) {
    Write-Host "`nErrors were detected in the following files:" -ForegroundColor Red
    foreach ($detail in $failedSummary) {
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
