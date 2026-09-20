# Generates the recoil profile data assets (P1 deliverables).
#
# Creates /Game/Weapons/Recoil/DA_Recoil_Rifle, _Pistol, _Shotgun.
# Idempotent: existing assets are SKIPPED unless -Force is passed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\gen-recoil-assets.ps1
#   powershell -ExecutionPolicy Bypass -File .\gen-recoil-assets.ps1 -Force

param(
    [string]$EngineRoot = "E:\UE_5.8",
    [string]$Project    = "E:\TPSGunsDemo\TPSGunsDemo.uproject",
    [switch]$Force
)

$editorCmd = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

$extraArgs = @()
if ($Force.IsPresent) {
    $extraArgs += "-force"
    Write-Host "Force mode: existing assets will be overwritten."
}

& $editorCmd $Project -run=LyraRecoilAssetGen -unattended -nopause -nullrhi -nosplash -log @extraArgs

$exitCode = $LASTEXITCODE
Write-Host ""
Write-Host "UnrealEditor-Cmd exit code: $exitCode"
exit $exitCode
