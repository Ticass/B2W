"""What runs in place of each WaW zombie-framework function in a converted map.

Every build already writes framework_coverage.json/.md beside its report
(waw2bo2.framework_coverage). This re-runs it for a staged project.

usage: python tools/framework_coverage.py <stage zone_raw/project> <waw script root>... [--md out.md]
The WaW roots are searched in order (map patch first, then map, then stock raw/).
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from waw2bo2 import framework_coverage  # noqa: E402
from waw2bo2.gscport import API  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    parser.add_argument("roots", type=Path, nargs="+")
    parser.add_argument("--md", type=Path)
    args = parser.parse_args()
    report = json.loads((args.project / "bridge_stage.report.json").read_text(encoding="utf-8"))
    cov = framework_coverage.coverage(report, args.roots)
    if args.md:
        args.md.write_text(framework_coverage.markdown(args.project.name, cov), encoding="utf-8")
    print(framework_coverage.totals(cov))
    for key, rows in sorted(cov.items()):
        print(key, dict(Counter(v.split(" ")[0] for v in rows.values())))
    for line in framework_coverage.undocumented_bo2(cov, API):
        print(line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
