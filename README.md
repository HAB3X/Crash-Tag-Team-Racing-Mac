# PS2 native runtime

A runtime and a static recompiler for running PlayStation 2 games natively on Mac, no emulator. The game's executable is
recompiled to C ahead of time, and the runtime stands in for the rest of the console: memory, the kernel, the DMA controller, the
IOP, and the graphics hardware.

Each game is its own repository that builds against this one. The first is
[Crash Bandicoot: The Wrath of Cortex](../crash-woc-mac).

This is a work in progress and it still has bugs.

## What's in it

* `recomp/`: the recompiler (Python). It finds the functions of a stripped executable and turns every R5900 instruction into C,
  including the 128-bit MMI set, the FPU and VU0 macro mode
* `runtime/`: the EE's address space, the hardware registers, the DMA controller, a high level kernel (threads, semaphores,
  interrupts) and a high level IOP (the SIF and the modules' RPC servers: files, CD, module loading, pads, memory cards)
* `hw/`: the GS (drawn on the GPU with OpenGL, or in software), GIF, VIF and VU emulation from the Crash Twinsanity native port
  (see `hw/README.md`)
* `tools/`: reading files from a disc image, disassemblers for the R5900 and the VUs

[DESIGN.md](DESIGN.md) has how it all works.

## Still to do

* Sound
* Movies
* Windows and Linux

## Building

A game's repository builds this one with CMake (`add_subdirectory`) and calls `ps2_recompile()` with its disc image. You need
CMake, Ninja, Python 3 and SDL3.

## Credits

The graphics hardware comes from the Crash Twinsanity native port. The PS2 hardware documentation and the work of the PCSX2 and
PS2SDK projects made this possible.

You need your own copy of each game: nothing of any game is in this repository.
