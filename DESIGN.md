# Design

How a PS2 game's executable becomes a native program: its MIPS code recompiled to C ahead of time, and a runtime that stands in
for the rest of the console. Wrath of Cortex (`../crash-woc-mac`) is the first game; nothing here is specific to it.

```
disc image ──tools/disc.py──▶ the executable (ELF, in the build folder only)
                                   │
                                   ▼  recomp/ (Python): find the functions, translate every instruction to C
                          one C function per MIPS function + an address table
                                   │  clang, arm64
                                   ▼
             the game ──calls──▶ runtime/ (C++): EE memory, hardware registers, DMAC, HLE kernel, HLE IOP, presenter
                                   │
                                   ▼
                          hw/: GS (software and OpenGL), GIF, VIF0/1, VU0/1 (from the Crash Twinsanity native port)
```

## Why recompile, and why our own recompiler

- An interpreter or JIT (an emulator) costs the most work at run time and gives the least room to change the game. Static
  recompilation turns the executable into C once: the host compiler optimises it, a debugger and a profiler see functions named
  by their PS2 address, and fixes or enhancements can replace any function by address.
- The known PS2 static recompiler, PS2Recomp (github.com/ran-j/PS2Recomp), is GPL-3.0, as are the game projects built on it.
  By the owner's rule GPL code is read-only reference, so this recompiler is written from scratch: the R5900 manual's
  instruction semantics, nothing copied.
- The game's own code stays as compiled by its developers: no decompilation is needed to run it. A decompilation, where one
  exists (WoC has one), is a reference for understanding the code and for replacing functions later.

## The recompiler (`recomp/`)

`recomp.py CONFIG ELF OUTDIR` writes `funcs.h`, `table.c` and `code_NNN.c`.

### Finding functions (`analysis.py`)

The retail executables are stripped. Function starts are:

- the entry point and every `JAL` target
- every word in the data sections that points into `.text` (function pointers, vtables, handler tables), except jump table
  entries
- targets of `J` and branches that leave the region they're in (tail calls)
- code a region never reaches after its end (functions only called through pointers that nothing in the data names)
- where control runs past a region's end (after a call whose delay slot is the region's last instruction)
- the config's `extra` starts; `not` removes false ones

A data word inside a switch's span of cases is never a start unless something calls it: a number that looks like an
address would otherwise split the function and cut it off from its cases.

The text is cut at the starts, each region's control flow followed from its start, and the rules repeated until nothing changes.
Jump tables (`lui`/`addiu`/`addu`/`lw`/`jr`, with the `sltiu` bound) are found by walking back from each `jr`. Hand-written
code that jumps into an unrolled block (`lui`+`addi` base plus an index) gets every instruction of the block as a target. For WoC:
3,956 functions, 136 jump tables, no unresolved `jr`.

### Translation (`codegen.py`)

- Registers live in a `Ps2Cpu` struct: 32 128-bit GPRs, HI/LO (128 bit: the R5900's second multiply pipeline uses the upper
  halves), SA, the FPU's 32 registers, ACC and FCR31. VU0's registers are the hardware layer's VU0 (`Ps2Vu0Regs` overlays it),
  so macro mode code and `VCALLMS` micro programs share them as on the console.
- Every instruction becomes one C statement. 32-bit operations sign-extend into the 64-bit register; MIPS integer arithmetic
  wraps (`-fwrapv`).
- 128-bit MMI (all of MMI0-3, PMFHL/PMTHL, QFSRV, the pipeline-1 multiplies) are functions in `runtime/src/mmi.c`.
- COP1 is IEEE single precision with the R5900's differences applied where they show: no infinities or NaNs (results clamp to
  the largest float), denormals flush to zero, `cvt.w.s` saturates, divide by zero gives the signed maximum.
- COP2 macro mode: every VU0 upper instruction (FMAC with broadcast, Q, I and ACC forms, OPMULA/OPMSUB, FTOI/ITOF, CLIP),
  the lower ones COP2 exposes (DIV/SQRT/RSQRT, the integer ALU, LQI/SQI/LQD/SQD, ILWR/ISWR, MTIR/MFIR, the R register),
  QMFC2/QMTC2/CFC2/CTC2 (CTC2 to CMSAR1 starts VU1), VCALLMS/VCALLMSR run VU0 micro code on the hardware layer's interpreter.
  The MAC and status flags aren't computed yet; the clip flags are.
- Branches: the condition is read, then the delay slot runs, then the jump (`goto` within the function). Branch-likely runs its
  delay slot only when taken. A label that lands on a delay slot gets an out-of-line copy.
- `JAL` is a C call; `JR RA` is `return`. `J`, branches and fall-through into another function are tail calls (`f(c); return;`).
  `JALR` and `JR` through other registers look the target up in the address table (`ps2_call`); a `JR` with a jump table is a
  `switch` first.
- Interrupts are taken at backward branches (`PS2_POLL`: one load and branch) and in the runtime's own calls.
- Loads and stores go through inline functions: one compare sends 0x10000000-0x1FFFFFFF (the hardware registers) to the
  runtime, everything else is a single host load or store at `ps2_mem + address`.
