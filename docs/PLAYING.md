# Playing Crash Tag Team Racing on a Mac

This is a native Mac port of Crash Tag Team Racing, made by recompiling the PS2 game's code. It contains no game data: you need
your own copy of the game.

## What you need

- A Mac with Apple silicon (M1 or later) running a recent macOS.
- A disc image of **your own** USA copy of the game (SLUS-21191), as an `.iso`. Other versions (PAL, the PSP game) aren't
  supported: the app checks the disc and says so.

Nothing else: the app carries its own libraries (no Homebrew needed).

## First run

1. Unzip `Crash-Tag-Team-Racing-macOS.zip` and move **Crash Tag Team Racing.app** wherever you like (Applications is fine).
2. The app is signed ad hoc, not by an Apple developer account. The first time you open it, macOS may say it can't check it:
   right-click (or Control-click) the app, choose **Open**, then **Open** again. You only have to do this once.
3. On the first launch the app asks for your disc image with a file dialog. Pick your `.iso`. It remembers it; to pick another,
   delete `disc_image.txt` from the settings folder (below).

## Where things are

Everything the game keeps is in **~/Library/Application Support/Crash Tag Team Racing/** (in Finder: Go > Go to Folder…, then
paste that path):

| What | Where |
| --- | --- |
| Saves (the game's memory card) | `memcards/port0/` |
| The options | `options.txt`, `graphics.txt` |
| Your controls | `bindings.txt` |
| The disc image's path | `disc_image.txt` |

Saving works like a PC game: your saves are dated with your Mac's clock, and no screen mentions a memory card.

## Controls

A controller (Xbox, PlayStation, Switch and most others) works out of the box, as on the PS2. The keyboard and mouse have a set of
keys for each part of the game:

| Action | On foot | Driving |
| --- | --- | --- |
| Move / steer | W A S D | A / D (or Left / Right) |
| Accelerate / brake and reverse | | W / S (or Up / Down); tap brake while turning to powerslide |
| Jump | Space | |
| Spin attack | Left mouse button (or J) | |
| Body slam | Left Shift (or K) | |
| Talk | E (or I) | |
| Camera | Arrow keys; Q first person, right mouse button zoom, R reset | |
| Power-up / turret | | Space / left mouse button |
| Clash / swap driver and gunner | | Left Shift or E / Q |
| Aim or tilt | | I / K |
| Map / skip tutorials | Tab / Backspace | |
| Taunt / reset the car | | T / R |
| Walk | Left Ctrl toggles walking | |
| Pause | Esc (or Enter) | Esc (or Enter) |

In the menus: the arrow keys or W A S D move, Enter or Space chooses, Backspace or Esc goes back, and the mouse points and
clicks. On the name screen just type your name (Backspace deletes, Enter is done). Fullscreen is Cmd+F (or Alt+Enter).

The game's control hints name the keys you have bound when you last used the keyboard, or your controller's buttons when you
last used a controller. Everything can be rebound in **PC Settings > Controls**.

## The pause menu and PC Settings

The pause menu is the game's own (Esc or Enter). Its **Preferences** has **PC Settings** in place of the controller's
vibration, which opens the PC menus over the paused game, in the Crash Twinsanity Mac port's style:

- **Gameplay:** pause in the background.
- **Display:** window size or fullscreen, the internal resolution (1x to 8x the PS2's), texture filtering, texture upscale
  (2x, 4x), anti-aliasing (FXAA, or supersampling 2x and 4x, which is slower), the CRT filter, v-sync.
- **Sound:** the master volume, mute in the background.
- **Controls:** the button prompts' style and every key to rebind, for the menus, on foot and driving.

Closing PC Settings goes back to the game's pause menu. If the game runs slowly, lower the internal resolution: 3x is the
default.

## If something goes wrong

The app writes what it's doing to its standard output: run it from Terminal to see it,
`"/Applications/Crash Tag Team Racing.app/Contents/MacOS/crash-cttr"`. A crash prints a report there; please include it with any
bug report.

## Licences

Crash Tag Team Racing and its content belong to their owners. The app bundles SDL 3 (zlib licence) and the fonts Luckiest Guy
(Apache 2.0) and Fredoka (SIL Open Font Licence), whose licences are in `Contents/Resources/licenses`, and stb_truetype (public
domain).
