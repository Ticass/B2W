# Linux

The Linux download contains the same programs as the Windows download (the
B2W launcher and the `waw2bo2n` converter) and runs them through Wine. Wine is
**not** bundled: install it once with your distribution's package manager. This
works on any recent x86-64 distribution.

## 1. Install Wine

You need 64-bit Wine able to run 32-bit programs (the BO2 linker is 32-bit).

**Ubuntu, Debian, Linux Mint, Pop!_OS and other Ubuntu/Debian-based distributions**

```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install wine wine64 wine32
```

**Fedora**

```sh
sudo dnf install wine
```

**Arch Linux, Manjaro, EndeavourOS**

Enable the `[multilib]` repository in `/etc/pacman.conf`, then:

```sh
sudo pacman -Syu wine
```

**Other distributions:** install Wine with 32-bit support.

## 2. Run it

Extract the entire `WawConverter-Linux-x86_64.tar.gz` archive, then run
`./WawConverter` inside its `WawConverter-Linux` folder. `./WawConverter.CLI`
is the command-line converter (`waw2bo2n`).

Use the same Wine prefix as Plutonium: the directory containing `drive_c` and
`dosdevices`, not `drive_c` itself. Installing a map then writes into that
prefix's Plutonium storage. For Faugus, use its selected Plutonium prefix:

```sh
cd WawConverter-Linux
WINEPREFIX=/absolute/path/to/plutonium-prefix ./WawConverter
```

If that runner is not `wine` on PATH, give its executable too:

```sh
WINEPREFIX=/absolute/path/to/plutonium-prefix \
WAWCONVERTER_WINE=/absolute/path/to/runner/bin/wine ./WawConverter
```

The launcher runs inside Wine, so its file pickers show Wine's drives; your
Linux files are under `Z:`. Browse to your game folders and original map in
Setup. Game content, BO2 Mod Tools and Plutonium are not included.

If a build fails, share `build.log` and the stage log named by its error, or
the diagnostics ZIP from **Reports → Save Diagnostics**.

Maintainers assemble the release from the Windows release folder with
`sh tools/package_linux.sh /path/to/WawConverter`. The Windows folder must be
from the same release source. The GitHub release workflow does this.
