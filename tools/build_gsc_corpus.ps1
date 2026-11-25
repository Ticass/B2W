# Collects the scripts of every WaW custom map installed on this PC into
# work\gsc_corpus\maps\<mod>\<zone or archive>\..., for "waw2bo2n gsc-check".
# Only .gsc/.csc files are kept; nothing here is redistributed.
param(
    [string]$Mods = "$env:LOCALAPPDATA\Activision\CoDWaW\mods",
    [string]$Out = "$PSScriptRoot\..\work\gsc_corpus\maps"
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$unlinker = "$PSScriptRoot\..\vendor\OpenAssetTools\build\bin\Release_x64\Unlinker.exe"
if (-not (Test-Path $unlinker)) { throw "Unlinker.exe is missing: $unlinker" }
[void][IO.Directory]::CreateDirectory($Out)
$Out = [IO.Path]::GetFullPath($Out)

function Safe([string]$name) { $name -replace '[^A-Za-z0-9_.-]', '_' }

foreach ($mod in Get-ChildItem -LiteralPath $Mods -Directory) {
    $folder = Join-Path $Out (Safe $mod.Name)
    if (Test-Path -LiteralPath $folder) { continue }   # delete a folder to collect it again
    $partial = "$folder.partial"
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Recurse -Force -Confirm:$false }
    [void][IO.Directory]::CreateDirectory($partial)
    foreach ($ff in Get-ChildItem -LiteralPath $mod.FullName -Recurse -Filter *.ff) {
        $zone = Join-Path $partial ('ff_' + (Safe $ff.BaseName))
        $log = & $unlinker --no-color --include-assets rawfile -o $zone $ff.FullName
        if ($LASTEXITCODE -ne 0) { Write-Warning "$($mod.Name)\$($ff.Name): $($log | Select-Object -Last 1)" }
    }
    foreach ($iwd in Get-ChildItem -LiteralPath $mod.FullName -Recurse -Filter *.iwd) {
        try { $zip = [IO.Compression.ZipFile]::OpenRead($iwd.FullName) } catch { Write-Warning "$($iwd.Name): not a zip"; continue }
        try {
            foreach ($entry in $zip.Entries) {
                if ($entry.FullName -notmatch '\.(gsc|csc)$') { continue }
                $path = [IO.Path]::GetFullPath((Join-Path (Join-Path $partial ('iwd_' + (Safe $iwd.BaseName))) ($entry.FullName -replace '[:*?"<>|]', '_')))
                if (-not $path.StartsWith($partial)) { continue }
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
                [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $path, $true)
            }
        } finally { $zip.Dispose() }
    }
    Get-ChildItem -LiteralPath $partial -Recurse -File | Where-Object { $_.Extension -notin '.gsc', '.csc' } | Remove-Item -Force -Confirm:$false
    Rename-Item -LiteralPath $partial -NewName ([IO.Path]::GetFileName($folder))
}
$scripts = @(Get-ChildItem -LiteralPath $Out -Recurse -File)
"{0} scripts from {1} maps in {2}" -f $scripts.Count, @(Get-ChildItem -LiteralPath $Out -Directory).Count, $Out