- `setjmp`-style and context-switching code doesn't exist in the guest: the kernel is the runtime's, so threads switch inside
  syscalls, on host threads.

### Self-modifying code and overlays

Neither is supported by the static path: the recompiled code is the executable as loaded. A game that loads code at run time
(overlays, IRX-style EE modules) needs either those files recompiled too (the same tool on each overlay, linked at its load
address) or an interpreter fallback (`ps2_call` on an address outside the table would run it). WoC has neither: `ps2_call` on an
unknown address stops with the address, which is how missing function starts are found. Code copied into kernel memory by
libkernel (its patches of the real kernel) is never run: the HLE kernel doesn't need it.

## The runtime (`runtime/`)

### Memory

The EE's 4 GB address space is a host reservation. Main memory (32 MB, one shared memory object) is mapped at 0x00000000 and at
its mirrors 0x20000000 (uncached), 0x30000000 (uncached accelerated), 0x80000000 (kseg0) and 0xA0000000 (kseg1); the
scratchpad at 0x70000000. Anything else faults, and the handler prints the guest address and the guest call stack (host stack
frames are named by guest address). Kernel memory below 0x00080000 is ordinary RAM, zeros unless the HLE kernel writes it.

### Hardware registers (`io.cpp`, `dmac.cpp`, `gs.cpp`)

- Timers T0-T3 from the host clock (bus clock 147.456 MHz and its dividers, hblank).
- DMAC: all ten channels, normal, chain (CNT, NEXT, REF, REFS, REFE, CALL, RET, END, the two-entry address stack, TTE, the
  tag IRQ with TIE) and interleave (SPR) modes; D_CTRL, D_STAT (clear/toggle semantics), D_PCR, D_SQWC. A transfer runs to its
  end when CHCR.STR is written, then the channel's interrupt is raised. VIF0/VIF1/GIF go to the hardware layer; the SIF
  channels belong to the kernel and the IOP (below). MFIFO and the VIF1 read-back (GS to memory) aren't done.
- INTC: I_STAT, I_MASK; causes raised by the GS (SIGNAL, FINISH, VSINT), the vblank timer and the DMAC.
- GS privileged registers: PMODE, SMODE1/2, DISPFB, DISPLAY, BGCOLOR, CSR (FIELD toggling per vblank, VSINT, FINISH, SIGNAL),
  IMR, SIGLBLID; drawing goes through the GIF to the hardware layer's GS.
- The VIF and GIF FIFOs (0x10004000-0x10007000) take 128-bit writes.

### The kernel (HLE, `kernel.cpp`)

