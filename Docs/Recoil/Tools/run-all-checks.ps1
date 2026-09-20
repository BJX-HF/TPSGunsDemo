# One-shot verification for the recoil system: build -> generate assets -> golden -> tests.
#
# This is the command to run before asking for acceptance on any phase.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\run-all-checks.ps1
#   powershell -ExecutionPolicy Bypass -File .\run-all-checks.ps1 -SkipAssets

param(
    [string]$EngineRoot = "D:\UE_5.8",
    [string]$Project    = "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject",
    [switch]$SkipAssets
)

$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildBat = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"

$overallExit = 0

Write-Host "############ 1/4  BUILD ############"
& $buildBat LyraEditor Win64 Development -Project="$Project" -WaitMutex -NoUBA
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED"; exit 1 }

if (-not $SkipAssets) {
    Write-Host ""
    Write-Host "############ 2/4  GENERATE ASSETS ############"
    & powershell -ExecutionPolicy Bypass -File (Join-Path $toolsDir "gen-recoil-assets.ps1")
    if ($LASTEXITCODE -ne 0) { Write-Host "ASSET GENERATION FAILED"; $overallExit = 1 }
}

Write-Host ""
Write-Host "############ 3/4  GOLDEN DATA ############"
& powershell -ExecutionPolicy Bypass -File (Join-Path $toolsDir "gen-recoil-golden.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "GOLDEN DUMP FAILED"; $overallExit = 1 }

Write-Host ""
Write-Host "############ 4/4  AUTOMATION TESTS ############"
& powershell -ExecutionPolicy Bypass -File (Join-Path $toolsDir "run-recoil-tests.ps1")
if ($LASTEXITCODE -ne 0) { Write-Host "TESTS FAILED"; $overallExit = 1 }

Write-Host ""
if ($overallExit -eq 0) {
    Write-Host "############ ALL CHECKS PASSED ############"
} else {
    Write-Host "############ SOME CHECKS FAILED ############"
}

exit $overallExit
