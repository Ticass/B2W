<#
Zip an installed Plutonium mod for publishing:

    powershell -File tools/zip_mod.ps1 <mod folder name>

Writes %LOCALAPPDATA%\Plutonium\storage\t6\mods\<name>.zip with the mod folder
at the archive root, leaving out what the game writes while playing
(scriptdata, console_zm.log*, games_mp.log).
#>
param([Parameter(Mandatory)][string]$Project)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$mods = Join-Path $env:LOCALAPPDATA 'Plutonium\storage\t6\mods'
$source = Join-Path $mods $Project
if (-not (Test-Path -LiteralPath $source -PathType Container)) { throw "no installed mod at $source" }
$output = Join-Path $mods "$Project.zip"
$temporary = "$output.tmp"

$archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
$names = [Collections.Generic.List[string]]::new()
try {
    foreach ($file in Get-ChildItem -LiteralPath $source -Recurse -File) {
        $relative = $file.FullName.Substring($mods.Length + 1)
        if ($relative.Split('\') -contains 'scriptdata') { continue }
        if ($file.Name.StartsWith('console_zm.log') -or $file.Name -eq 'games_mp.log') { continue }
        $name = $relative.Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $name, [IO.Compression.CompressionLevel]::Optimal)
        $names.Add($name)
    }
}
finally {
    $archive.Dispose()
}
Move-Item -LiteralPath $temporary -Destination $output -Force
"$($names.Count) files"
"$output $((Get-Item -LiteralPath $output).Length)"
