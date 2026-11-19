<#
Build the static map compatibility catalog for GitHub Pages:

    pwsh tools/build_map_compatibility_site.ps1 [-Output _site] [-Offline]

The map list comes from CodRepo's World at War category (the checked-in
catalog/codrepo-maps.json snapshot when -Offline or when CodRepo is down),
the statuses from catalog/compatibility.json. With GITHUB_TOKEN and
GITHUB_REPOSITORY set, Discord-submitted reports (issues labelled
map-compatibility-report) are listed beside them; they never change a
verified status.
#>
param(
    [string]$Output,
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $Root '_site' }
$Catalog = Join-Path $Root 'catalog/codrepo-maps.json'
$Compatibility = Join-Path $Root 'catalog/compatibility.json'
$Api = 'https://callofdutyrepo.com/wp-json/wp/v2'
$Source = 'https://callofdutyrepo.com/wawmaps/'
$UserAgent = 'B2W-map-compatibility-catalog/1.0 (+https://github.com/Ticass/B2W)'
$ValidStatuses = @('unknown', 'passed', 'failed')
$ReportMarkerPrefix = '<!-- b2w-map-compatibility:v1:'
$Utf8 = [Text.UTF8Encoding]::new($false)
$PageTemplate = @'
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#101311"><meta name="description" content="Community compatibility records for World at War map conversion to Black Ops II.">
  <title>Map Compatibility · B2W</title>
  <style>
    :root{color-scheme:dark;--bg:#101311;--panel:#171c19;--panel2:#1d2420;--line:#303a33;--ink:#ecf0eb;--muted:#9aa69c;--green:#8bdb9a;--lime:#c1ef80;--amber:#e6c77a;--red:#ee8c83;--mono:"Cascadia Code","SFMono-Regular",Consolas,monospace;--sans:Inter,"Segoe UI",system-ui,sans-serif}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 75% -10%,#27352b 0,transparent 45%),var(--bg);color:var(--ink);font:15px/1.55 var(--sans);min-height:100vh}a{color:inherit}.shell{width:min(1180px,calc(100% - 40px));margin:auto}.topline{height:58px;border-bottom:1px solid #ffffff12;display:flex;align-items:center;justify-content:space-between;color:var(--muted);font:11px var(--mono);letter-spacing:.09em;text-transform:uppercase}.brand{display:flex;align-items:center;gap:11px;color:var(--ink);font-weight:700}.brand-mark{width:25px;height:25px;display:grid;place-items:center;border:1px solid #8bdb9a66;border-radius:7px;color:var(--green);font-size:13px}.hero{padding:72px 0 43px;max-width:800px}.eyebrow{color:var(--green);font:11px var(--mono);text-transform:uppercase;letter-spacing:.16em}.hero h1{font-size:clamp(38px,7vw,68px);line-height:1.02;letter-spacing:-.055em;margin:15px 0 17px;font-weight:650}.hero h1 span{color:var(--green)}.hero p{margin:0;color:#b3bdb5;font-size:17px;max-width:680px}.source-note{font:12px var(--mono);color:var(--muted);margin-top:24px}.source-note a{color:var(--green);text-underline-offset:3px}.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:0 0 27px}.stat{background:#171c19c9;border:1px solid var(--line);border-radius:10px;padding:16px 18px}.stat strong{display:block;font:23px var(--mono);color:var(--green);letter-spacing:-.05em}.stat span{display:block;color:var(--muted);font-size:12px;margin-top:2px}.toolbar{position:sticky;top:0;z-index:2;padding:14px 0;background:#101311eF;backdrop-filter:blur(12px);border-block:1px solid #ffffff12;display:flex;gap:10px;align-items:center}.search{flex:1;min-width:180px;position:relative}.search input,.toolbar select{height:42px;border:1px solid var(--line);border-radius:8px;background:var(--panel);color:var(--ink);font:13px var(--sans);outline:none}.search input{width:100%;padding:0 14px 0 39px}.search input:focus,.toolbar select:focus{border-color:#8bdb9a99;box-shadow:0 0 0 3px #8bdb9a14}.search:before{content:"⌕";position:absolute;left:14px;top:7px;color:var(--muted);font-size:22px}.toolbar select{padding:0 30px 0 12px}.result-count{color:var(--muted);font:11px var(--mono);white-space:nowrap;margin-left:auto}.section-head{display:flex;justify-content:space-between;align-items:end;padding:29px 0 14px}.section-head h2{margin:0;font-size:17px;letter-spacing:-.02em}.section-head span{color:var(--muted);font:11px var(--mono)}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;padding-bottom:60px}.map-card{background:linear-gradient(145deg,#1a201c,#151a17);border:1px solid var(--line);border-radius:10px;padding:15px;min-height:190px;display:flex;flex-direction:column;transition:border-color .16s,transform .16s}.map-card:hover{border-color:#8bdb9a66;transform:translateY(-2px)}.card-top{display:flex;justify-content:space-between;align-items:center}.map-id{font:10px var(--mono);color:#77847a;letter-spacing:.08em}.source-link{text-decoration:none;color:#859287;width:23px;height:23px;display:grid;place-items:center;border:1px solid var(--line);border-radius:6px}.source-link:hover{color:var(--green);border-color:#8bdb9a66}.map-card h2{font-size:16px;line-height:1.3;letter-spacing:-.02em;margin:9px 0 12px;font-weight:600}.status-row{display:flex;gap:6px;flex-wrap:wrap}.status{border:1px solid var(--line);padding:4px 7px;border-radius:5px;font:10px var(--mono);color:#bac4bc}.status i{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:5px;background:#707b72}.status.passed{color:var(--green);border-color:#8bdb9a3b;background:#8bdb9a0c}.status.passed i{background:var(--green);box-shadow:0 0 8px #8bdb9a66}.status.failed{color:var(--red);border-color:#ee8c833b;background:#ee8c830c}.status.failed i{background:var(--red)}.evidence{color:var(--muted);font-size:12px;line-height:1.45;margin:12px 0 16px;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}.card-foot{margin-top:auto;padding-top:9px;border-top:1px solid #ffffff0c;display:flex;justify-content:space-between;gap:8px;color:#7e8a80;font:9px var(--mono)}.empty{grid-column:1/-1;text-align:center;color:var(--muted);padding:58px 20px;border:1px dashed var(--line);border-radius:10px}.footer{border-top:1px solid #ffffff12;padding:22px 0 35px;display:flex;justify-content:space-between;gap:20px;color:#7e8a80;font-size:11px}.footer a{color:#aeb9b0;text-underline-offset:3px}.footer .mono{font:10px var(--mono)}
    .community-counts{display:flex;gap:8px;flex-wrap:wrap;color:#b8c7bb;font:9px var(--mono);margin:0 0 8px}.community{border-top:1px solid #ffffff12;padding-top:8px;color:var(--muted);font-size:11px}.community summary{cursor:pointer;color:var(--green);font:10px var(--mono)}.community p{margin:7px 0}.community ul{padding-left:17px}.community li{margin:10px 0;overflow-wrap:anywhere}.community a{color:#b6d8bc;text-underline-offset:2px}.community-empty{color:#758177;font-size:10px;margin:0 0 8px}.map-card[hidden]{display:none}@media(max-width:900px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:600px){.shell{width:min(100% - 26px,1180px)}.hero{padding:52px 0 30px}.hero p{font-size:15px}.stats{gap:7px}.stat{padding:12px 10px}.stat strong{font-size:19px}.stat span{font-size:10px}.toolbar{flex-wrap:wrap}.search{flex-basis:100%}.toolbar select{flex:1;min-width:0}.result-count{margin:0 0 0 auto}.grid{grid-template-columns:1fr}.footer{flex-direction:column}.topline{height:50px}}
    @media(max-width:600px){.stats{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media(prefers-reduced-motion:reduce){*,*:before,*:after{scroll-behavior:auto!important;transition:none!important}}
  </style>
</head>
<body>
  <header class="shell topline"><div class="brand"><span class="brand-mark">B</span> B2W <span style="color:#718074">/</span> COMPATIBILITY INDEX</div><span>COMMUNITY TEST RECORDS</span></header>
  <main class="shell">
    <section class="hero"><div class="eyebrow">World at War → Black Ops II</div><h1>Maps, <span>verified.</span></h1><p>Compatibility notes from real conversions and in-game checks. Every map starts untested; statuses change only when a test report is recorded.</p><div class="source-note">BASE LIST · <a href="https://callofdutyrepo.com/wawmaps/" target="_blank" rel="noopener noreferrer">CODREPO WORLD AT WAR MAPS ↗</a></div></section>
    <section class="stats" aria-label="Catalog summary"><div class="stat"><strong>{{MAP_COUNT}}</strong><span>maps in the CodRepo index</span></div><div class="stat"><strong>{{COMPILE_COUNT}}</strong><span>confirmed compile successfully</span></div><div class="stat"><strong>{{PLAY_COUNT}}</strong><span>confirmed playable in game</span></div><div class="stat"><strong>{{REPORT_COUNT}}</strong><span>unverified community reports</span></div></section>
    <section aria-label="Map catalog"><div class="toolbar"><label class="search"><input id="search" type="search" placeholder="Search maps…" autocomplete="off" aria-label="Search maps"></label><select id="compile" aria-label="Filter by compile status"><option value="all">Any compile status</option><option value="passed">Compiles</option><option value="failed">Compile failed</option><option value="unknown">Untested</option></select><select id="play" aria-label="Filter by game status"><option value="all">Any game status</option><option value="passed">Played successfully</option><option value="failed">Play failed</option><option value="unknown">Not play-tested</option></select><span class="result-count" id="count" aria-live="polite"></span></div>
      <div class="section-head"><h2>Map records</h2><span>STATUS REQUIRES EVIDENCE</span></div><div class="grid" id="grid">{{CARDS}}</div>
    </section>
  </main>
  <footer class="shell footer"><span>Map names and links are sourced from <a href="https://callofdutyrepo.com/wawmaps/" target="_blank" rel="noopener noreferrer">Call of Duty Repo</a>. Compatibility notes are maintained by the B2W community.</span><span class="mono">GENERATED {{GENERATED}} · SOURCE INDEX UPDATED {{FETCHED}}</span></footer>
  <script src="./map-catalog.js"></script>
</body>
</html>
'@
$PageTemplate += "`n"
$Invariant = [Globalization.CultureInfo]::InvariantCulture

function Read-Json([string]$Path) { [IO.File]::ReadAllText($Path, $Utf8) | ConvertFrom-Json }
function Write-Text([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $Utf8) }

# a field of a parsed JSON object, $Default when absent
function Get-Field($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    if ($Object -is [Collections.IDictionary]) { if ($Object.Contains($Name)) { return $Object[$Name] } else { return $Default } }
    $property = $Object.PSObject.Properties[$Name]
    if ($property) { $property.Value } else { $Default }
}

# JSON as Python's json.dumps(ensure_ascii=False) writes it: compact, or indented
function ConvertTo-PyJson($Value, [int]$Indent = -1, [int]$Level = 0) {
    $newline = if ($Indent -ge 0) { "`n" + (' ' * ($Indent * ($Level + 1))) } else { '' }
    $close = if ($Indent -ge 0) { "`n" + (' ' * ($Indent * $Level)) } else { '' }
    $keySeparator = if ($Indent -ge 0) { ': ' } else { ':' }
    if ($null -eq $Value) { return 'null' }
    if ($Value -is [bool]) { return $(if ($Value) { 'true' } else { 'false' }) }
    if ($Value -is [string]) {
        $builder = [Text.StringBuilder]::new('"')
        foreach ($c in $Value.ToCharArray()) {
            switch ([int]$c) {
                0x22 { [void]$builder.Append('\"') } 0x5C { [void]$builder.Append('\\') }
                0x0A { [void]$builder.Append('\n') } 0x0D { [void]$builder.Append('\r') } 0x09 { [void]$builder.Append('\t') }
                0x08 { [void]$builder.Append('\b') } 0x0C { [void]$builder.Append('\f') }
                default { if ([int]$c -lt 0x20) { [void]$builder.AppendFormat('\u{0:x4}', [int]$c) } else { [void]$builder.Append($c) } }
            }
        }
        return $builder.Append('"').ToString()
    }
    if ($Value -is [int] -or $Value -is [long] -or $Value -is [decimal] -or $Value -is [double]) { return ([IFormattable]$Value).ToString($null, $Invariant) }
    if ($Value -is [Collections.IDictionary] -or $Value -is [Management.Automation.PSCustomObject]) {
        $pairs = if ($Value -is [Collections.IDictionary]) { foreach ($k in $Value.Keys) { , @($k, $Value[$k]) } } else { foreach ($p in $Value.PSObject.Properties) { , @($p.Name, $p.Value) } }
        $items = @(foreach ($pair in $pairs) { (ConvertTo-PyJson ([string]$pair[0])) + $keySeparator + (ConvertTo-PyJson $pair[1] $Indent ($Level + 1)) })
        if (-not $items) { return '{}' }
        return '{' + $newline + ($items -join (',' + $newline)) + $close + '}'
    }
    if ($Value -is [Collections.IEnumerable]) {
        $items = @(foreach ($item in $Value) { ConvertTo-PyJson $item $Indent ($Level + 1) })
        if (-not $items) { return '[]' }
        return '[' + $newline + ($items -join (',' + $newline)) + $close + ']'
    }
    throw "cannot write $($Value.GetType()) as JSON"
}

# Python's html.escape(quote=True)
function Escape-Html($Value, [string]$Default = '') {
    $s = if ($null -ne $Value) { [string]$Value } else { $Default }
    $s.Replace('&', '&amp;').Replace('<', '&lt;').Replace('>', '&gt;').Replace('"', '&quot;').Replace("'", '&#x27;')
}

function Title-Case([string]$Word) { if ($Word) { $Word.Substring(0, 1).ToUpperInvariant() + $Word.Substring(1).ToLowerInvariant() } else { $Word } }
function Format-Count([int]$Count) { $Count.ToString('N0', $Invariant) }
function Get-SafeStatus($Entry, [string]$Key) { $v = Get-Field $Entry $Key 'unknown'; if ($ValidStatuses -contains $v) { $v } else { 'unknown' } }
function Get-SafeUrl([string]$Value) { if ($Value -and $Value.StartsWith('https://')) { $Value } else { $Source } }
function Get-UtcNow { [DateTime]::UtcNow }

function Invoke-Json([string]$Url, [hashtable]$Headers) {
    $response = Invoke-WebRequest -Uri $Url -Headers $Headers -UserAgent $UserAgent -UseBasicParsing -TimeoutSec 35
    $text = $Utf8.GetString($response.RawContentStream.ToArray())
    ($text | ConvertFrom-Json), $response.Headers
}

function Get-CodRepoMaps {
    $headers = @{ Accept = 'application/json'; Referer = $Source }
    $categories, $unused = Invoke-Json "$Api/categories?slug=wawmaps&per_page=100" $headers
    $categories = @($categories)
    if ($categories.Count -ne 1) { throw 'CodRepo did not return exactly one Waw Maps category' }
    $category = $categories[0].id
    $fields = 'id,modified,link,title,slug'
    $unused, $responseHeaders = Invoke-Json "$Api/posts?categories=$category&per_page=100&page=1&_fields=$fields" $headers
    $total = [int]@($responseHeaders['X-WP-Total'])[0]
    $pages = [int]@($responseHeaders['X-WP-TotalPages'])[0]
    if ($total -le 0 -or $pages -le 0) { throw 'CodRepo returned an empty map catalog' }
    $maps = [Collections.Generic.List[object]]::new()
    foreach ($page in 1..$pages) {
        $rows, $unused = Invoke-Json "$Api/posts?categories=$category&per_page=100&page=$page&_fields=$fields" $headers
        foreach ($row in @($rows)) {
            $title = ([string](Get-Field (Get-Field $row 'title') 'rendered' '')).Trim()
            $link = [string](Get-Field $row 'link' '')
            if (-not (Get-Field $row 'id') -or -not $title -or -not $link.StartsWith('https://callofdutyrepo.com/')) { throw "CodRepo returned an invalid map record on page $page" }
            $maps.Add([ordered]@{ id = [long]$row.id; title = $title; url = $link; slug = [string](Get-Field $row 'slug' ''); modified = [string](Get-Field $row 'modified' '') })
        }
    }
    $ids = [Collections.Generic.HashSet[long]]::new()
    foreach ($m in $maps) { [void]$ids.Add($m.id) }
    if ($ids.Count -ne $maps.Count -or $maps.Count -lt $total) { throw "CodRepo catalog was incomplete or duplicated ($($maps.Count) of $total)" }
    [ordered]@{ source = $Source; source_api = "$Api/posts?categories=$category"; fetched_at = (Get-UtcNow).ToString("yyyy-MM-dd'T'HH:mm:ss'+00:00'", $Invariant); maps = $maps }
}

function Get-Catalog {
    $cached = Read-Json $Catalog
    if ($Offline) { return $cached, $true }
    try {
        $fresh = Get-CodRepoMaps
        Write-Text $Catalog ((ConvertTo-PyJson $fresh 2) + "`n")
        return $fresh, $false
    }
    catch {
        [Console]::Error.WriteLine("CodRepo refresh failed; using the checked-in $(@($cached.maps).Count)-map snapshot: $_")
        return $cached, $true
    }
}

# Discord-submitted compatibility issues; these never alter verified statuses
function Get-CommunityReports {
    $token = $env:GITHUB_TOKEN
    $repository = $env:GITHUB_REPOSITORY
    if (-not $token -or -not $repository) { return }
    $headers = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'X-GitHub-Api-Version' = '2022-11-28' }
    $reports = [Collections.Generic.List[object]]::new()
    for ($page = 1; ; $page++) {
        $issues, $unused = Invoke-Json "https://api.github.com/repos/$repository/issues?state=all&labels=map-compatibility-report&per_page=100&page=$page" $headers
        $issues = @($issues)
        foreach ($issue in $issues) {
            if (Get-Field $issue 'pull_request') { continue }
            $lines = ([string](Get-Field $issue 'body' '')).Replace("`r`n", "`n").Split("`n")
            if (-not $lines[0].StartsWith($ReportMarkerPrefix) -or -not $lines[0].EndsWith(' -->')) { continue }
            $encoded = $lines[0].Substring($ReportMarkerPrefix.Length, $lines[0].Length - $ReportMarkerPrefix.Length - 4)
            try {
                $padded = $encoded.Replace('-', '+').Replace('_', '/') + ('=' * ((4 - $encoded.Length % 4) % 4))
                $report = $Utf8.GetString([Convert]::FromBase64String($padded)) | ConvertFrom-Json
                $report.map_id = [long]$report.map_id
            }
            catch { continue }
            if (@('playable', 'broken') -notcontains (Get-Field $report 'outcome') -or @('windows', 'linux') -notcontains (Get-Field $report 'platform') -or -not (Get-Field $report 'map_title')) { continue }
            foreach ($pair in @(@('issue_number', $issue.number), @('issue_url', $issue.html_url), @('issue_state', $issue.state), @('issue_updated_at', (Get-Field $issue 'updated_at' '')))) {
                $report | Add-Member -NotePropertyName $pair[0] -NotePropertyValue $pair[1] -Force
            }
            $reports.Add($report)
        }
        if ($issues.Count -lt 100) { break }
    }
    [Linq.Enumerable]::OrderByDescending($reports, [Func[object, string]] { param($r) [string](Get-Field $r 'submitted_at' '') }, [StringComparer]::Ordinal)
}

function Get-Cards($Maps, $Records, [hashtable]$ReportsByMap) {
    $cards = foreach ($item in $Maps) {
        $title = [Net.WebUtility]::HtmlDecode([string]$item.title)
        $record = Get-Field $Records ([string]$item.id) @{}
        $compileStatus = Get-SafeStatus $record 'compile'
        $playStatus = Get-SafeStatus $record 'play'
        $evidence = @(Get-Field $record 'evidence' @())
        $latest = if ($evidence.Count -and ($evidence[0] -is [Management.Automation.PSCustomObject] -or $evidence[0] -is [Collections.IDictionary])) { $evidence[0] } else { @{} }
        $testedOn = Escape-Html (Get-Field $latest 'date') 'No test recorded'
        $version = Escape-Html (Get-Field $latest 'converter_version') 'Version not recorded'
        $notes = Escape-Html (Get-Field $latest 'notes') 'No compatibility evidence has been submitted yet.'
        $community = @(if ($ReportsByMap.ContainsKey([long]$item.id)) { $ReportsByMap[[long]$item.id] })
        $counts = @{}
        foreach ($outcome in 'playable', 'broken') { $counts[$outcome] = @($community | Where-Object { $_.outcome -eq $outcome -and (Get-Field $_ 'issue_state') -ne 'closed' }).Count }
        $rows = foreach ($report in $community) {
            $video = [string](Get-Field $report 'video_url' '')
            $videoHtml = if ($video) { "<a href=`"$(Escape-Html (Get-SafeUrl $video))`" target=`"_blank`" rel=`"noopener noreferrer`">Gameplay video</a>" } else { 'No video' }
            $links = (@(foreach ($file in @(Get-Field $report 'attachments' @())) {
                        if ($file -is [Management.Automation.PSCustomObject]) { "<a href=`"$(Escape-Html (Get-SafeUrl ([string](Get-Field $file 'url' ''))))`" target=`"_blank`" rel=`"noopener noreferrer`">$(Escape-Html (Get-Field $file 'name' 'Evidence'))</a>" }
                    }) -join ' · ')
            if (-not $links) { $links = 'No files' }
            "<li><strong>$(Escape-Html (Title-Case $report.outcome))</strong> · $(Escape-Html (Title-Case ([string](Get-Field $report 'platform' '')))) · " +
            "WawConverter $(Escape-Html (Get-Field $report 'tool_version' 'unknown')) · $(Escape-Html (Get-Field $report 'submitted_at' ''))<br>" +
            "$(Escape-Html (Get-Field $report 'known_issues' 'No notes supplied.'))<br>$videoHtml · $links · " +
            "<a href=`"$(Escape-Html (Get-SafeUrl ([string](Get-Field $report 'discord_url' ''))))`" target=`"_blank`" rel=`"noopener noreferrer`">Discord report</a> · " +
            "<a href=`"$(Escape-Html (Get-SafeUrl ([string](Get-Field $report 'issue_url' ''))))`" target=`"_blank`" rel=`"noopener noreferrer`">GitHub issue #$([long]$report.issue_number)</a></li>"
        }
        $communityHtml = if ($community.Count) {
            '<details class="community"><summary>Community reports · ' + "$($counts.playable) playable / $($counts.broken) broken</summary>" +
            '<p>Unverified community evidence; it does not change the verified status above.</p><ul>' + ($rows -join '') + '</ul></details>'
        } else { '<p class="community-empty">No community reports yet.</p>' }
        "<article class=`"map-card`" data-name=`"$(Escape-Html $title.ToLowerInvariant())`" data-compile=`"$compileStatus`" data-play=`"$playStatus`">" +
        '<div class="card-top"><span class="map-id">MAP / ' + ([long]$item.id).ToString('D5', $Invariant) +
        "</span><a class=`"source-link`" href=`"$(Escape-Html (Get-SafeUrl ([string]$item.url)))`" " +
        'target="_blank" rel="noopener noreferrer" aria-label="Open this map on CodRepo">↗</a></div>' +
        "<h2>$(Escape-Html $title)</h2>" +
        '<div class="status-row">' +
        "<span class=`"status $compileStatus`"><i></i>Compile · $(Title-Case $compileStatus)</span>" +
        "<span class=`"status $playStatus`"><i></i>In game · $(Title-Case $playStatus)</span>" +
        '</div>' +
        "<p class=`"evidence`">$notes</p>" +
        "<div class=`"community-counts`"><span>Community playable · $($counts.playable)</span><span>Community broken · $($counts.broken)</span></div>" +
        $communityHtml +
        "<div class=`"card-foot`"><span>$testedOn</span><span>$version</span></div>" +
        '</article>'
    }
    $cards -join "`n"
}

$catalogData, $usedCache = Get-Catalog
$records = Get-Field (Read-Json $Compatibility) 'maps'
if ($records -isnot [Management.Automation.PSCustomObject]) { throw 'catalog/compatibility.json must contain a maps object' }
# by title, casefolded, in code point order (stable)
$maps = @([Linq.Enumerable]::OrderBy([object[]]@($catalogData.maps), [Func[object, string]] { param($m) ([string]$m.title).ToLowerInvariant().Replace('ß', 'ss') }, [StringComparer]::Ordinal))
$reports = @(Get-CommunityReports)
$reportsByMap = @{}
foreach ($report in $reports) {
    $id = [long]$report.map_id
    if (-not $reportsByMap.ContainsKey($id)) { $reportsByMap[$id] = [Collections.Generic.List[object]]::new() }
    $reportsByMap[$id].Add($report)
}
$cards = Get-Cards $maps $records $reportsByMap
$compilePassed = @($maps | Where-Object { (Get-SafeStatus (Get-Field $records ([string]$_.id) @{}) 'compile') -eq 'passed' }).Count
$playPassed = @($maps | Where-Object { (Get-SafeStatus (Get-Field $records ([string]$_.id) @{}) 'play') -eq 'passed' }).Count
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$page = $PageTemplate
$values = [ordered]@{
    '{{GENERATED}}'     = (Get-UtcNow).ToString('yyyy-MM-dd HH:mm', $Invariant) + ' UTC'
    '{{FETCHED}}'       = Escape-Html (Get-Field $catalogData 'fetched_at' 'Unknown')
    '{{MAP_COUNT}}'     = Format-Count $maps.Count
    '{{COMPILE_COUNT}}' = Format-Count $compilePassed
    '{{PLAY_COUNT}}'    = Format-Count $playPassed
    '{{REPORT_COUNT}}'  = Format-Count $reports.Count
    '{{CARDS}}'         = $cards
}
foreach ($placeholder in $values.Keys) { $page = $page.Replace($placeholder, $values[$placeholder]) }
Write-Text (Join-Path $Output 'index.html') $page
Copy-Item -LiteralPath (Join-Path $Root 'catalog/map-catalog.js') -Destination (Join-Path $Output 'map-catalog.js') -Force
Write-Text (Join-Path $Output '.nojekyll') ''
$export = [ordered]@{}
foreach ($property in $catalogData.PSObject.Properties) { if ($catalogData -is [Collections.IDictionary]) { break }; $export[$property.Name] = $property.Value }
if ($catalogData -is [Collections.IDictionary]) { foreach ($key in $catalogData.Keys) { $export[$key] = $catalogData[$key] } }
$export['community_reports'] = $reports
Write-Text (Join-Path $Output 'maps.json') ((ConvertTo-PyJson $export) + "`n")
Write-Text (Join-Path $Output 'robots.txt') "User-agent: *`nAllow: /`n"
"Generated $Output with $($maps.Count) maps" + $(if ($usedCache) { ' from cached snapshot' } else { ' from CodRepo' })
if ($env:GITHUB_STEP_SUMMARY) {
    $snapshot = if ($usedCache) { 'cached' } else { 'refreshed' }
    [IO.File]::AppendAllText($env:GITHUB_STEP_SUMMARY, "## Map compatibility Pages`n`n- Maps: **$(Format-Count $maps.Count)**`n- CodRepo snapshot: **$snapshot**`n", $Utf8)
}
