# Builds the LyraEditor target for the recoil system work.
#
# IMPORTANT (2026-09-20 correction): two things are REQUIRED on this machine.
#
#   1. -UBARootDir must point somewhere outside C:\ProgramData.
#      UBA's session server cannot write its "memgroups" file under
#      C:\ProgramData\Epic\UnrealBuildAccelerator (the host sandbox denies it),
#      which makes UBT report "Result: Failed (OtherCompilationError)" even
#      though there are ZERO compile errors. Verified error text:
#        UbaSessionServer - ERROR opening file
#          C:\ProgramData\Epic\UnrealBuildAccelerator\memgroups
#          for write after retrying for 20 seconds (Access is denied.)
#
#   2. The build must be launched from the Bash tool, not the PowerShell tool.
#      Both tools are sandboxed, but Bash escalates and re-runs the command
#      unsandboxed once the sandbox denies it; PowerShell does not.
#
# -NoUBA is NOT sufficient on its own. In UE 5.8, ExecutorFactory.GetUBAExecutor()
# always constructs a UBAExecutor (see ExecutorFactory.cs L46-53); -NoUBA only sets
# Config.bAllowDetour = false (BuildConfiguration.cs L54), so the local executor
# still starts a session server and still writes memgroups.
#
# Harmless noise (do NOT treat as failure):
#   UbaSessionServer - SetFileInformationByHandle (FileDispositionInfo) failed on
#     ...\memgroups (Access is denied.)
#   ... Trace.uba (Access is denied.)
# Those are delete-on-close cleanups. The judgement call is the "Result:" line.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\build.ps1
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -Target LyraEditor -Config Development
#
# Recommended (Bash tool side, so the sandbox escalation applies):
#   cmd //c "<wrapper>.bat"   where the wrapper calls this command line.

param(
    [string]$EngineRoot = "D:\UE_5.8",
    [string]$Project    = "D:\TPSGunsDemo\TPSGunsDemo\TPSGunsDemo.uproject",
    [string]$Target     = "LyraEditor",
    [string]$Platform   = "Win64",
    [string]$Config     = "Development",
    [string]$UBARootDir = "D:\TPSGunsDemo\TPSGunsDemo\Saved\UBACache"
)

$buildBat = Join-Path $EngineRoot "Engine\Build\BatchFiles\Build.bat"

Write-Host "Build: $Target $Platform $Config"
Write-Host "Engine: $EngineRoot"
Write-Host "Project: $Project"
Write-Host "UBA root dir: $UBARootDir"
Write-Host ""

& $buildBat $Target $Platform $Config -Project="$Project" -WaitMutex -NoUBA -UBARootDir="$UBARootDir"

$exitCode = $LASTEXITCODE
Write-Host ""
Write-Host "Build.bat exit code: $exitCode"
exit $exitCode
