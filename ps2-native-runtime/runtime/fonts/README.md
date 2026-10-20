# The PC menus' fonts

The PC menus (`runtime/src/ui/`) draw in these, as the Crash Twinsanity native port's options screen does; they're built into
the program (`ui/overlay.cpp`). Copied from the Twinsanity port's `native/third_party/fonts`.

| File | Font | Licence | Source |
|---|---|---|---|
| LuckiestGuy-Regular.ttf | Luckiest Guy (Astigmatic) | Apache License 2.0, LICENSE-LuckiestGuy.txt | https://github.com/google/fonts/tree/main/apache/luckiestguy |
| Fredoka-SemiBold.ttf, Fredoka-Bold.ttf | Fredoka (The Fredoka Project Authors), its weights 600 and 700 made from the variable font (stb_truetype reads no variable fonts) | SIL Open Font License 1.1, OFL-Fredoka.txt | https://github.com/google/fonts/tree/main/ofl/fredoka |

`runtime/third_party/stb/stb_truetype.h` (Sean Barrett's, public domain or MIT: its LICENSE.txt) reads them.
