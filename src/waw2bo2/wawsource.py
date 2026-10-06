"""WaW assets that exist only as WaW Mod Tools sources (GUIDELINES 27, source 4).

Some effects a map loads are in no compiled zone of the WaW install, but the
WaW Mod Tools ship their sources (``raw/fx/*.efx``). They are compiled here
with WaW's own linker (``bin/linker_pc.exe``) into a scratch fastfile, which
the T4 OAT unlinker then dumps like any other WaW zone. No .efx reader is
reimplemented: the compiled data is exactly what WaW's toolchain produces
(verified: recompiled stock effects dump identical to the shipped ones,
except where the shipped zone was built from an older revision of the source).

FX source recovery is opt-in: those effects were never compiled into the map,
so porting them adds content the WaW build did not show. Referenced material and model
definitions also use this compiler before being declared absent. Every use is
reported by the caller (``WAW_SOURCE_ASSET``).
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

LINKER = Path("bin") / "linker_pc.exe"
DUMP_ASSETS = "material,image,fx,xmodel"
DUMP_MARKER = ".waw2bo2_source_dump_v3"  # v3: verify the requested asset was actually dumped
LINK_TIMEOUT = 300  # s; after an unrecoverable error the linker waits for a key press


class WawSourceError(RuntimeError):
    pass


def locate_mod_tools(explicit: Path | None, waw_root: Path | None, project_root: Path | None = None) -> Path | None:
    """The WaW Mod Tools folder (the one holding bin/linker_pc.exe). An explicit
    path wins; otherwise the WaW install itself (the default installer target)
    and a ``wawModTools`` folder next to the project are tried."""
    if explicit is not None:
        return explicit
    for cand in (waw_root, project_root / "wawModTools" if project_root else None):
        if cand is not None and (cand / LINKER).exists():
            return cand
    return None


def _junction(link: Path, target: Path) -> None:
    if link.exists():
        return
    if os.name == "nt":
        import _winapi
        _winapi.CreateJunction(str(target), str(link))
    else:
        link.symlink_to(target, target_is_directory=True)


@dataclass
class WawSourceAssets:
    mod_tools: Path
    waw_root: Path
    unlinker: Path
    work: Path

    @property
    def linker(self) -> Path:
        return self.mod_tools / LINKER

    @property
    def raw(self) -> Path:
        # The installer puts raw/ into the WaW install; a self-contained Mod
        # Tools folder may carry its own.
        for cand in (self.mod_tools / "raw", self.waw_root / "raw"):
            if (cand / "fx").is_dir():
                return cand
        return self.waw_root / "raw"

    def check(self) -> list[str]:
        problems = []
        if not self.linker.exists():
            problems.append(f"WaW linker {self.linker} missing")
        if not (self.raw / "fx").is_dir():
            problems.append(f"no raw/fx in {self.mod_tools} or {self.waw_root}")
        if not (self.waw_root / "main").is_dir():
            problems.append(f"no main/ in {self.waw_root}")
        return problems

    def source(self, kind: str, name: str) -> Path | None:
        if kind == 'xmodel':
            p = self.raw / 'xmodel' / name
            return p if p.is_file() else None
        if kind == "fx":
            p = self.raw / "fx" / f"{name}.efx"
            return p if p.is_file() else None
        if kind == 'material':
            p = self.raw / 'materials' / name
            if p.is_file():
                return p
            # The linker adds a draw-family prefix; raw materials do not have it.
            family, _, base = name.partition('/')
            if family in ('mc', 'wc', 'mlv'):
                p = self.raw / 'materials' / base
                return p if p.is_file() else None
            return None
        return None

    def _workspace(self) -> Path:
        # The linker resolves ../raw, ../zone_source, ../zone and the game
        # files (main/iw_00.iwd holds fileSysCheck.cfg) relative to its cwd.
        ws = self.work / "linker"
        for d in ("bin", "zone_source", "zone/english"):
            (ws / d).mkdir(parents=True, exist_ok=True)
        _junction(ws / "raw", self.raw)
        _junction(ws / "main", self.waw_root / "main")
        return ws

    def compile(self, kind: str, name: str) -> Path | None:
        """Compile one source asset (and everything it references) and return
        the dump root, or None when the Mod Tools have no source for it."""
        src = self.source(kind, name)
        if src is None:
            return None
        # Engine $ materials are compiled without a draw-family variant. Models
        # imported from other CoD tools can nevertheless name mc/$default3d.
        family, _, base = name.partition('/')
        if (kind == 'material' and family in ('mc', 'wc', 'mlv') and base.startswith('$')
                and src == self.raw / 'materials' / base):
            recovered = self.compile(kind, base)
            from .techsets import oat_material_path
            original = recovered / oat_material_path(base)
            alias = recovered / oat_material_path(name)
            alias.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(original, alias)
            return recovered
        zone = "w2bsrc_" + re.sub(r"[^A-Za-z0-9_]", "_", f"{kind}_{name}").lower()
        out = self.work / zone
        stamp = f"{src.stat().st_mtime_ns} {src.stat().st_size}"
        marker = out / DUMP_MARKER
        if marker.exists() and marker.read_text(encoding="utf-8") == stamp:
            return out
        ws = self._workspace()
        (ws / "zone_source" / f"{zone}.csv").write_text(f"{kind},{name}\n", encoding="ascii")
        ff = ws / "zone" / "english" / f"{zone}.ff"
        if ff.exists():
            ff.unlink()
        log = self.work / f"{zone}.link.log"
        try:
            with log.open("w", encoding="utf-8", errors="replace") as fh:
                subprocess.run([str(self.linker), "-language", "english", zone], cwd=ws / "bin",
                               stdin=subprocess.DEVNULL, stdout=fh, stderr=subprocess.STDOUT, timeout=LINK_TIMEOUT)
        except subprocess.TimeoutExpired:
            raise WawSourceError(f"WaW linker hung compiling {kind} {name} (see {log})")
        text = log.read_text(encoding="utf-8", errors="replace")
        if not ff.exists() or "UNRECOVERABLE ERROR" in text:
            raise WawSourceError(f"WaW linker failed compiling {kind} {name} (see {log})")
        if out.exists():
            # A changed source must not inherit an asset from an older dump
            # when the linker emits an incomplete fastfile.
            out.resolve().relative_to(self.work.resolve())
            if out.is_symlink():
                raise WawSourceError(f"source dump is a symlink: {out}")
            shutil.rmtree(out)
        dump_log = self.work / f"{zone}.dump.log"
        with dump_log.open("w", encoding="utf-8") as fh:
            r = subprocess.run([str(self.unlinker), "--no-color", "--search-path", str(self.waw_root / "main"),
                                "--image-format", "DDS", "--model-format", "GLTF",
                                "--include-assets", DUMP_ASSETS, "--output-folder", str(out), str(ff)],
                               stdout=fh, stderr=subprocess.STDOUT)
        if r.returncode:
            raise WawSourceError(f"dump of compiled source {kind} {name} failed ({r.returncode}); see {dump_log}")
        from .assetresolve import asset_path
        if not (out / asset_path(kind, name)).is_file():
            raise WawSourceError(f"WaW linker produced no {kind} {name} (see {log})")
        marker.write_text(stamp, encoding="utf-8")
        return out

    def link_warnings(self, kind: str, name: str) -> list[str]:
        """Assets the linker could not load while compiling (missing sources)."""
        zone = "w2bsrc_" + re.sub(r"[^A-Za-z0-9_]", "_", f"{kind}_{name}").lower()
        log = self.work / f"{zone}.link.log"
        if not log.exists():
            return []
        return [ln.strip() for ln in log.read_text(encoding="utf-8", errors="replace").splitlines()
                if "failed loading" in ln or ln.strip().endswith("not found")]
