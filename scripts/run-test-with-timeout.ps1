param(
    [string]$TestExe,
    [string]$TestArgs = "",
    [int]$TimeoutSec = 5
)

# Disable all Windows crash / error dialogs (SEM_NOGPFAULTERRORBOX = 0x0002)
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class NativeErrorMode {
    [DllImport("kernel32.dll")]
    public static extern uint SetErrorMode(uint uMode);
}
"@ -ErrorAction SilentlyContinue
try {
    [NativeErrorMode]::SetErrorMode(0x0001 -bor 0x0002 -bor 0x0020 -bor 0x8000) | Out-Null
} catch {}

$env:PATH = "C:\CraftRoot\bin;C:\CraftRoot\mingw64\bin;C:\CraftRoot\dev-utils\bin;$env:PATH"
$env:QT_PLUGIN_PATH = "C:\CraftRoot\plugins"
$env:QT_QPA_PLATFORM = "offscreen"

$resolvedExe = (Resolve-Path $TestExe).Path
$workDir = Split-Path -Parent $resolvedExe

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $resolvedExe
$psi.Arguments = $TestArgs
$psi.WorkingDirectory = $workDir
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi

try {
    $proc.Start() | Out-Null
    
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    
    $exited = $proc.WaitForExit($TimeoutSec * 1000)
    if (-not $exited) {
        Write-Warning "TIMED OUT after $TimeoutSec seconds. Force killing process..."
        try { $proc.Kill($true) } catch {}
        try { Stop-Process -Name "WerFault" -Force -ErrorAction SilentlyContinue } catch {}
    } else {
        [System.Threading.Tasks.Task]::WaitAll(@($outTask, $errTask), 2000) | Out-Null
        $stdout = $outTask.Result
        $stderr = $errTask.Result
        if ($stdout) { Write-Host $stdout }
        if ($stderr) { Write-Host "STDERR: $stderr" }
        Write-Host "EXIT_CODE=$($proc.ExitCode)"
    }
} finally {
    $proc.Dispose()
}
