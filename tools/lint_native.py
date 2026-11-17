"""Run clang-tidy on the converter's compilation units."""
import argparse
import json
import os
import shutil
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="CMake build directory with compile_commands.json")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--clang-tidy", default="clang-tidy")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    tidy = shutil.which(args.clang_tidy)
    if tidy is None:
        parser.error("clang-tidy is missing. Install the lint dependencies with python -m pip install -e .[lint]")
    build = args.build.resolve()
    database = build / "compile_commands.json"
    if not database.is_file():
        parser.error(f"{database} is missing. Configure CMake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
    native = Path(__file__).resolve().parents[1] / "native"
    entries = json.loads(database.read_text(encoding="utf-8"))
    files = sorted({path for entry in entries
                    if (path := (Path(entry["directory"]) / entry["file"]).resolve()).suffix == ".cpp"
                    and (path.is_relative_to(native / "src") or path.is_relative_to(native / "tests"))})
    if not files:
        parser.error("the compilation database has no converter C++ files")

    def lint(path: Path):
        return subprocess.run([tidy, "-p", str(build), str(path)], text=True,
                              errors="replace", capture_output=True, check=False)

    failures = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for path, result in zip(files, pool.map(lint, files), strict=True):
            if result.returncode or result.stdout.strip():
                print(path.relative_to(native), flush=True)
                print((result.stdout + result.stderr).rstrip(), flush=True)
            failures += result.returncode != 0
    print(f"clang-tidy checked {len(files)} C++ files, {failures} failed.")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
