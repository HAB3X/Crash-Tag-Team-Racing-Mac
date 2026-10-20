# Building Crash Tag Team Racing for Mac

The game's executable (USA, SLUS-21191) is statically recompiled: every function of the R5900 code is turned into C ahead of
time and built into a native Mac program. The runtime in `ps2-native-runtime/` stands in for the rest of the console: the kernel,
DMA, the IOP's modules (files, the CD, pads, memory cards), the SPU2 sound chip, and the GS, VIF and VU graphics hardware (drawn
on the GPU with OpenGL). This repository's own code is what's specific to Crash Tag Team Racing: Radical's IOP audio module and
the movies' sound, the PC controls, menus and hints, the texts and the app.

The repository has none of the game's code or data. You need your own copy of the game: building reads its executable from your
disc image, and the recompiled C stays in `build/` on your machine.

**Playing on a Mac:** download `Crash-Tag-Team-Racing-macOS.zip` from the
[releases](https://github.com/HAB3X/Crash-Tag-Team-Racing-Mac/releases) and read [PLAYING.md](PLAYING.md).

[ps2-native-runtime/DESIGN.md](../ps2-native-runtime/DESIGN.md) explains how the runtime is put together.

## What you need

- **A Mac with Apple silicon** and Xcode's command line tools (`xcode-select --install`).
- **The game**: a disc image of the USA release (SLUS-21191), as an `.iso`.
- **CMake** 3.20 or later, **Ninja** and **SDL3**: `brew install cmake ninja sdl3`.
- **Python 3.11 or later**, for the recompiler (it only needs Python's own library). The app's icon, made from the game's own
  save icon on your disc, also needs Pillow and NumPy (`python3 -m pip install pillow numpy`); without them the app is built
  with no icon.
- **Git**, to clone this repository: `git clone https://github.com/HAB3X/Crash-Tag-Team-Racing-Mac.git`

## Building

In the repository's folder:

```sh
cmake -S . -B build -G Ninja -DCTTR_DISC="/path/to/your/game.iso"
cmake --build build
./run.sh
```

The first build recompiles the executable (a few minutes); the build is `build/crash-cttr.app`. `run.sh` runs it with the disc
from `CTTR_DISC` or from `local.json` (`{"disc": "/path/to/game.iso"}`), passing its options on.

The release, the app made standalone (its libraries inside, no Homebrew needed) and zipped:

```sh
cmake --build build --target release
```

That makes `build/dist/Crash Tag Team Racing.app` and `build/dist/Crash-Tag-Team-Racing-macOS.zip`.

## Options for testing

| Option | What |
|---|---|
| `--headless --snapshots DIR --snapshot-every S` (or `--snapshot-at S,S,...`) | Runs with no window, saving pictures |
| `--run-seconds S` | Stops after S seconds |
| `--press S:BUTTON,...`, `--key S:KEY[:HOLD],...`, `--ui S:KEY,...` | Presses buttons, holds keys and uses the PC menus at those times of the game's own clock |
| `--settings DIR`, `--memcards DIR` | Another settings folder or save folder |
| `--fps` | Logs the frame rate |
| `--gs hardware\|software`, `--scale 1-8`, `--texture-upscale 0\|2\|4` | The renderer, its internal resolution and texture upscale |
| `--mute`, `--audio-dump FILE.wav` | No sound, or what's played kept in a file |
| `--log kernel,syscall,io,dma,sif,iop,gs,...` | Logs the runtime's parts |

`CTTR_AUDIO_TRACE=1` logs the audio module's loads and voices, and `CTTR_MENU_TRACE=1` the menus' clicks.

## Layout

| Path | What |
|---|---|
| `src/` | Crash Tag Team Racing's own code: Radical's audio module (`audio.cpp`), the movies' IOP side (`radiop.cpp`), the PC controls (`controls.cpp`), the mouse on the menus (`menus.cpp`), names typed (`typing.cpp`), the hints (`hints.cpp`), the pause menu and its PC Settings (`pause.cpp`, `pcsettings.cpp`), the texts (`pctext.cpp`), fibers (`fibers.cpp`) |
| `config/cttr.toml` | The recompiler's settings: the game's functions replaced by `src/`, its VU1 programs |
| `tools/` | The app's icon and the standalone app |
| `PC_TEXT.md` | Every text said as a computer's, with its original |
| `ps2-native-runtime/` | The runtime: the recompiler, memory, the kernel, DMA, the IOP, the SPU2, input, the window, the PC menus, and the GS, VIF and VU hardware |

`build/` is made by the build and isn't committed.

## Credits

- The [Crash Twinsanity Mac port](https://github.com/HAB3X/Crash-Twinsanity-Mac) and the
  [Wrath of Cortex Mac port](https://github.com/HAB3X/Crash-Wrath-of-Cortex-Mac), whose runtime this shares.
- The PS2 hardware documentation and the work of the PCSX2 and PS2SDK projects.
- Crash Tag Team Racing is a game by Radical Entertainment and Vivendi Universal Games. This repository has none of its code or
  data: you need your own copy of the disc.
