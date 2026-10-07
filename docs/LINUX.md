# Linux executable

Extract the entire `WawConverter-Linux-x86_64.tar.gz` archive, then run
`WawConverter` inside its `WawConverter-Linux` folder. The GUI and CLI are native
x86-64 Linux executables with bundled Python/Tk. Built on Ubuntu 24.04;
glibc 2.39 or newer is required.

Conversion uses the included Windows worker and tools through Wine. PowerShell
is no longer required. Wine, game content, BO2 Mod Tools, and Plutonium are not
bundled. Use a Wine runner capable of running 64-bit Windows Python and 32-bit
conversion tools.

Select the same prefix as Plutonium: the directory containing `drive_c` and
`dosdevices`, not `drive_c` itself. For Faugus, use its selected Plutonium prefix:

```sh
cd WawConverter-Linux
WINEPREFIX=/absolute/path/to/plutonium-prefix ./WawConverter
```

If that runner is not `wine` on PATH, specify its executable too:

```sh
WINEPREFIX=/absolute/path/to/plutonium-prefix \
WAWCONVERTER_WINE=/absolute/path/to/runner/bin/wine ./WawConverter
```

`winepath` must be available beside the runner or on PATH. The native file
picker takes Linux paths; the app translates them for the Windows worker and
shares a single build folder. Install writes into the actual Wine user's
Plutonium storage. Browse to your game folders and original map in Setup.

Release CI checks native GUI/CLI startup and Windows worker startup through
Wine. Full Linux conversion, audio decoding, and gameplay still need
map-specific verification. If a build fails, share `build.log` and the stage
log named by its error. Run the native resource self-test with:

```sh
./WawConverter.CLI --self-test
```

Maintainers build on Linux with `python tools/build_linux.py --windows-bundle
/path/to/WawConverter`. The Windows bundle must be from the same release
source. The GitHub release workflow automates compilation and checks.
