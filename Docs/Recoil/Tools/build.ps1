# Builds the LyraEditor target for the recoil system work.
#
# IMPORTANT: -NoUBA is mandatory on this machine.
# Without it, UnrealBuildAccelerator fails to clean up its temp files
# (SetFileInformationByHandle / FileDispositionInfo is blocked by the host),
# and UBT reports "Result: Failed (OtherCompilationError)" even though the
# actual compilation succeeded. With -NoUBA the classic local executor is used
# and the build completes normally.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target LyraEditor -Config Development

param(
    [string]$EngineRoot = "D:\UE_5.8",
    [string]$Project    = "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject",
    [string]$Target     = "LyraEditor",
    [string]$Platform   = "Win64",
    [string]$Config     = "Development"
)

$buildBat = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"

Write-Host "Build: $Target $Platform $Config"
Write-Host "Engine: $EngineRoot"
Write-Host "Project: $Project"
Write-Host ""

& $buildBat $Target $Platform $Config -Project="$Project" -WaitMutex -NoUBA

$exitCode = $LASTEXITCODE
Write-Host ""
Write-Host "Build.bat exit code: $exitCode"
exit $exitCode
