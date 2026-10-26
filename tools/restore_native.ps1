# Restore the native tools of a developer checkout from what the repo pins:
#   vendor/OpenAssetTools    T4 extractor: upstream 7d027e8 + vendor/OpenAssetTools.patch
#   vendor/OpenAssetToolsT6  T6 bridge: tracked source + third-party snapshot 95b8c68
#   premake 5.0.0-beta8      verified against the T4 generate.bat checksum
# Every step is skipped when its result already exists, so rerunning after a
# partial failure continues where it stopped. Nothing tracked is modified.
#   powershell -ExecutionPolicy Bypass -File tools\restore_native.ps1
# Parallelism is deliberately modest: full-width MSBuild/cl runs out of memory
# beside a running game. Raise -Projects/-Compilers on an idle machine.
param([int]$Projects = 2, [int]$Compilers = 4, [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Test-Empty([string]$Path) {
    return -not (Test-Path -LiteralPath $Path) -or -not (Get-ChildItem -LiteralPath $Path -Force | Select-Object -First 1)
}

$premakeDir = Join-Path $root 'work\native_restore\premake'
$premake = Join-Path $premakeDir 'premake5.exe'
if (-not (Test-Path -LiteralPath $premake)) {
    New-Item -ItemType Directory -Force $premakeDir | Out-Null
    Invoke-WebRequest 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-windows.zip' -OutFile "$premakeDir\premake.zip"
    Expand-Archive -LiteralPath "$premakeDir\premake.zip" -DestinationPath $premakeDir -Force
}
if ((Get-FileHash -LiteralPath $premake -Algorithm SHA256).Hash -ne '2301e3e23ff3074cb83a5ea6103d68c7ea81dad56b786807c84b0643cddea31b') {
    throw 'Premake checksum mismatch.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\amd64\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 C++ build tools (64-bit MSBuild) not found.' }

# T4: a patched upstream clone (ignored by git, so it is lost with the folder)
$t4 = Join-Path $root 'vendor\OpenAssetTools'
if (-not (Test-Path -LiteralPath "$t4\.git")) {
    if (-not (Test-Empty $t4)) { throw "$t4 has files but is not a git clone; move it aside first." }
    if (Test-Path -LiteralPath $t4) { Remove-Item -LiteralPath $t4 }
    Invoke-Checked git @('clone', 'https://github.com/Laupetin/OpenAssetTools.git', 'vendor/OpenAssetTools')
    Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'checkout', '7d027e8f89118196713e955b0e11f8404149c54d')
    Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'submodule', 'update', '--init', '--recursive')
    Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'apply', '--check', '../OpenAssetTools.patch')
    Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'apply', '../OpenAssetTools.patch')
}

# T6: tracked source; only its ignored thirdparty folder comes from upstream
$t6 = Join-Path $root 'vendor\OpenAssetToolsT6'
if (Test-Empty "$t6\thirdparty\json") {
    $deps = Join-Path $root 'work\oat_t6_dependencies'
    if (-not (Test-Path -LiteralPath "$deps\.git")) {
        Invoke-Checked git @('clone', 'https://github.com/Laupetin/OpenAssetTools.git', 'work/oat_t6_dependencies')
        Invoke-Checked git @('-C', 'work/oat_t6_dependencies', 'checkout', '95b8c68fbc464109c9289a833035763505ea57da')
        Invoke-Checked git @('-C', 'work/oat_t6_dependencies', 'submodule', 'update', '--init', '--recursive')
    }
    New-Item -ItemType Directory -Force "$t6\thirdparty" | Out-Null
    Get-ChildItem "$deps\thirdparty" | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination "$t6\thirdparty" -Recurse -Force
    }
}

$tools = @(
    @{ Folder = $t4; Premake = @('vs2022'); Targets = '/t:Tools\UnlinkerCli'; Exes = @('Unlinker.exe') },
    @{ Folder = $t6; Premake = @('--oat-version=waw2bo2-v0.1.0', 'vs2022'); Targets = '/t:Tools\Linker;Tools\Unlinker'; Exes = @('Linker.exe', 'Unlinker.exe') }
)
foreach ($tool in $tools) {
    $bin = Join-Path $tool.Folder 'build\bin\Release_x86'
    if (-not $Rebuild -and -not ($tool.Exes | Where-Object { -not (Test-Path -LiteralPath (Join-Path $bin $_)) })) {
        Write-Output "up to date: $bin"
        continue
    }
    Push-Location $tool.Folder
    try {
        if (-not (Test-Path -LiteralPath 'build\OpenAssetTools.sln')) { Invoke-Checked $premake $tool.Premake }
        Invoke-Checked $msbuild @('build\OpenAssetTools.sln', "/m:$Projects", '/nodeReuse:false', '/v:minimal',
            '/p:Configuration=Release', '/p:Platform=Win32', '/p:PreferredToolArchitecture=x64',
            "/p:CL_MPCount=$Compilers", $tool.Targets)
    } finally { Pop-Location }
    foreach ($exe in $tool.Exes) {
        if (-not (Test-Path -LiteralPath (Join-Path $bin $exe))) { throw "missing after build: $bin\$exe" }
    }
    Write-Output "built: $bin"
}
& "$PSScriptRoot/build_audio_decoder.ps1"
