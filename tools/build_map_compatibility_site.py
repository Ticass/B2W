#!/usr/bin/env python3
"""Build the static map compatibility catalog for GitHub Pages."""

from __future__ import annotations

import argparse
import base64
import datetime as dt
import html
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / "catalog" / "codrepo-maps.json"
COMPATIBILITY = ROOT / "catalog" / "compatibility.json"
API = "https://callofdutyrepo.com/wp-json/wp/v2"
SOURCE = "https://callofdutyrepo.com/wawmaps/"
USER_AGENT = "B2W-map-compatibility-catalog/1.0 (+https://github.com/Ticass/B2W)"
VALID_STATUSES = {"unknown", "passed", "failed"}
REPORT_MARKER_PREFIX = "<!-- b2w-map-compatibility:v1:"


def get_json(url: str) -> tuple[Any, Any]:
    request = urllib.request.Request(
        url,
        headers={"User-Agent": USER_AGENT, "Accept": "application/json", "Referer": SOURCE},
    )
    with urllib.request.urlopen(request, timeout=35) as response:
        return json.load(response), response.headers


def fetch_codrepo_maps() -> dict[str, Any]:
    categories, _ = get_json(f"{API}/categories?slug=wawmaps&per_page=100")
    if not isinstance(categories, list) or len(categories) != 1:
        raise ValueError("CodRepo did not return exactly one Waw Maps category")
    category_id = categories[0]["id"]
    params = urllib.parse.urlencode(
        {"categories": category_id, "per_page": 100, "page": 1, "_fields": "id,modified,link,title,slug"}
    )
    _, headers = get_json(f"{API}/posts?{params}")
    total = int(headers.get("X-WP-Total", "0"))
    pages = int(headers.get("X-WP-TotalPages", "0"))
    if total <= 0 or pages <= 0:
        raise ValueError("CodRepo returned an empty map catalog")

    maps: list[dict[str, Any]] = []
    for page in range(1, pages + 1):
        params = urllib.parse.urlencode(
            {"categories": category_id, "per_page": 100, "page": page, "_fields": "id,modified,link,title,slug"}
        )
        rows, _ = get_json(f"{API}/posts?{params}")
        if not isinstance(rows, list):
            raise ValueError(f"CodRepo returned an invalid response on page {page}")
        for row in rows:
            title = row.get("title", {}).get("rendered", "").strip()
            link = row.get("link", "")
            if not row.get("id") or not title or not link.startswith("https://callofdutyrepo.com/"):
                raise ValueError(f"CodRepo returned an invalid map record on page {page}")
            maps.append(
                {
                    "id": int(row["id"]),
                    "title": title,
                    "url": link,
                    "slug": row.get("slug", ""),
                    "modified": row.get("modified", ""),
                }
            )
    ids = {item["id"] for item in maps}
    if len(ids) != len(maps) or len(maps) < total:
        raise ValueError(f"CodRepo catalog was incomplete or duplicated ({len(maps)} of {total})")
    return {
        "source": SOURCE,
        "source_api": f"{API}/posts?categories={category_id}",
        "fetched_at": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "maps": maps,
    }


