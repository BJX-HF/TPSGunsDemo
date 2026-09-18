# Regenerates the P3 recoil pattern golden data.
#
# Writes Source\LyraGame\Tests\Data\RecoilGolden_<asset>.json for the three
# shipped profiles (Rifle / Pistol / Shotgun), 20 shots each, using
# FRecoilRuntimeState::ComputeShotKick with PoseMultiplier = 1, GlobalScale = 1
# and the seed taken from the asset.
#
# MUST be re-run after changing any of these asset fields, otherwise
# Lyra.Recoil.Pattern.Golden will fail with "golden data is stale":
#   FixedRandomSeed, RandomSeedMode, PatternLength, PatternPoints,
#   RecoilPerShot_Vertical / _Horizontal, VerticalKickCurve, HorizontalRandomRange
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\gen-recoil-golden.ps1

param(
    [string]$EngineRoot = "D:\UE_5.8",
    [string]$Project    = "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject"
)

$editorCmd = Join-Path $EngineRoot "Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$projectDir = Split-Path -Parent $Project
$projectName = [System.IO.Path]::GetFileNameWithoutExtension($Project)
$logFile = Join-Path $projectDir ("Saved\Logs\" + $projectName + ".log")

Write-Host "Regenerating recoil golden data..."
Write-Host ""

& $editorCmd $Project -run=LyraRecoilGoldenDump -unattended -nopause -nullrhi -nosplash -log | Out-Null

$exitCode = $LASTEXITCODE
Write-Host "UnrealEditor-Cmd exit code: $exitCode"
Write-Host ""
Write-Host "===== Golden dump log ====="

if (Test-Path $logFile) {
    Select-String -Path $logFile -Pattern "LogLyraRecoilGolden" -SimpleMatch | Select-Object -Last 20 | ForEach-Object { Write-Host $_.Line }
}

$dataDir = Join-Path $projectDir "Source\LyraGame\Tests\Data"
Write-Host ""
Write-Host "===== Files in $dataDir ====="
if (Test-Path $dataDir) {
    Get-ChildItem -Path $dataDir -Filter "RecoilGolden_*.json" | ForEach-Object {
        Write-Host ("{0}  {1} bytes  {2}" -f $_.Name, $_.Length, $_.LastWriteTime)
    }
}

exit $exitCode
