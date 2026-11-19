<#
Assemble the portable Windows release from a built checkout:

    powershell -File tools/package_windows.ps1 [-Output work/desktop_dist]

Needs native/build (B2W.exe, waw2bo2n.exe), the OpenAssetTools builds and
tools/bin (xaudio_wma_decoder.exe, ffmpeg.exe), as tools/restore_native.ps1
and tools/fetch_wheel_binary.ps1 leave them. Writes <Output>/WawConverter
(the folder layout the converter's resource lookup expects) and
<Output>/WawConverter-Windows.zip.
#>
param([string]$Output)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $root 'work/desktop_dist' }
$Output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Output)
$bundle = Join-Path $Output 'WawConverter'

function Copy-Into([string]$Relative, [string]$Target = $Relative) {
    $source = Join-Path $root $Relative
    if (-not (Test-Path -LiteralPath $source)) { throw "missing $Relative (run tools/restore_native.ps1)" }
    $destination = Join-Path $bundle $Target
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

# a zip of a native checkout's sources (GPL corresponding source): its files
# and its thirdparty dependency checkouts, without build outputs
function New-SourceArchive([string]$Folder, [string[]]$Files, [string]$Destination) {
    $paths = [Collections.Generic.List[string]]::new()
    foreach ($file in $Files) { $paths.Add($file.Replace('/', '\')) }
    $thirdparty = Join-Path $Folder 'thirdparty'
    if (Test-Path -LiteralPath $thirdparty) {
        foreach ($file in Get-ChildItem -LiteralPath $thirdparty -Recurse -File -Force) { $paths.Add($file.FullName.Substring($Folder.Length + 1)) }
    }
    $skipped = @('.exe', '.dll', '.obj', '.pdb', '.lib', '.a', '.so', '.pyc')
    $archive = [IO.Compression.ZipFile]::Open($Destination, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new()
        foreach ($relative in $paths) {
            $full = Join-Path $Folder $relative
            $parts = $relative.Split('\')
            if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { continue }
            if ($parts -contains '.git' -or $parts[-1].StartsWith('.git') -or $parts -contains 'build' -or $skipped -contains [IO.Path]::GetExtension($full).ToLowerInvariant()) { continue }
            $name = $relative.Replace('\', '/')
            if ($seen.Add($name)) { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $full, $name, [IO.Compression.CompressionLevel]::Optimal) }
        }
    }
    finally { $archive.Dispose() }
}

if (Test-Path -LiteralPath $bundle) { Remove-Item -LiteralPath $bundle -Recurse -Force }
New-Item -ItemType Directory -Force -Path $bundle | Out-Null

# the launcher and the converter
Copy-Into 'native/build/B2W.exe' 'B2W.exe'
Copy-Into 'native/build/waw2bo2n.exe' 'waw2bo2n.exe'
Copy-Into 'VERSION'
# the converter's data and native tools, at their checkout paths
Copy-Into 'src/waw2bo2/compat'
Copy-Into 'tools/bin/xaudio_wma_decoder.exe'
Copy-Into 'tools/bin/ffmpeg.exe'
# the WaW extractor runs linked into waw2bo2n; its Unlinker marks the folder
Copy-Into 'vendor/OpenAssetTools/build/bin/Release_x64/Unlinker.exe'
Copy-Into 'vendor/OpenAssetTools/src/ObjCommon/Game/T4/Weapon/WeaponFields.h'
foreach ($name in 'Linker.exe', 'Unlinker.exe') { Copy-Into "vendor/OpenAssetToolsT6/build/bin/Release_x86/$name" }
foreach ($name in 'WeaponFields.h', 'FlameTableFields.h') { Copy-Into "vendor/OpenAssetToolsT6/src/ObjCommon/Game/T6/Weapon/$name" }
Copy-Into 'vendor/OpenAssetToolsT6/raw/t6'

# The bundled BO2 linker is large-address aware (it runs out of 2 GB on big maps).
$linker = Join-Path $bundle 'vendor/OpenAssetToolsT6/build/bin/Release_x86/Linker.exe'
$bytes = [IO.File]::ReadAllBytes($linker)
$pe = [BitConverter]::ToInt32($bytes, 0x3c)
if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw 'The bundled linker is not a valid PE executable' }
$flags = [BitConverter]::ToUInt16($bytes, $pe + 22) -bor 0x20
[BitConverter]::GetBytes([uint16]$flags).CopyTo($bytes, $pe + 22)
[IO.File]::WriteAllBytes($linker, $bytes)

# The Visual C++ runtime beside the programs (app-local redistribution): a
# fresh Windows or Wine prefix lacks it. x64 for B2W and waw2bo2n, x86 for
# the BO2 tools.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$redist = Get-ChildItem -Path (Join-Path $vs 'VC\Redist\MSVC') -Directory | Where-Object { $_.Name -match '^\d+\.' } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $redist) { throw 'no Visual C++ redistributable folder in the Visual Studio install' }
foreach ($target in @(@('x64', ''), @('x86', 'vendor/OpenAssetToolsT6/build/bin/Release_x86'))) {
    $crt = Get-ChildItem -Path (Join-Path $redist.FullName $target[0]) -Directory -Filter 'Microsoft.VC*.CRT' | Select-Object -First 1
    if (-not $crt) { throw "no $($target[0]) CRT in $($redist.FullName)" }
    foreach ($dll in Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll') {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path (Join-Path $bundle $target[1]) $dll.Name) -Force
    }
}

# documentation
Copy-Into 'docs/DESKTOP.md' 'START HERE.md'
Copy-Into 'docs/LINUX.md' 'LINUX.md'
Copy-Into 'docs/EXTRACT_ALL.md' 'Extract All.md'
Copy-Into 'docs/USAGE.md' 'Advanced usage.md'

# licenses and corresponding sources
Copy-Into 'vendor/OpenAssetToolsT6/LICENSE' 'licenses/OpenAssetTools-GPL-3.0.txt'
Copy-Into 'native/licenses/Pillow.txt' 'licenses/Pillow-MIT-CMU.txt'
Copy-Into 'vendor/imgui/LICENSE.txt' 'licenses/Dear-ImGui-MIT.txt'
[IO.File]::WriteAllText((Join-Path $bundle 'licenses/THIRD_PARTY.md'), @"
# Bundled third-party components

OpenAssetTools: GPL-3.0. Corresponding T4/T6 source archives, including dependency checkouts, are in ../source/.

Dear ImGui: MIT; see Dear-ImGui-MIT.txt.

Pillow: MIT-CMU. The native menu filter and block encoder use Pillow-compatible algorithms. See Pillow-MIT-CMU.txt.

FFmpeg (tools/bin/ffmpeg.exe): 7.1 essentials build by gyan.dev, as shipped in imageio-ffmpeg 0.6.0, GPL-3.0. License text: OpenAssetTools-GPL-3.0.txt. FFmpeg source: https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
Build scripts and library source links: https://github.com/GyanD/codexffmpeg and https://www.gyan.dev/ffmpeg/builds/

"@.Replace("`r`n", "`n"))
$source = Join-Path $bundle 'source'
New-Item -ItemType Directory -Force -Path $source | Out-Null
$t4 = Join-Path $root 'vendor/OpenAssetTools'
$t4Files = git -C $t4 ls-files --cached --others --exclude-standard
if ($LASTEXITCODE) { throw 'git ls-files failed in vendor/OpenAssetTools' }
New-SourceArchive $t4 $t4Files (Join-Path $source 'OpenAssetTools-T4.zip')
$prefix = 'vendor/OpenAssetToolsT6/'
$t6Files = git -C $root ls-files $prefix | ForEach-Object { $_.Substring($prefix.Length) }
if ($LASTEXITCODE) { throw 'git ls-files failed for vendor/OpenAssetToolsT6' }
New-SourceArchive (Join-Path $root 'vendor/OpenAssetToolsT6') $t6Files (Join-Path $source 'OpenAssetTools-T6.zip')
foreach ($name in 'xaudio_wma_decoder.cpp', 'build_audio_decoder.ps1', 'restore_native.ps1', 'package_windows.ps1') { Copy-Into "tools/$name" "source/$name" }

$zip = Join-Path $Output 'WawConverter-Windows.zip'
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
[IO.Compression.ZipFile]::CreateFromDirectory($bundle, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
"Portable folder: $bundle"
"Portable download: $zip"
