"""Locate WaW assets a map references but does not ship.

A WaW mod zone may hold an asset only as a reference (",name") because WaW's
linker found it in a stock zone. Those assets are still WaW content: they live
in the stock zones of the WaW install (other zombie maps, campaign levels).
This index lists every stock zone once (T4 OAT ``Unlinker --list``, cached)
and dumps the zones that define wanted assets, so the converter ports the real
WaW asset instead of substituting anything. Each use is reported by the caller.
"""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

INDEX_VERSION = 1
DUMP_ASSETS = "material,image,fx,xmodel,weapon,xanim,sound,loadedsound,rawfile,comworld,physpreset,snddriverglobals"
DUMP_MARKER = ".waw2bo2_dump_v5_collsurfs"  # v5: xmodel collSurfs/contents in the JSON


def _zone_rank(zone: str) -> tuple:
    # zombie zones first (closest to a zombies map), then localized last
    return (0 if zone.startswith("nazi_zombie") else 2 if zone.startswith("localized") else 1, zone)


@dataclass
class StockWawAssets:
    waw_root: Path
    unlinker: Path
    work: Path
    index: dict[str, dict[str, list[str]]] = field(default_factory=dict)  # zone -> type -> names
    prepared: dict[str, Path] = field(default_factory=dict)

    @property
    def zone_dir(self) -> Path:
        return self.waw_root / "zone" / "english"

    def load(self) -> None:
        shared = self.work / 'all2raw.json'
        if shared.is_file():
            data = json.loads(shared.read_text(encoding='utf-8'))
            for filename, entry in data['zones'].items():
                if Path(filename).parent.name.lower() == 'english':
                    zone = Path(filename).stem
                    self.index[zone] = entry['index']
                    self.prepared[zone] = Path(entry['folder'])
            return
        cache = self.work / "stock_index.json"
        if cache.exists():
            data = json.loads(cache.read_text(encoding="utf-8"))
            if data.get("version") == INDEX_VERSION:
                self.index = data["zones"]
                return
        zones: dict[str, dict[str, list[str]]] = {}
        for ff in sorted(self.zone_dir.glob("*.ff")):
            out = subprocess.run([str(self.unlinker), "--no-color", "--list", str(ff)], capture_output=True,
                                 text=True, errors="replace")
            types: dict[str, list[str]] = {}
            for line in out.stdout.splitlines():
                kind, _, name = line.partition(",")
                name = name.strip()
                if not name or name.startswith(","):  # a reference, not a definition
                    continue
                types.setdefault(kind.strip(), []).append(name)
            zones[ff.stem] = types
        self.work.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps({"version": INDEX_VERSION, "zones": zones}), encoding="utf-8")
        self.index = zones

    def zones_defining(self, kind: str, name: str) -> list[str]:
        low = name.lower()
        hits = [z for z, types in self.index.items() if any(n.lower() == low for n in types.get(kind, []))]
        return sorted(hits, key=_zone_rank)

    def dump(self, zone: str) -> Path:
        if zone in self.prepared:
            return self.prepared[zone]
        out = self.work / zone
        if not (out / DUMP_MARKER).exists():
            log = self.work / f"{zone}.log"
            with log.open("w", encoding="utf-8") as fh:
                r = subprocess.run([str(self.unlinker), "--no-color", "--search-path", str(self.waw_root / "main"),
                                    "--image-format", "DDS", "--model-format", "GLTF",
                                    "--include-assets", DUMP_ASSETS, "--output-folder", str(out),
                                    str(self.zone_dir / f"{zone}.ff")], stdout=fh, stderr=subprocess.STDOUT)
            if r.returncode:
                raise RuntimeError(f"dump of stock WaW zone {zone} failed ({r.returncode}); see {log}")
            (out / DUMP_MARKER).write_text("", encoding="utf-8")
        return out

    def root_for(self, kind: str, name: str) -> tuple[str, Path] | None:
        """(zone, dump root) of the preferred stock zone defining the asset."""
        zones = self.zones_defining(kind, name)
        if not zones:
            return None
        return zones[0], self.dump(zones[0])
