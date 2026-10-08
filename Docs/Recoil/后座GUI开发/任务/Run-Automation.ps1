param(
    [string]$Phase = 'P0-Baseline',
    [string]$Filter = 'Lyra.Recoil'
)
$ErrorActionPreference = 'Stop'
trap { [Console]::Error.WriteLine($_); exit 1 }
$projectRoot = 'D:\TPSGunsDemo\TPSGunsDemo'
$runTag = Get-Date -Format 'yyyyMMdd-HHmmss'
$reportPath = Join-Path $projectRoot "Saved\Automation\RecoilGUI-$Phase-$runTag"
$logPath = Join-Path $projectRoot "Saved\Logs\RecoilGUI-$Phase-$runTag.log"
& 'D:\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' (Join-Path $projectRoot 'TPSGunsDemo.uproject') "-ExecCmds=Automation RunTests $Filter" '-TestExit=Automation Test Queue Empty' "-ReportExportPath=$reportPath" "-abslog=$logPath" -unattended -nopause -nullrhi -nosplash
$editorExit = $LASTEXITCODE
$indexPath = Join-Path $reportPath 'index.json'
if (-not (Test-Path -LiteralPath $indexPath)) { throw "Missing current report: $indexPath; process exit $editorExit" }
$report = Get-Content -LiteralPath $indexPath -Raw -Encoding utf8 | ConvertFrom-Json
foreach ($key in @('failed','notRun','inProcess','succeeded','succeededWithWarnings','tests')) {
    if ($report.PSObject.Properties.Name -notcontains $key) { throw "Missing report field: $key" }
}
$summary = [ordered]@{phase=$Phase; filter=$Filter; processExit=$editorExit; report=$indexPath; log=$logPath; succeeded=$report.succeeded; succeededWithWarnings=$report.succeededWithWarnings; failed=$report.failed; notRun=$report.notRun; inProcess=$report.inProcess; tests=@($report.tests | ForEach-Object { $_.fullTestPath })}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot "Docs\Recoil\后座GUI开发\验收\$Phase-$runTag.json") -Encoding utf8
$summary | ConvertTo-Json -Depth 8
if ($editorExit -ne 0 -or $report.failed -ne 0 -or $report.notRun -ne 0 -or $report.inProcess -ne 0 -or ($report.succeeded + $report.succeededWithWarnings) -eq 0) { throw 'Current automation run is incomplete or failed.' }
