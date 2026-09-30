"""Regenerate OAT *.template sources into a build tree (MSBuild's own template
step fails here, see AGENT_HANDOFF Session 4/17).

    python tools/retemplate_oat.py <oat repo> <src-relative template> [...]

For each template: runs build/buildtools/Release_x86/RawTemplater.exe into a
temp dir, then copies every generated file to build/src/<Project>/<same path>
and touches it newer than the template, so MSBuild does not rerun the step.
"""
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    repo = Path(sys.argv[1]).resolve()
    templater = repo / "build" / "buildtools" / "Release_x86" / "RawTemplater.exe"
    for rel in sys.argv[2:]:
        template = repo / "src" / rel
        project = Path(rel).parts[0]
        build_dir = repo / "build" / "src" / project
        with tempfile.TemporaryDirectory() as temp:
            proc = subprocess.run([str(templater), "-o", temp, str(template)], cwd=build_dir,
                                  capture_output=True, text=True, errors="replace")
            generated = [p for p in Path(temp).rglob("*") if p.is_file()]
            if proc.returncode or not generated:
                print(proc.stdout + proc.stderr)
                raise SystemExit(f"templating failed: {template}")
            for src in generated:
                dst = build_dir / src.relative_to(temp)
                dst.parent.mkdir(parents=True, exist_ok=True)
                dst.write_bytes(src.read_bytes())
                print(f"{rel} -> {dst.relative_to(repo)}")
        # the custom build step also declares <template minus .template>.log
        stamp = build_dir / Path(*Path(rel).parts[1:]).with_suffix(".log")
        stamp.parent.mkdir(parents=True, exist_ok=True)
        stamp.write_text(f"templated by tools/retemplate_oat.py\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
