param(
    [string]$BuildDir = "",
    [int]$TotalTimeoutSec = 120
)

# Disable crash dialogs for this process and any child processes
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class NativeCrashGuard {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@ -ErrorAction SilentlyContinue
try {
    [NativeCrashGuard]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x0020 -bor 0x8000) | Out-Null
} catch {}

$env:PATH = "C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;$env:PATH"
$env:QT_PLUGIN_PATH = "C:\CraftRoot\plugins"
$env:QT_QPA_PLATFORM = "offscreen"

$repoRoot = (Get-Item $PSScriptRoot).Parent.FullName
$buildDir = if (-not [string]::IsNullOrWhiteSpace($BuildDir)) {
    (Resolve-Path $BuildDir).Path
} elseif (Test-Path (Join-Path $repoRoot "build-ai")) {
    Join-Path $repoRoot "build-ai"
} else {
    Join-Path $repoRoot "build-test"
}

Write-Host "Building project in $buildDir..."
$buildPsi = New-Object System.Diagnostics.ProcessStartInfo
$buildPsi.FileName = "cmake.exe"
$buildPsi.Arguments = "--build . --parallel"
$buildPsi.WorkingDirectory = $buildDir
$buildPsi.UseShellExecute = $false
$buildProc = [System.Diagnostics.Process]::Start($buildPsi)
if (-not $buildProc.WaitForExit(60000)) {
    Write-Error "Build timed out after 60 seconds!"
    try { $buildProc.Kill($true) } catch {}
    exit 1
}
if ($buildProc.ExitCode -ne 0) {
    Write-Error "Build failed with exit code $($buildProc.ExitCode)"
    exit $buildProc.ExitCode
}

Write-Host "Running CTest with 15s per-test timeout and $TotalTimeoutSec s total timeout..."
$ctestPsi = New-Object System.Diagnostics.ProcessStartInfo
$ctestPsi.FileName = "ctest.exe"
$ctestPsi.Arguments = "-L AIStroke --output-on-failure --no-tests=error --timeout 15"
$ctestPsi.WorkingDirectory = $buildDir
$ctestPsi.UseShellExecute = $false
$ctestPsi.RedirectStandardOutput = $true
$ctestPsi.RedirectStandardError = $true
$ctestPsi.CreateNoWindow = $true

$ctestProc = New-Object System.Diagnostics.Process
$ctestProc.StartInfo = $ctestPsi

try {
    $ctestProc.Start() | Out-Null
    $outTask = $ctestProc.StandardOutput.ReadToEndAsync()
    $errTask = $ctestProc.StandardError.ReadToEndAsync()

    $completed = $ctestProc.WaitForExit($TotalTimeoutSec * 1000)
    if (-not $completed) {
        Write-Error "CTest suite timed out after $TotalTimeoutSec seconds! Force killing..."
        try { $ctestProc.Kill($true) } catch {}
        try { Stop-Process -Name "WerFault" -Force -ErrorAction SilentlyContinue } catch {}
        exit 1
    }
    [System.Threading.Tasks.Task]::WaitAll(@($outTask, $errTask), 2000) | Out-Null
    $stdout = $outTask.Result
    $stderr = $errTask.Result
    Write-Host $stdout
    if ($stderr) { Write-Host "STDERR: $stderr" }
    Write-Host "CTEST_EXIT_CODE=$($ctestProc.ExitCode)"
    exit $ctestProc.ExitCode
} finally {
    $ctestProc.Dispose()
}