def load_catalog(offline: bool) -> tuple[dict[str, Any], bool]:
    cached = json.loads(CATALOG.read_text(encoding="utf-8"))
    if offline:
        return cached, True
    try:
        fresh = fetch_codrepo_maps()
        CATALOG.write_text(json.dumps(fresh, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return fresh, False
    except (OSError, ValueError, KeyError, TypeError, urllib.error.URLError, TimeoutError) as error:
        print(f"CodRepo refresh failed; using the checked-in {len(cached['maps'])}-map snapshot: {error}", file=sys.stderr)
        return cached, True


def text(value: Any, default: str = "") -> str:
    return html.escape(str(value if value is not None else default), quote=True)


def safe_status(entry: dict[str, Any], key: str) -> str:
    value = entry.get(key, "unknown")
    return value if value in VALID_STATUSES else "unknown"


def safe_url(value: str) -> str:
    return value if value.startswith("https://") else SOURCE


def fetch_community_reports() -> list[dict[str, Any]]:
    """Read Discord-submitted compatibility issues; these never alter verified statuses."""
    token = os.environ.get("GITHUB_TOKEN")
    repository = os.environ.get("GITHUB_REPOSITORY")
    if not token or not repository:
        return []
    reports: list[dict[str, Any]] = []
    page = 1
    while True:
        query = urllib.parse.urlencode({"state": "all", "labels": "map-compatibility-report",
                                       "per_page": 100, "page": page})
        request = urllib.request.Request(
            f"https://api.github.com/repos/{repository}/issues?{query}",
            headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json",
                     "X-GitHub-Api-Version": "2022-11-28", "User-Agent": USER_AGENT},
        )
        with urllib.request.urlopen(request, timeout=35) as response:
            issues = json.load(response)
        for issue in issues:
            if "pull_request" in issue:
                continue
            lines = (issue.get("body") or "").splitlines()
            if not lines or not lines[0].startswith(REPORT_MARKER_PREFIX) or not lines[0].endswith(" -->"):
                continue
            encoded = lines[0][len(REPORT_MARKER_PREFIX):-4]
            try:
                report = json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))
                report["map_id"] = int(report["map_id"])
            except (ValueError, TypeError, KeyError, json.JSONDecodeError):
                continue
            if (report.get("outcome") not in {"playable", "broken"} or
                    report.get("platform") not in {"windows", "linux"} or
                    not report.get("map_title")):
                continue
            report["issue_number"] = issue["number"]
            report["issue_url"] = issue["html_url"]
            report["issue_state"] = issue["state"]
            report["issue_updated_at"] = issue.get("updated_at", "")
            reports.append(report)
        if len(issues) < 100:
            break
        page += 1
    reports.sort(key=lambda report: report.get("submitted_at", ""), reverse=True)
    return reports


def render_cards(maps: list[dict[str, Any]], records: dict[str, Any], reports_by_map: dict[int, list[dict[str, Any]]]) -> str:
    cards: list[str] = []
    for item in maps:
        title = html.unescape(str(item["title"]))
        record = records.get(str(item["id"]), {})
        compile_status = safe_status(record, "compile")
        play_status = safe_status(record, "play")
        evidence = record.get("evidence", [])
        latest = evidence[0] if evidence and isinstance(evidence[0], dict) else {}
        tested_on = text(latest.get("date"), "No test recorded")
        version = text(latest.get("converter_version"), "Version not recorded")
        notes = text(latest.get("notes"), "No compatibility evidence has been submitted yet.")
        community = reports_by_map.get(int(item["id"]), [])
        community_counts = {outcome: sum(r["outcome"] == outcome and r.get("issue_state") != "closed" for r in community)
                            for outcome in ("playable", "broken")}
        report_rows = []
        for report in community:
            video = report.get("video_url", "")
            video_html = f'<a href="{text(safe_url(video))}" target="_blank" rel="noopener noreferrer">Gameplay video</a>' if video else "No video"
            evidence_links = " · ".join(
                f'<a href="{text(safe_url(file.get("url", "")))}" target="_blank" rel="noopener noreferrer">{text(file.get("name", "Evidence"))}</a>'
                for file in report.get("attachments", []) if isinstance(file, dict)
            ) or "No files"
            report_rows.append(
                f'<li><strong>{text(report["outcome"].title())}</strong> · {text(report.get("platform", "").title())} · '
                f'WawConverter {text(report.get("tool_version", "unknown"))} · {text(report.get("submitted_at", ""))}<br>'
                f'{text(report.get("known_issues", "No notes supplied."))}<br>{video_html} · {evidence_links} · '
                f'<a href="{text(safe_url(report.get("discord_url", "")))}" target="_blank" rel="noopener noreferrer">Discord report</a> · '
                f'<a href="{text(safe_url(report.get("issue_url", "")))}" target="_blank" rel="noopener noreferrer">GitHub issue #{int(report["issue_number"])}</a></li>'
            )
        community_html = ('<details class="community"><summary>Community reports · '
            f'{community_counts["playable"]} playable / {community_counts["broken"]} broken</summary>'
            '<p>Unverified community evidence; it does not change the verified status above.</p><ul>'
            + "".join(report_rows) + "</ul></details>") if community else '<p class="community-empty">No community reports yet.</p>'
        cards.append(
            f'<article class="map-card" data-name="{text(title.lower())}" '
            f'data-compile="{compile_status}" data-play="{play_status}">'
            '<div class="card-top"><span class="map-id">MAP / '
            f'{int(item["id"]):05d}</span><a class="source-link" href="{text(safe_url(item["url"]))}" '
            'target="_blank" rel="noopener noreferrer" aria-label="Open this map on CodRepo">↗</a></div>'
            f'<h2>{text(title)}</h2>'
            '<div class="status-row">'
            f'<span class="status {compile_status}"><i></i>Compile · {compile_status.title()}</span>'
            f'<span class="status {play_status}"><i></i>In game · {play_status.title()}</span>'
            '</div>'
            f'<p class="evidence">{notes}</p>'
            f'<div class="community-counts"><span>Community playable · {community_counts["playable"]}</span><span>Community broken · {community_counts["broken"]}</span></div>'
            f'{community_html}'
            f'<div class="card-foot"><span>{tested_on}</span><span>{version}</span></div>'
            '</article>'
        )
    return "\n".join(cards)


