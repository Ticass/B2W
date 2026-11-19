<#
Regenerate OAT *.template sources into a build tree (MSBuild's own template
step fails here):

    powershell -File tools/retemplate_oat.ps1 <oat repo> <src-relative template>...

For each template: runs build/buildtools/Release_x86/RawTemplater.exe into a
temporary folder, copies every generated file to build/src/<Project>/<same
path>, and writes the step's .log stamp so MSBuild does not rerun it.
#>
param(
    [Parameter(Mandatory, Position = 0)][string]$Repository,
    [Parameter(Mandatory, Position = 1, ValueFromRemainingArguments)][string[]]$Templates
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath $Repository).Path
$templater = Join-Path $repo 'build\buildtools\Release_x86\RawTemplater.exe'
foreach ($relative in $Templates) {
    $relative = $relative.Replace('/', '\')
    $template = Join-Path $repo "src\$relative"
    $project = $relative.Split('\')[0]
    $buildDir = Join-Path $repo "build\src\$project"
    $temporary = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
    New-Item -ItemType Directory -Path $temporary | Out-Null
    try {
        Push-Location $buildDir
        try { $log = & $templater -o $temporary $template 2>&1 } finally { Pop-Location }
        $generated = @(Get-ChildItem -LiteralPath $temporary -Recurse -File)
        if ($LASTEXITCODE -or -not $generated) {
            $log | Out-Host
            throw "templating failed: $template"
        }
        foreach ($file in $generated) {
            $destination = Join-Path $buildDir $file.FullName.Substring($temporary.Length + 1)
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
            "$relative -> $($destination.Substring($repo.Length + 1))"
        }
    }
    finally {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
    # the custom build step also declares <template minus .template>.log
    $inProject = $relative.Substring($project.Length + 1)
    $stamp = Join-Path $buildDir ([IO.Path]::ChangeExtension($inProject, '.log'))
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $stamp) | Out-Null
    [IO.File]::WriteAllText($stamp, "templated by tools/retemplate_oat.ps1`n")
}
