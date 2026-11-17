# Shadow port_map over many maps; summary lines in <out>\summary.txt.
# Usage: shadow_portmap_batch.ps1 -Out <out> -Maps "<ff>|<project>;<ff>|<project>..."
param([string]$Out, [string]$Maps)
$env:PYTHONPATH = "C:\WawConverter\src"
$env:PYTHONHASHSEED = "0"
New-Item -ItemType Directory -Force $Out | Out-Null
$summary = Join-Path $Out "summary.txt"
if (-not (Test-Path $summary)) { Set-Content $summary "" }
foreach ($entry in $Maps.Split(";")) {
    $ff, $project = $entry.Split("|")
    $dir = Join-Path $Out $project
    $lines = python C:\WawConverter\native\tools\shadow_portmap.py $ff $project $dir 2>&1 | ForEach-Object { "$_" }
    $shadow = $lines | Where-Object { $_ -match "^SHADOW|Traceback|Error" }
    Add-Content $summary "== $project"
    Add-Content $summary ($shadow | Select-Object -First 25)
    # identical: drop the staging (keep it to rerun --again on a difference)
    if ($shadow -match "only python 0, only native 0, different 0") { Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue }
}
