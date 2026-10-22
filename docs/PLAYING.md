# Playing Crash Tag Team Racing on Mac

## You need

- An Apple-silicon Mac on a recent macOS
- Your own USA disc image (SLUS-21191) as an `.iso`. PAL and the PSP version aren't supported.

## First run

1. Unzip and move **Crash Tag Team Racing.app** anywhere (Applications is fine).
2. The first time, right-click it, choose **Open**, then **Open** again.
3. Pick your disc image. It's remembered; delete `disc_image.txt` (below) to pick another.

## Where your files are

`~/Library/Application Support/Crash Tag Team Racing/` (Finder: Go › Go to Folder)

| What | File |
| --- | --- |
| Saves | `memcards/port0/` |
| Settings | `options.txt`, `graphics.txt`, `bindings.txt` |
| Disc path | `disc_image.txt` |

## Controls

Controllers work out of the box. Keyboard and mouse:

| Action | On foot | Driving |
| --- | --- | --- |
| Move / steer | W A S D | A / D |
| Accelerate / brake | | W / S (tap brake while turning to powerslide) |
| Jump | Space | |
| Spin attack | Left mouse (or J) | |
| Body slam | Left Shift (or K) | |
| Talk | E (or I) | |
| Camera | Arrows; Q first person, right mouse zoom, R reset | |
| Power-up / turret | | Space / left mouse |
| Clash / swap seats | | Left Shift or E / Q |
| Aim | | I / K |
| Map / skip tutorial | Tab / Backspace | |
| Taunt / reset car | | T / R |
| Walk | Left Ctrl | |
| Pause | Esc or Enter | Esc or Enter |

Menus: arrows or WASD, Enter to choose, Esc to go back, or use the mouse. Fullscreen: Cmd+F. Rebind everything in
**PC Settings › Controls**.

## PC Settings

In the pause menu, under **Preferences**:

- **Display:** window or fullscreen, resolution (1x–8x), texture filtering and upscale, anti-aliasing, CRT filter, v-sync
- **Sound:** volume, mute in the background
- **Controls:** prompts and key bindings

Running slowly? Lower the resolution (3x is the default).

## Problems

Run the app from Terminal to see its log:
`"/Applications/Crash Tag Team Racing.app/Contents/MacOS/crash-cttr"`. Include it in bug reports.

## Licences

Crash Tag Team Racing belongs to its owners. The app includes SDL 3, Luckiest Guy, Fredoka and stb_truetype; licences are in
`Contents/Resources/licenses`.
