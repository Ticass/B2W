# Linux

The Linux download contains the WawConverter app as plain Python, plus the
Windows conversion worker that runs through Wine. Python, Tk and Wine are **not**
bundled: install them once with your distribution's package manager, then run
the app. This works on any recent x86-64 distribution, not only the one the
release was built on.

## 1. Install the dependencies

You need **Python 3.11 or newer** with **Tk** and **Pillow** (the GUI), and **Wine**
(the conversion worker; 64-bit Wine able to run 32-bit programs).

**Ubuntu, Debian, Linux Mint, Pop!_OS and other Ubuntu/Debian-based distributions**

```sh
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install python3 python3-tk python3-pil python3-pil.imagetk wine wine64 wine32
```

Ubuntu 24.04 or newer (or Debian 12 or newer) is needed for Python 3.11+.
Ubuntu 22.04 and Mint 21 ship Python 3.10: install `python3.11` and the
matching Tk package, then start the app with `WAWCONVERTER_PYTHON=python3.11`.

**Fedora**

```sh
sudo dnf install python3 python3-tkinter python3-pillow python3-pillow-tk wine
```

**Arch Linux, Manjaro, EndeavourOS**

Enable the `[multilib]` repository in `/etc/pacman.conf` (needed by Wine), then:

```sh
sudo pacman -Syu python tk python-pillow wine
```

**Other distributions:** install Python 3.11+, its Tk module (`tkinter`),
Pillow with Tk support (`PIL.ImageTk`), and Wine with 32-bit support.

If something is missing, the app says what and prints these commands.

## 2. Run it

Extract the entire `WawConverter-Linux-x86_64.tar.gz` archive, then run
`./WawConverter` inside its `WawConverter-Linux` folder. `./WawConverter.CLI`
is the command-line interface and `./All2Raw` the Extract All tool. To use a
specific Python, set `WAWCONVERTER_PYTHON` (for example `python3.12`).

Select the same Wine prefix as Plutonium: the directory containing `drive_c` and
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
Game content, BO2 Mod Tools and Plutonium are not included.

Release CI checks GUI/CLI startup with the distribution's own Python packages
and Windows worker startup through Wine. If a build fails, share `build.log`
and the stage log named by its error. Check the installation with:

```sh
./WawConverter.CLI --self-test
```

Maintainers assemble the release on any OS with `python tools/build_linux.py
--windows-bundle /path/to/WawConverter`. The Windows bundle must be from the
same release source. The GitHub release workflow automates this and the checks.
