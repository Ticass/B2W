# Map compatibility catalog

The [Map Compatibility page](https://ticass.github.io/B2W/) records whether
each World at War map has compiled and launched successfully with WawConverter.
Its starting list comes from [Call of Duty Repo's WAW maps index](https://callofdutyrepo.com/wawmaps/)
and is refreshed by a GitHub-hosted runner every day. The page is generated and
deployed by `.github/workflows/map-compatibility-pages.yml`; no build runs on a
developer's PC. If CodRepo is unavailable, the generator uses the checked-in
`catalog/codrepo-maps.json` snapshot and tries again at the next scheduled run.

Every imported map starts with **Unknown** compile and game status. A successful
conversion alone does not prove that a map plays correctly. Compatibility
entries live in `catalog/compatibility.json`, keyed by the map's CodRepo
WordPress post ID. Update that file in a pull request when a reproducible test
has been completed:

```json
{
  "maps": {
    "64072": {
      "compile": "passed",
      "play": "passed",
      "evidence": [
        {
          "date": "2026-10-08",
          "converter_version": "0.2.14",
          "tester": "community",
          "notes": "Converted, installed, and loaded into a solo match."
        }
      ]
    }
  }
}
```

Use `passed`, `failed`, or `unknown` for each status. Record the converter
version, date, test conditions, and a concise result in the newest evidence
entry. A failed test is useful compatibility information too. Keep newest
evidence first. The site filters by map name and either status, links each
entry to its CodRepo page, and includes an offline JSON copy of the base list.

Pull requests build a Pages artifact on GitHub for validation but do not
publish it. Pushes to `main` and daily scheduled runs deploy the site. GitHub
Pages is configured to use GitHub Actions for this repository; if Pages is
recreated or the repository is moved, set **Settings → Pages → Build and
deployment source** to **GitHub Actions**.
