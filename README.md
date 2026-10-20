# Crash Tag Team Racing for Mac

Crash Tag Team Racing (PS2) running natively on Mac, no emulator. The game's own code is recompiled from its PS2 executable to
native code ahead of time, and a runtime stands in for the rest of the console, with the PS2's graphics chips recreated on the
GPU. It's built on the same runtime as the [Crash Bandicoot The Wrath of Cortex Mac port](https://github.com/HAB3X/Crash-Wrath-of-Cortex-Mac).

This is a work in progress and it still has bugs.

## Play

For Apple silicon Macs. Download `Crash-Tag-Team-Racing-macOS.zip` from the
[releases](https://github.com/HAB3X/Crash-Tag-Team-Racing-Mac/releases), unzip it and open Crash Tag Team Racing. The first time,
right click the app and choose Open, then pick your own USA disc image (SLUS 21191). The game is not included.

[docs/PLAYING.md](docs/PLAYING.md) has the controls and options.

## What works

* The game's code recompiled to run natively: the menus, the park on foot and the races
* Graphics on the GPU at up to 8x resolution
* Sound and music, the movies with their sound, and saving and loading
* Keyboard, mouse and controllers, with remappable controls for the menus, on foot and driving; click the menus with the mouse
  and type your name
* The control hints name the keys you play with, or your controller's buttons
* The game's own pause menu, with PC Settings in its Preferences (display, sound, controls) in the Crash Twinsanity Mac port's
  style
* The game's texts about memory cards and controllers said as a computer's ([PC_TEXT.md](PC_TEXT.md))

## Still to do

* Races at full speed (they run below 30 frames a second for now; the park holds its 30)
* Online play, peer to peer
* Intel Macs
* Linux and Windows

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

## Credits

The [Crash Twinsanity Mac port](https://github.com/HAB3X/Crash-Twinsanity-Mac) and the
[Wrath of Cortex Mac port](https://github.com/HAB3X/Crash-Wrath-of-Cortex-Mac), whose runtime, graphics hardware layer, sound
chip and PC menus this shares. The PS2 hardware documentation and the work of the PCSX2 and PS2SDK projects.

Crash Tag Team Racing is a game by Radical Entertainment and Vivendi Universal Games. This repository has none of its code or
data: you need your own copy of the disc.
