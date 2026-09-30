# Build the generic native XWMA bridge; no system DLLs are copied or bundled.
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$audioRepo = Split-Path $PSScriptRoot -Parent
$audioSource = Join-Path $PSScriptRoot 'xaudio_wma_decoder.cpp'
$audioBinary = Join-Path $PSScriptRoot 'bin\xaudio_wma_decoder.exe'
if (-not $Force -and (Test-Path -LiteralPath $audioBinary) -and
    (Get-Item -LiteralPath $audioBinary).LastWriteTimeUtc -ge (Get-Item -LiteralPath $audioSource).LastWriteTimeUtc) {
    Write-Output $audioBinary
    return
}
$audioVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $audioVswhere)) {
    throw 'Building the XWMA bridge needs Visual Studio C++ build tools, or a prebuilt tools/bin/xaudio_wma_decoder.exe.'
}
$audioVisualStudio = & $audioVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $audioVisualStudio) { throw 'No x86 C++ toolchain found for the XWMA bridge.' }
$audioVcvars = Join-Path $audioVisualStudio 'VC\Auxiliary\Build\vcvarsall.bat'
$audioObjects = Join-Path $audioRepo 'work\native_tools'
New-Item -ItemType Directory -Path (Split-Path $audioBinary), $audioObjects -Force | Out-Null
$audioObject = Join-Path $audioObjects 'xaudio_wma_decoder.obj'
$audioCommand = '"{0}" x86 >nul && cl /nologo /O2 /EHsc "{1}" /Fe:"{2}" /Fo:"{3}"' -f $audioVcvars, $audioSource, $audioBinary, $audioObject
& $env:ComSpec /d /c $audioCommand
if ($LASTEXITCODE -ne 0) { throw "XWMA bridge compilation failed ($LASTEXITCODE)." }
Write-Output $audioBinary
