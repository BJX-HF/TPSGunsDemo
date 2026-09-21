# Runs the UE Automation tests for the recoil system.
#
# Test groups:
#   Lyra.Recoil.Profile.Validation   (P1, LyraEditor module, needs content assets)
#   Lyra.Recoil.State.*              (P2, pure numeric)
#   Lyra.Recoil.Pattern.*            (P3)
#   Lyra.Recoil.Pose.*               (P4)
#   Lyra.Recoil.Dump.*               (P5)
#
# Passing a filter selects a subset, e.g. -TestFilter "Lyra.Recoil.State"
#
# Note: in -Cmd mode the automation results are written to the log file, not
# reliably to stdout, so this script re-reads Saved\Logs\<ProjectName>.log and
# prints the LogAutomationController lines. That is the evidence to keep.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\run-recoil-tests.ps1
#   powershell -ExecutionPolicy Bypass -File .\run-recoil-tests.ps1 -TestFilter "Lyra.Recoil.State"

param(
    [string]$EngineRoot = "E:\UE_5.8",
    [string]$Project    = "E:\TPSGunsDemo\TPSGunsDemo.uproject",
    [string]$TestFilter = "Lyra.Recoil"
)

$editorCmd = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$projectDir = Split-Path -Parent $Project
$projectName = [System.IO.Path]::GetFileNameWithoutExtension($Project)
$logFile = Join-Path $projectDir ("Saved\Logs\" + $projectName + ".log")

Write-Host "Running automation tests matching: $TestFilter"
Write-Host "Log file: $logFile"
Write-Host ""

& $editorCmd $Project `
    -ExecCmds="Automation RunTests $TestFilter" `
    -TestExit="Automation Test Queue Empty" `
    -unattended -nopause -nullrhi -nosplash -log | Out-Null

$editorExit = $LASTEXITCODE

Write-Host "UnrealEditor-Cmd exit code: $editorExit"
Write-Host ""
Write-Host "===== Automation results (from log) ====="

if (-not (Test-Path $logFile)) {
    Write-Host "Log file not found: $logFile"
    exit 1
}

$patterns = @(
    "LogAutomationController: Display: Found ",
    "LogAutomationController: Display: Test Started",
    "LogAutomationController: Display: Test Completed",
    "LogAutomationController: Error: Test Completed",
    "LogAutomationController: Error:",
    "LogAutomationCommandLine: Display: ...Automation Test Queue Empty"
)

$lines = Select-String -Path $logFile -Pattern $patterns -SimpleMatch | Select-Object -Last 200
foreach ($line in $lines) {
    Write-Host $line.Line
}

Write-Host ""
Write-Host "===== Summary ====="
$failCount = ($lines | Where-Object { $_.Line -like "*Result={Fail}*" }).Count
$successCount = ($lines | Where-Object { $_.Line -like "*Result={Success}*" }).Count
Write-Host "Succeeded: $successCount"
Write-Host "Failed:    $failCount"

if ($failCount -gt 0) { exit 1 }
exit $editorExit
