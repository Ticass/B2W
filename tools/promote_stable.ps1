<#
Turn a nightly snapshot into the next stable version (the Saturday stable
promotion workflow):

    pwsh tools/promote_stable.ps1 next-version v0.2.14 v0.2.9 ...
    pwsh tools/promote_stable.ps1 release VERSION [-FallbackNotes FILE]
    pwsh tools/promote_stable.ps1 released-entries VERSION CHANGELOG
    pwsh tools/promote_stable.ps1 sync-main VERSION RELEASED_CHANGELOG

"release" runs in the nightly's checkout: it sets VERSION and turns the
"Unreleased" changelog section into the version's section; when that section
is missing or empty, the lines of -FallbackNotes (the commit subjects since
the previous stable) are used instead. "sync-main" runs in a main checkout: it
sets VERSION and moves only the entries the release shipped under the version
heading, keeping later work in "Unreleased".
#>
param(
    [Parameter(Mandatory, Position = 0)][ValidateSet('next-version', 'release', 'released-entries', 'sync-main')][string]$Command,
    [Parameter(Position = 1, ValueFromRemainingArguments)][string[]]$Arguments,
    [string]$FallbackNotes,
    [string]$Root
)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$Unreleased = '## Unreleased'
$Utf8 = [Text.UTF8Encoding]::new($false)

function Read-Text([string]$Path) { [IO.File]::ReadAllText($Path, $Utf8).Replace("`r`n", "`n") }
function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $Utf8) }
function Get-Lines([string]$Text) { if ($Text) { , $Text.Split("`n") } else { , @() } }

function Get-NextVersion([string[]]$Tags) {
    $versions = foreach ($tag in $Tags) {
        if ($tag -match '^v(\d+)\.(\d+)\.(\d+)$') { , @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3]) }
    }
    if (-not $versions) { throw 'No vMAJOR.MINOR.PATCH tag exists to increment' }
    $latest = $versions | Sort-Object { $_[0] }, { $_[1] }, { $_[2] } | Select-Object -Last 1
    "{0}.{1}.{2}" -f $latest[0], $latest[1], ($latest[2] + 1)
}

function Set-Version([string]$Version) { Write-Text (Join-Path $Root 'VERSION') "$Version`n" }

function Format-Section([string]$Heading, [string[]]$Lines) {
    $body = ($Lines -join "`n").Trim("`n")
    if ($body) { "$Heading`n`n$body`n`n" } else { "$Heading`n`n" }
}

# (text before the Unreleased section, its body lines, text after it)
function Split-Unreleased([string]$Text) {
    $start = $Text.IndexOf("$Unreleased`n")
    if ($start -lt 0) { throw "CHANGELOG.md has no `"$Unreleased`" section" }
    $bodyStart = $start + $Unreleased.Length + 1
    $following = [regex]::Match($Text.Substring($bodyStart), '(?m)^## ')
    $bodyEnd = if ($following.Success) { $bodyStart + $following.Index } else { $Text.Length }
    $Text.Substring(0, $start), (Get-Lines $Text.Substring($bodyStart, $bodyEnd - $bodyStart).Trim("`n")), $Text.Substring($bodyEnd)
}

function Get-ReleasedEntries([string]$Text, [string]$Version) {
    $heading = "## v$Version`n"
    $start = $Text.IndexOf($heading)
    if ($start -lt 0) { throw "Released changelog has no `"## v$Version`" section" }
    $body = $Text.Substring($start + $heading.Length)
    $following = [regex]::Match($body, '(?m)^## ')
    Get-Lines $(if ($following.Success) { $body.Substring(0, $following.Index) } else { $body }).Trim("`n")
}

function Get-ReleaseChangelog([string]$Text, [string]$Version, [string[]]$Fallback) {
    if (-not $Text.Contains("$Unreleased`n")) {
        $first = [regex]::Match($Text, '(?m)^## ')
        $at = if ($first.Success) { $first.Index } else { $Text.Length }
        $Text = $Text.Substring(0, $at).TrimEnd("`n") + "`n`n" + (Format-Section $Unreleased @()) + $Text.Substring($at)
    }
    $before, $lines, $after = Split-Unreleased $Text
    if (-not ($lines | Where-Object { $_.Trim() })) {
        $lines = @($Fallback | Where-Object { $_ -and $_.Trim() } | ForEach-Object { "- $($_.Trim())" })
        if (-not $lines) { $lines = @('- Maintenance release; no user-facing changes were recorded.') }
    }
    $before + (Format-Section $Unreleased @()) + (Format-Section "## v$Version" $lines) + $after
}

function Get-SyncedChangelog([string]$Text, [string]$Version, [string[]]$Shipped) {
    if ($Text.Contains("## v$Version`n")) { return $Text }
    $before, $lines, $after = Split-Unreleased $Text
    $shippedSet = [Collections.Generic.HashSet[string]]::new([string[]]@($Shipped | Where-Object { $_.Trim() }))
    $pending = @($lines | Where-Object { -not $shippedSet.Contains($_) })
    $before + (Format-Section $Unreleased $pending) + (Format-Section "## v$Version" $Shipped) + $after
}

switch ($Command) {
    'next-version' { Get-NextVersion $Arguments }
    'released-entries' { (Get-ReleasedEntries (Read-Text $Arguments[1]) $Arguments[0]) -join "`n" }
    'release' {
        $version = $Arguments[0]
        $changelog = Join-Path $Root 'CHANGELOG.md'
        $fallback = if ($FallbackNotes) { Get-Lines (Read-Text $FallbackNotes) } else { @() }
        Write-Text $changelog (Get-ReleaseChangelog (Read-Text $changelog) $version $fallback)
        Set-Version $version
    }
    'sync-main' {
        $version = $Arguments[0]
        $changelog = Join-Path $Root 'CHANGELOG.md'
        $shipped = Get-ReleasedEntries (Read-Text $Arguments[1]) $version
        Write-Text $changelog (Get-SyncedChangelog (Read-Text $changelog) $version $shipped)
        Set-Version $version
    }
}
