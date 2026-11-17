# Developing B2W

[Setup and CLI usage](USAGE.md) · [Conversion guidelines](../GUIDELINES.MD) · [C++ migration](CPP_PORT_PLAN.md) · [Build and release pipeline](CI_CD.md)

## Lint checks

Run these from an x64 Visual Studio Developer PowerShell. clang-tidy uses
the native build's CMake compilation database and third-party headers.

```powershell
python -m pip install -e '.[lint]'
python -m ruff check .
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
python tools/lint_native.py native/build
```

For the map compatibility page, use Node.js 24 and run `npm ci` followed by
`npm run lint`. Windows build CI runs Ruff and clang-tidy before packaging.
The Pages workflow runs ESLint before building the catalog.

## Project directories

- `native/` contains the C++ converter.
- `src/waw2bo2/` contains the desktop launcher, Python conversion stages, and compatibility data.
- `tests/` contains regression tests. See [Usage](USAGE.md) for the test command.
- `tools/` contains build, audit, and maintenance commands.
- `vendor/` contains the patched OpenAssetTools sources and patch files. See the [T6 fork license](../vendor/OpenAssetToolsT6/LICENSE).

See [source model recovery](SCRIPT_MODEL_SOURCE_RECOVERY.md), [shader translation](SHADER_TRANSLATION.md),
[sound conversion](SOUND_CONVERSION.md), and [collision conversion](COLLISION_CONVERSION.md) for subsystem details.