def build_site(output: Path, offline: bool = False) -> tuple[int, bool]:
    catalog, used_cache = load_catalog(offline)
    compatibility = json.loads(COMPATIBILITY.read_text(encoding="utf-8"))
    records = compatibility.get("maps", {})
    if not isinstance(records, dict):
        raise ValueError("catalog/compatibility.json must contain a maps object")
    maps = sorted(catalog["maps"], key=lambda item: item["title"].casefold())
    reports = fetch_community_reports()
    reports_by_map: dict[int, list[dict[str, Any]]] = {}
    for report in reports:
        reports_by_map.setdefault(int(report["map_id"]), []).append(report)
    cards = render_cards(maps, records, reports_by_map)
    compile_passed = sum(safe_status(records.get(str(m["id"]), {}), "compile") == "passed" for m in maps)
    play_passed = sum(safe_status(records.get(str(m["id"]), {}), "play") == "passed" for m in maps)
    updated = text(catalog.get("fetched_at", "Unknown"))
    output.mkdir(parents=True, exist_ok=True)
    page = PAGE_TEMPLATE
    for placeholder, value in {
        "{{GENERATED}}": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "{{FETCHED}}": updated,
        "{{MAP_COUNT}}": f"{len(maps):,}",
        "{{COMPILE_COUNT}}": f"{compile_passed:,}",
        "{{PLAY_COUNT}}": f"{play_passed:,}",
        "{{REPORT_COUNT}}": f"{len(reports):,}",
        "{{CARDS}}": cards,
    }.items():
        page = page.replace(placeholder, value)
    (output / "index.html").write_text(page, encoding="utf-8")
    (output / ".nojekyll").write_text("", encoding="utf-8")
    export = {**catalog, "community_reports": reports}
    (output / "maps.json").write_text(json.dumps(export, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
    (output / "robots.txt").write_text("User-agent: *\nAllow: /\n", encoding="utf-8")
    return len(maps), used_cache


PAGE_TEMPLATE = r'''<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#101311"><meta name="description" content="Community compatibility records for World at War map conversion to Black Ops II.">
  <title>Map Compatibility · WawConverter</title>
  <style>
    :root{color-scheme:dark;--bg:#101311;--panel:#171c19;--panel2:#1d2420;--line:#303a33;--ink:#ecf0eb;--muted:#9aa69c;--green:#8bdb9a;--lime:#c1ef80;--amber:#e6c77a;--red:#ee8c83;--mono:"Cascadia Code","SFMono-Regular",Consolas,monospace;--sans:Inter,"Segoe UI",system-ui,sans-serif}
    *{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 75% -10%,#27352b 0,transparent 45%),var(--bg);color:var(--ink);font:15px/1.55 var(--sans);min-height:100vh}a{color:inherit}.shell{width:min(1180px,calc(100% - 40px));margin:auto}.topline{height:58px;border-bottom:1px solid #ffffff12;display:flex;align-items:center;justify-content:space-between;color:var(--muted);font:11px var(--mono);letter-spacing:.09em;text-transform:uppercase}.brand{display:flex;align-items:center;gap:11px;color:var(--ink);font-weight:700}.brand-mark{width:25px;height:25px;display:grid;place-items:center;border:1px solid #8bdb9a66;border-radius:7px;color:var(--green);font-size:13px}.hero{padding:72px 0 43px;max-width:800px}.eyebrow{color:var(--green);font:11px var(--mono);text-transform:uppercase;letter-spacing:.16em}.hero h1{font-size:clamp(38px,7vw,68px);line-height:1.02;letter-spacing:-.055em;margin:15px 0 17px;font-weight:650}.hero h1 span{color:var(--green)}.hero p{margin:0;color:#b3bdb5;font-size:17px;max-width:680px}.source-note{font:12px var(--mono);color:var(--muted);margin-top:24px}.source-note a{color:var(--green);text-underline-offset:3px}.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin:0 0 27px}.stat{background:#171c19c9;border:1px solid var(--line);border-radius:10px;padding:16px 18px}.stat strong{display:block;font:23px var(--mono);color:var(--green);letter-spacing:-.05em}.stat span{display:block;color:var(--muted);font-size:12px;margin-top:2px}.toolbar{position:sticky;top:0;z-index:2;padding:14px 0;background:#101311eF;backdrop-filter:blur(12px);border-block:1px solid #ffffff12;display:flex;gap:10px;align-items:center}.search{flex:1;min-width:180px;position:relative}.search input,.toolbar select{height:42px;border:1px solid var(--line);border-radius:8px;background:var(--panel);color:var(--ink);font:13px var(--sans);outline:none}.search input{width:100%;padding:0 14px 0 39px}.search input:focus,.toolbar select:focus{border-color:#8bdb9a99;box-shadow:0 0 0 3px #8bdb9a14}.search:before{content:"⌕";position:absolute;left:14px;top:7px;color:var(--muted);font-size:22px}.toolbar select{padding:0 30px 0 12px}.result-count{color:var(--muted);font:11px var(--mono);white-space:nowrap;margin-left:auto}.section-head{display:flex;justify-content:space-between;align-items:end;padding:29px 0 14px}.section-head h2{margin:0;font-size:17px;letter-spacing:-.02em}.section-head span{color:var(--muted);font:11px var(--mono)}.grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px;padding-bottom:60px}.map-card{background:linear-gradient(145deg,#1a201c,#151a17);border:1px solid var(--line);border-radius:10px;padding:15px;min-height:190px;display:flex;flex-direction:column;transition:border-color .16s,transform .16s}.map-card:hover{border-color:#8bdb9a66;transform:translateY(-2px)}.card-top{display:flex;justify-content:space-between;align-items:center}.map-id{font:10px var(--mono);color:#77847a;letter-spacing:.08em}.source-link{text-decoration:none;color:#859287;width:23px;height:23px;display:grid;place-items:center;border:1px solid var(--line);border-radius:6px}.source-link:hover{color:var(--green);border-color:#8bdb9a66}.map-card h2{font-size:16px;line-height:1.3;letter-spacing:-.02em;margin:9px 0 12px;font-weight:600}.status-row{display:flex;gap:6px;flex-wrap:wrap}.status{border:1px solid var(--line);padding:4px 7px;border-radius:5px;font:10px var(--mono);color:#bac4bc}.status i{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:5px;background:#707b72}.status.passed{color:var(--green);border-color:#8bdb9a3b;background:#8bdb9a0c}.status.passed i{background:var(--green);box-shadow:0 0 8px #8bdb9a66}.status.failed{color:var(--red);border-color:#ee8c833b;background:#ee8c830c}.status.failed i{background:var(--red)}.evidence{color:var(--muted);font-size:12px;line-height:1.45;margin:12px 0 16px;display:-webkit-box;-webkit-line-clamp:2;-webkit-box-orient:vertical;overflow:hidden}.card-foot{margin-top:auto;padding-top:9px;border-top:1px solid #ffffff0c;display:flex;justify-content:space-between;gap:8px;color:#7e8a80;font:9px var(--mono)}.empty{grid-column:1/-1;text-align:center;color:var(--muted);padding:58px 20px;border:1px dashed var(--line);border-radius:10px}.footer{border-top:1px solid #ffffff12;padding:22px 0 35px;display:flex;justify-content:space-between;gap:20px;color:#7e8a80;font-size:11px}.footer a{color:#aeb9b0;text-underline-offset:3px}.footer .mono{font:10px var(--mono)}
    .community-counts{display:flex;gap:8px;flex-wrap:wrap;color:#b8c7bb;font:9px var(--mono);margin:0 0 8px}.community{border-top:1px solid #ffffff12;padding-top:8px;color:var(--muted);font-size:11px}.community summary{cursor:pointer;color:var(--green);font:10px var(--mono)}.community p{margin:7px 0}.community ul{padding-left:17px}.community li{margin:10px 0;overflow-wrap:anywhere}.community a{color:#b6d8bc;text-underline-offset:2px}.community-empty{color:#758177;font-size:10px;margin:0 0 8px}.map-card[hidden]{display:none}@media(max-width:900px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}@media(max-width:600px){.shell{width:min(100% - 26px,1180px)}.hero{padding:52px 0 30px}.hero p{font-size:15px}.stats{gap:7px}.stat{padding:12px 10px}.stat strong{font-size:19px}.stat span{font-size:10px}.toolbar{flex-wrap:wrap}.search{flex-basis:100%}.toolbar select{flex:1;min-width:0}.result-count{margin:0 0 0 auto}.grid{grid-template-columns:1fr}.footer{flex-direction:column}.topline{height:50px}}
    @media(max-width:600px){.stats{grid-template-columns:repeat(2,minmax(0,1fr))}}
    @media(prefers-reduced-motion:reduce){*,*:before,*:after{scroll-behavior:auto!important;transition:none!important}}
  </style>
</head>
<body>
  <header class="shell topline"><div class="brand"><span class="brand-mark">W</span> WawConverter <span style="color:#718074">/</span> COMPATIBILITY INDEX</div><span>COMMUNITY TEST RECORDS</span></header>
  <main class="shell">
    <section class="hero"><div class="eyebrow">World at War → Black Ops II</div><h1>Maps, <span>verified.</span></h1><p>Compatibility notes from real conversions and in-game checks. Every map starts untested; statuses change only when a test report is recorded.</p><div class="source-note">BASE LIST · <a href="https://callofdutyrepo.com/wawmaps/" target="_blank" rel="noopener noreferrer">CODREPO WORLD AT WAR MAPS ↗</a></div></section>
    <section class="stats" aria-label="Catalog summary"><div class="stat"><strong>{{MAP_COUNT}}</strong><span>maps in the CodRepo index</span></div><div class="stat"><strong>{{COMPILE_COUNT}}</strong><span>confirmed compile successfully</span></div><div class="stat"><strong>{{PLAY_COUNT}}</strong><span>confirmed playable in game</span></div><div class="stat"><strong>{{REPORT_COUNT}}</strong><span>unverified community reports</span></div></section>
    <section aria-label="Map catalog"><div class="toolbar"><label class="search"><input id="search" type="search" placeholder="Search maps…" autocomplete="off" aria-label="Search maps"></label><select id="compile" aria-label="Filter by compile status"><option value="all">Any compile status</option><option value="passed">Compiles</option><option value="failed">Compile failed</option><option value="unknown">Untested</option></select><select id="play" aria-label="Filter by game status"><option value="all">Any game status</option><option value="passed">Played successfully</option><option value="failed">Play failed</option><option value="unknown">Not play-tested</option></select><span class="result-count" id="count" aria-live="polite"></span></div>
      <div class="section-head"><h2>Map records</h2><span>STATUS REQUIRES EVIDENCE</span></div><div class="grid" id="grid">{{CARDS}}</div>
    </section>
  </main>
  <footer class="shell footer"><span>Map names and links are sourced from <a href="https://callofdutyrepo.com/wawmaps/" target="_blank" rel="noopener noreferrer">Call of Duty Repo</a>. Compatibility notes are maintained by the B2W community.</span><span class="mono">GENERATED {{GENERATED}} · SOURCE INDEX UPDATED {{FETCHED}}</span></footer>
  <script>
    const search=document.querySelector('#search'),compile=document.querySelector('#compile'),play=document.querySelector('#play'),grid=document.querySelector('#grid'),count=document.querySelector('#count');
    function filterMaps(){const query=search.value.trim().toLocaleLowerCase();let shown=0;for(const card of grid.querySelectorAll('.map-card')){const match=card.dataset.name.includes(query)&&(compile.value==='all'||card.dataset.compile===compile.value)&&(play.value==='all'||card.dataset.play===play.value);card.hidden=!match;if(match)shown++}let empty=grid.querySelector('.empty');if(!shown&&!empty){empty=document.createElement('div');empty.className='empty';empty.textContent='No maps match those filters.';grid.append(empty)}else if(shown&&empty)empty.remove();count.textContent=`${shown.toLocaleString()} MAP${shown===1?'':'S'}`}
    search.addEventListener('input',filterMaps);compile.addEventListener('change',filterMaps);play.addEventListener('change',filterMaps);filterMaps();
  </script>
</body>
</html>
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "_site")
    parser.add_argument("--offline", action="store_true", help="use the checked-in CodRepo snapshot")
    args = parser.parse_args()
    count, used_cache = build_site(args.output, args.offline)
    print(f"Generated {args.output} with {count} maps" + (" from cached snapshot" if used_cache else " from CodRepo"))
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as stream:
            stream.write(f"## Map compatibility Pages\n\n- Maps: **{count:,}**\n- CodRepo snapshot: **{'cached' if used_cache else 'refreshed'}**\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
