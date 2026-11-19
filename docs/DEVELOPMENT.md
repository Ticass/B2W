# Developing B2W

[Setup and CLI usage](USAGE.md) · [Conversion guidelines](../GUIDELINES.MD) · [C++ migration](CPP_PORT_PLAN.md) · [Build and release pipeline](CI_CD.md)

## Lint checks

Run these from an x64 Visual Studio Developer PowerShell. clang-tidy uses
the native build's CMake compilation database and third-party headers.

```powershell
powershell -File tools/fetch_wheel_binary.ps1 -Package clang-tidy -Version 22.1.7 -Member '*/bin/clang-tidy.exe' -Destination work/tools/clang-tidy.exe
cmake -S native -B native/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build native/build --target w2b_lint
native/build/w2b_lint.exe native/build --clang-tidy work/tools/clang-tidy.exe
```

For the map compatibility page, use Node.js 24 and run `npm ci` followed by
`npm run lint`. Windows build CI runs clang-tidy (22.1.7, fetched with
`tools/fetch_wheel_binary.ps1`) before packaging.
The Pages workflow runs ESLint before building the catalog.

## Project directories

- `native/` contains the converter (`waw2bo2n`), the desktop launcher (`B2W`) and their tests (`native/tests`, run `native/build/w2b_tests.exe`).
- `src/waw2bo2/compat/` contains the compatibility data the converter reads.
- `tools/` contains the build, packaging and release scripts (PowerShell).
- `services/discord_support/` contains the Discord support bot (Python).
- `vendor/` contains the patched OpenAssetTools sources and patch files. See the [T6 fork license](../vendor/OpenAssetToolsT6/LICENSE).

See [source model recovery](SCRIPT_MODEL_SOURCE_RECOVERY.md), [shader translation](SHADER_TRANSLATION.md),
[sound conversion](SOUND_CONVERSION.md), and [collision conversion](COLLISION_CONVERSION.md) for subsystem details.
