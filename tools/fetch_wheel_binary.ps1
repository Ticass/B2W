<#
Fetch one prebuilt executable from a pinned PyPI wheel, without Python: a
wheel is a zip archive, and PyPI publishes each file's SHA-256.

    powershell -File tools/fetch_wheel_binary.ps1 -Package clang-tidy -Version 22.1.7 -Member '*/bin/clang-tidy.exe' -Destination work/tools/clang-tidy.exe
    powershell -File tools/fetch_wheel_binary.ps1 -Package imageio-ffmpeg -Version 0.6.0 -Member 'imageio_ffmpeg/binaries/ffmpeg-*.exe' -Destination tools/bin/ffmpeg.exe

-Member is a wildcard matched against the archive's entry names; exactly one
entry must match. Uses the win_amd64 wheel.
#>
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$Member,
    [Parameter(Mandatory)][string]$Destination,
    [string]$Platform = 'win_amd64'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$target = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)

$release = Invoke-RestMethod -UseBasicParsing "https://pypi.org/pypi/$Package/$Version/json"
$wheel = @($release.urls | Where-Object { $_.packagetype -eq 'bdist_wheel' -and $_.filename -like "*$Platform*.whl" }) | Select-Object -First 1
if (-not $wheel) { throw "$Package $Version has no $Platform wheel" }
$download = Join-Path ([IO.Path]::GetTempPath()) $wheel.filename
Invoke-WebRequest -UseBasicParsing -Uri $wheel.url -OutFile $download
try {
    $digest = (Get-FileHash -Algorithm SHA256 -LiteralPath $download).Hash.ToLowerInvariant()
    if ($digest -ne $wheel.digests.sha256) { throw "$($wheel.filename): SHA-256 $digest, PyPI lists $($wheel.digests.sha256)" }
    $archive = [IO.Compression.ZipFile]::OpenRead($download)
    try {
        $entries = @($archive.Entries | Where-Object { $_.FullName -like $Member })
        if ($entries.Count -ne 1) { throw "$($wheel.filename): $($entries.Count) entries match $Member" }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent ($target)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[0], $target, $true)
    }
    finally { $archive.Dispose() }
}
finally { Remove-Item -LiteralPath $download -Force }
"$Destination <- $($wheel.filename) ($($entries[0].FullName))"
