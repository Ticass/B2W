# Debug: one collision-bisection round. Relinks the map with WAW2BO2_DEBUG_DROP,
# installs it, launches the game (the debug GSC runs the arch movement test),
# waits for its log line and closes the game.
param([string]$Drop = '', [string]$Name = 'baseline', [switch]$NoLink)
$ErrorActionPreference = 'Continue'
$env:PYTHONPATH = 'C:\WawConverter\src'
$Stage = 'C:\WawConverter\work\final_stage'; $Project = 'zm_nuketown_waw'
$OatT6 = 'C:\WawConverter\vendor\OpenAssetToolsT6\build\bin\Release_x86'
$Bo2 = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II'
if (-not $NoLink) {
$editbin = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio' -Recurse -Filter editbin.exe -ErrorAction SilentlyContinue | Where-Object FullName -like '*Hostx64\x86*' | Select-Object -First 1
& $editbin.FullName /nologo /LARGEADDRESSAWARE (Join-Path $OatT6 'Linker.exe') | Out-Null
if ($Drop) { $env:WAW2BO2_DEBUG_DROP = $Drop } else { Remove-Item Env:WAW2BO2_DEBUG_DROP -ErrorAction SilentlyContinue }
python -m waw2bo2.cli bridge-link $Stage $Project --linker (Join-Path $OatT6 'Linker.exe') --techset-dump 'C:\WawConverter\work\stock_t6_dump' --log "C:\WawConverter\work\bisect_$Name.link.log" | Out-Null
if ($LASTEXITCODE) { "link failed"; exit 1 }
Remove-Item Env:WAW2BO2_DEBUG_DROP -ErrorAction SilentlyContinue
python -m waw2bo2.cli package $Stage $Project --bo2 $Bo2 --work 'C:\WawConverter\work\mod_build' | Out-Null
if ($LASTEXITCODE) { "package failed"; exit 1 }
}
$log = "$env:LOCALAPPDATA\Plutonium\storage\t6\mods\zm_nuketown_waw\games_mp.log"
$before = if (Test-Path $log) { (Get-Content $log | Measure-Object -Line).Lines } else { 0 }
Start-Process -FilePath "$env:LOCALAPPDATA\Plutonium\bin\plutonium-bootstrapper-win32.exe" -WorkingDirectory "$env:LOCALAPPDATA\Plutonium" -ArgumentList @('t6zm', "`"$Bo2`"", '-lan', '+set', 'fs_game', 'mods/zm_nuketown_waw', '+set', 'waw2bo2_variant', $Name, '+devmap', 'zm_nuketown_waw')
$deadline = (Get-Date).AddMinutes(4)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if (Test-Path $log) {
        $new = Get-Content $log | Select-Object -Skip $before
        if ($new -match 'WAW2BO2ARCHDONE') { break }
    }
}
Stop-Process -Name plutonium-bootstrapper-win32 -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
"== $Name drop '$Drop'"
Select-String -Path "C:\WawConverter\work\bisect_$Name.link.log" -Pattern 'DEBUG: dropped' | ForEach-Object { $_.Line }
Get-Content $log | Select-Object -Skip $before | Where-Object { $_ -match 'WAW2BO2MOVE|WAW2BO2ARCHDONE' } | ForEach-Object { $_.Substring(0, [Math]::Min(150, $_.Length)) }
