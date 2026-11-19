<#
Rebuild a checked-in T6 32-bit menu wrapper with hksc configured for T6:

    powershell -File tools/compile_menu_layers.ps1 <hksc.exe> [menu_layers|menu_preview|waw_hud|waw_hud_entry|waw_menu|...]

Compiles src/waw2bo2/compat/<wrapper>.lua to <wrapper>.luac, lists it, and
records both files' SHA-256 in <wrapper>.json (the converter checks them).
#>
param(
    [Parameter(Mandatory, Position = 0)][string]$Compiler,
    [Parameter(Position = 1)][string]$Wrapper = 'menu_layers'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$source = "src/waw2bo2/compat/$Wrapper.lua"
$binary = "src/waw2bo2/compat/$Wrapper.luac"
if (-not (Test-Path -LiteralPath (Join-Path $root $source))) { throw "no wrapper source $source" }
Push-Location $root
try {
    & $compilerPath -mwindows -m32 -s -o $binary $source
    if ($LASTEXITCODE) { throw "hksc failed compiling $source" }
    & $compilerPath -mwindows -m32 -b -p $binary
    if ($LASTEXITCODE) { throw "hksc failed listing $binary" }
    $hash = { param($path) (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }
    $json = "{`n  `"source_sha256`": `"$(& $hash $source)`",`n  `"bytecode_sha256`": `"$(& $hash $binary)`"`n}`n"
    [IO.File]::WriteAllText((Join-Path $root "src/waw2bo2/compat/$Wrapper.json"), $json)
}
finally {
    Pop-Location
}
