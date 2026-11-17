"""Share immutable dependency files between extraction generations."""
from __future__ import annotations

import json
from pathlib import Path
from tempfile import TemporaryDirectory

from . import native


def share_dependencies(root: Path, staging: Path, inputs: dict, files: list[str]) -> None:
    if root.parent.name != "dependencies" or native.tool() is None:
        return
    identity = {key: val for key, val in inputs.items() if key != "assets"}
    wanted = set((inputs["assets"] or "").split(","))
    candidates = []
    for receipt in root.glob("*/extraction.json"):
        try:
            old = json.loads(receipt.read_text(encoding="utf-8"))["inputs"]
            if {key: val for key, val in old.items() if key != "assets"} != identity:
                continue
            old_types = set((old["assets"] or "").split(","))
            candidates.append((len(old_types & wanted), receipt.stat().st_mtime_ns, receipt.parent))
        except (OSError, ValueError, KeyError):
            continue
    if not candidates:
        return
    # Recent generations with the most shared types are usually supersets
    # of older results. Bound extra reads when the cache has many generations.
    prior = [str(path) for _, _, path in sorted(candidates, reverse=True)[:2]]
    with TemporaryDirectory(prefix="waw2bo2_cache_share_") as temp:
        query = Path(temp) / "share.json"
        query.write_text(json.dumps({"staging": str(staging), "prior": prior, "files": files}), encoding="utf-8")
        shared = json.loads(native.run("share-cache-files", str(query)))
    if shared["files"]:
        space = shared["shared_bytes"] / (1024 ** 2)
        print(f'Extract All: shared {shared["files"]} identical dependency files ({space:.1f} MiB)', flush=True)