The EE kernel's syscalls are done in C++: threads (Create/Start/Exit/Terminate/Delete, Sleep/Wakeup, Suspend/Resume,
priorities, RotateThreadReadyQueue, ReferThreadStatus), semaphores, INTC and DMAC handlers (add, remove, enable), alarms,
SetupThread/SetupHeap/EndOfHeap (the C runtime's start-up), SetGsCrt, GsGetIMR/GsPutIMR, SetVSyncFlag, the SIF's DMA and
registers, SetSyscall (libkernel's own syscall handlers run as guest code), and the info calls. Unknown syscalls log once and
return 0.

Each EE thread is a host thread; one holds the "processor" at a time, chosen as the EE kernel chooses (highest priority, first
come first served, no time slicing). Interrupt handlers run on whichever host thread holds the processor, with their own
register context and stack, or on the timer thread when every thread waits. EI/DI and COP0 Status's EIE are honoured.

### The IOP (HLE, `iop/`)

The EE side of the SIF (libkernel's sceSifInitCmd/BindRpc/CallRpc, libcdvd, the file stubs, libpad, libmc) runs recompiled
and unchanged. The runtime plays the IOP at the level of the SIF command protocol: packets the EE sends with SifSetDma land in
a 2 MB IOP memory; INIT_CMD, CHANGE_SADDR, SET_SREG, RESET, RPC_BIND, RPC_CALL and RPC_RDATA are answered the way the IOP's
sifcmd/sifrpc modules answer them (replies written into the EE's command buffer, then the DMAC channel 5 interrupt, one at a
time). RPC servers stand in for the modules:

| Server | Status |
|---|---|
| loadfile (0x80000006) | module loads succeed (the IOP modules themselves are never run); version calls answer the IOPRP image's release |
| iop heap (0x80000003) | allocations in the IOP memory |
| fileio (0x80000001) | open, read, lseek, close on the disc image; SDK 2.5's asynchronous completion (result written, semaphore signalled) |
| cdvd (0x80000592-0x8000059C) | init, media mode, disk type, disk ready, search file |
| padman (0x80000100) | SDK 2.x libpad: port open/close, main mode, pressure mode, actuators; port 0 has a DualShock 2 whose two frames in EE memory are rewritten at every vblank (`iop/padman.cpp`) |
| mcserv (0x80000400) | the libmc of SDKs before 2.6: init, info, open, close, seek, read, write, flush, chdir, getdir, set file info, delete, format; each port's card is a host folder (`iop/mcserv.cpp`) |
| libsd/sdrdrv | answered with zeros: no sound yet |
| a game's own modules | registered by the game (`ps2/iop.h`): WoC's NUSOUND |

The alternative, emulating the IOP's MIPS R3000 and running the real IRX modules, would bring sound and pads for free but needs an
IOP CPU, its kernel and the SPU2 timing; it can come later behind the same SIF interface.

### Graphics (`hw/`)

The Crash Twinsanity native port's hardware layer, copied with its origin noted (`hw/README.md`): the GS's software rasteriser
(checked against PCSX2 pixel for pixel on Twinsanity's screens), its OpenGL renderer, the GIF, VIF0/1 and VU0/1 interpreters with
the pipelines' timing. It is game-neutral: Twinsanity's own renderer reached it through DMA chains, as a recompiled game does.
The runtime wires it: VU1's XGKICK to the GIF, VU0's registers to the recompiled code, the GS's SIGNAL/FINISH to the CSR, and
the PCRTC's picture to the presenter at each vblank (an SDL3 window, or PNGs with `--headless --snapshots`).

### The render thread

The GS's side (the GIF's paths, the GS's registers, local memory, drawing and the PCRTC's picture) runs on a thread of its
own (`render.cpp`, from the Twinsanity port's): what reaches the GIF is copied into a queue the render thread plays back in
order, the privileged registers the PCRTC reads are answered from copies at once and set on the GS in their place in the
queue, and SIGNAL and FINISH reach the INTC when the render thread gets to them. The queue is handed over at the end of each
DMA transfer and whenever the game reads CSR, and the game's thread stays at most two pictures ahead. Every working thread
asks macOS for the performance cores: left to the default class, about a third of the time ran on the efficiency cores.
With both, WoC's 3D scenes went from 16-20 frames a second to the full 60. `PS2_RENDER_THREAD=0` keeps the GS on the
game's thread.

### VU1's programs, translated

A game's VU1 microcode is recompiled too: `recomp/vutranslate.py` reads the programs the executable's `.vutext` uploads (each
DMA packet's MPGs), and makes C++ for each block of straight code once per pipeline state it can be entered with (what's in
flight: FMAC results and their flags, Q, ILW's results, the branch read rule's old value), so the stalls and the flags'
timing are worked out at build time. `hw/ee/vu1translate.cpp` finds the program micro memory holds wherever it was uploaded,
enters the variant made for the interpreter's state, and hands the state back where there's none; the interpreter runs the
rest. The results are the interpreter's bit for bit: `PS2_VU1=verify` runs both and compares every XGKICK's packet, and
`PS2_VU1=interpret` turns the translation off. For WoC: 16 programs, 9,184 instructions, 99.6% of those run translated.

### Controllers and memory cards

Port 0's controller is the keyboard and mouse, and the first gamepad SDL3 finds (`input.cpp`). The keyboard and mouse defaults
are the Crash Twinsanity native port's PC controls: W A S D move (a stick, diagonals as a stick's), Space is cross, the mouse's
left button or J square, Left Shift or K circle, Tab or I triangle, Z and C the shoulders, Escape or Return start, Left Ctrl
toggles walking, Q and E the right stick, and the arrows the d-pad; every one can be changed in the settings folder's
`bindings.txt` (written with the defaults the first time). A game can take typed text (its `typing` hook): the keyboard then
types instead (`ps2_take_typed`), and the game's `vblank` hook puts it in its memory. Presses can be scripted with `--press
30:start,31:cross` and typing with `--type 41:NAME<enter>` for headless runs. Scripted presses and
`--snapshot-every`/`--snapshot-at` count the game's time (its vblanks), not the wall clock's, so a script lands on the same
frames however fast the host draws and loads. The memory cards are folders, `port0` and `port1`, in `--memcards DIR` (by default
`~/Library/Application Support/PS2 Native Runtime/<executable>/memcards`): port 0's card is always in, port 1's when its folder
exists. A save is the game's own directory of files, as on the card.

### Debugging

`--watch ADDR[:WORDS],...` logs guest memory every `--watch-every` seconds, `--fps` the game's frames and the vblanks of each
second; `--log` turns on the channels' logs.

### Not done yet

Sound (SPU2 and the game's sound driver), rumble, the memory card's newer protocol (libmc 2.6 on), movies (IPU), MFIFO, the VU flags in macro mode, the FPU's
exact rounding, and the OpenGL renderer's hookup (the software GS draws).

## Repositories and licences

- `ps2-native-runtime` (this repository): the recompiler, the runtime and the hardware layer. Game-neutral. Licence to be
  decided by the owner; `hw/` came from the Twinsanity native port (same owner).
- One repository per game (`crash-woc-mac`): its recompiler config, its IOP servers, fixes and overrides, the app. Its
  game-specific work is the owner's own code.
- No game data in any repository: executables and generated C stay in build folders, read from the user's own disc at build time.
