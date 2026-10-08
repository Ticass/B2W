# Build native tools from the pinned sources documented in docs/USAGE.md.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path $PSScriptRoot -Parent
Set-Location $nativeRoot

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

# Fresh CI checkouts only: never overwrite a developer's patched upstream tree.
foreach ($nativePath in @('vendor/OpenAssetTools', 'work/oat_t6_dependencies')) {
    if (Test-Path -LiteralPath $nativePath) { throw "Expected a fresh checkout; $nativePath already exists." }
}
New-Item -ItemType Directory -Force work/ci_premake | Out-Null
Invoke-WebRequest 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-windows.zip' -OutFile work/ci_premake/premake.zip
Expand-Archive -LiteralPath work/ci_premake/premake.zip -DestinationPath work/ci_premake -Force
$nativePremake = Join-Path $nativeRoot 'work/ci_premake/premake5.exe'
# Hash from the pinned T4 generate.bat, for the executable inside the archive.
if ((Get-FileHash -LiteralPath $nativePremake -Algorithm SHA256).Hash -ne '2301e3e23ff3074cb83a5ea6103d68c7ea81dad56b786807c84b0643cddea31b') {
    throw 'Premake checksum mismatch.'
}
Invoke-Checked git @('clone', 'https://github.com/Laupetin/OpenAssetTools.git', 'vendor/OpenAssetTools')
Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'checkout', '7d027e8f89118196713e955b0e11f8404149c54d')
Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'submodule', 'update', '--init', '--recursive')
Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'apply', '--check', '../OpenAssetTools.patch')
Invoke-Checked git @('-C', 'vendor/OpenAssetTools', 'apply', '../OpenAssetTools.patch')

Invoke-Checked git @('clone', 'https://github.com/Laupetin/OpenAssetTools.git', 'work/oat_t6_dependencies')
Invoke-Checked git @('-C', 'work/oat_t6_dependencies', 'checkout', '95b8c68fbc464109c9289a833035763505ea57da')
Invoke-Checked git @('-C', 'work/oat_t6_dependencies', 'submodule', 'update', '--init', '--recursive')
New-Item -ItemType Directory -Force vendor/OpenAssetToolsT6/thirdparty | Out-Null
# The ignored thirdparty directory also contains Premake .lua definitions.
Get-ChildItem work/oat_t6_dependencies/thirdparty | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination vendor/OpenAssetToolsT6/thirdparty -Recurse -Force
}

foreach ($nativeFolder in @('OpenAssetTools', 'OpenAssetToolsT6')) {
    Push-Location (Join-Path $nativeRoot "vendor/$nativeFolder")
    try {
        Invoke-Checked $nativePremake @('vs2022')
        # Premake maps its x86 platform to Visual Studio's Win32 platform.
        # Build only packaged tools and their dependencies, with bounded parallelism
        # and the 64-bit compiler host on GitHub's Windows runner.
        $nativeTargets = if ($nativeFolder -eq 'OpenAssetTools') {
            '/t:Tools\UnlinkerCli'
        } else {
            '/t:Tools\Linker;Tools\Unlinker'
        }
        Invoke-Checked MSBuild.exe @('build/OpenAssetTools.sln', '/m:2', '/nodeReuse:false',
            '/p:Configuration=Release', '/p:Platform=Win32', '/p:PreferredToolArchitecture=x64',
            '/p:CL_MPCount=2', $nativeTargets)
    } finally { Pop-Location }
}
& "$PSScriptRoot/build_audio_decoder.ps1" -Force
