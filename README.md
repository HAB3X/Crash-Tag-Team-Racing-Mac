# Crash Tag Team Racing for Mac

Crash Tag Team Racing (PS2) running natively on Mac. Recompiled, not emulated. Built on the same runtime as the
[Wrath of Cortex Mac port](https://github.com/HAB3X/Crash-Wrath-of-Cortex-Mac).

Work in progress: expect bugs.

## Play

Apple-silicon Macs. Download `Crash-Tag-Team-Racing-macOS.zip` from
[Releases](https://github.com/HAB3X/Crash-Tag-Team-Racing-Mac/releases), unzip and open it. The first time, right-click it and
choose Open, then pick your own USA disc image (SLUS-21191). No game data is included.

Controls and options: [docs/PLAYING.md](docs/PLAYING.md).

## What works

- Menus, the park on foot and the races
- Up to 8x resolution on the GPU
- Sound, music, movies, saving and loading
- Keyboard, mouse and controllers, all remappable; mouse in the menus; type your name
- Button hints show your keys or your controller's buttons
- PC Settings inside the game's own pause menu
- No memory card or console wording ([PC_TEXT.md](PC_TEXT.md))

## To do

- Races at full speed (the park holds 30 fps; races dip below)
- Online play
- Intel Macs, Linux, Windows

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## Credits

Shares its runtime, graphics, sound and PC menus with the [Crash Twinsanity](https://github.com/HAB3X/Crash-Twinsanity-Mac)
and [Wrath of Cortex](https://github.com/HAB3X/Crash-Wrath-of-Cortex-Mac) Mac ports. Thanks to the PCSX2 and PS2SDK projects.

Crash Tag Team Racing is by Radical Entertainment and Vivendi Universal Games. You need your own copy of the game.
