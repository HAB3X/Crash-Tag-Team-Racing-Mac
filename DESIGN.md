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
  entries and words landing where the code before runs on into them (a number that looks like an address: compiled functions
  begin after a jump, a return, padding, or a call that doesn't return followed by a prologue)
- addresses code builds with `lui` and `addiu`/`ori` (a thread's entry, a callback), where a function can begin
- targets of `J` and branches that leave the region they're in (tail calls)
- code a region never reaches after its end (functions only called through pointers that nothing in the data names)
- where control runs past a region's end (after a call whose delay slot is the region's last instruction)
- the config's `extra` starts; `not` removes false ones

A data word inside a switch's span of cases is never a start unless something calls it: a number that looks like an
address would otherwise split the function and cut it off from its cases.

The text is cut at the starts, each region's control flow followed from its start, and the rules repeated until nothing changes.
Jump tables (`lui`/`addiu`/`addu`/`lw`/`jr`, with the `sltiu` bound) are found by walking back from each `jr`. Hand-written
code that jumps into an unrolled block (`lui`+`addi` base plus an index) gets every instruction of the block as a target. For WoC:
3,885 functions, 136 jump tables, no unresolved `jr`; for CTTR, 23,210.

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
- `ERET` is a jump to ErrorEPC (with ERL on) or EPC, which the runtime keeps from `MTC0`: back to the caller when that's the
  return address in RA (Crash Tag Team Racing's libkernel writes timer 2 from kernel mode that way: ERL on, its return
  address in ErrorEPC, the store, ERET), else to the function starting there. Status's EIE is EI and DI's, and interrupts
  wait while its EXL or ERL is on.
- Interrupts are taken at backward branches (`PS2_POLL`: one load and branch) and in the runtime's own calls.
- Loads and stores go through inline functions: one compare sends 0x10000000-0x1FFFFFFF (the hardware registers) to the
  runtime, everything else is a single host load or store at `ps2_mem + address`.
- `setjmp`-style and context-switching code doesn't exist in the guest: the kernel is the runtime's, so threads switch inside
  syscalls, on host threads.

### Overlays, stripped executables and self-modifying code

- **Overlays.** A game that loads code from files at fixed addresses (Biohazard Outbreak's `BIN/n.DAT`, each the `MWo3`
  header then code and data, loaded whole) lists them in its config (`[[overlays]]`: the file on the disc, where it's
  loaded, its code's range). `recomp.py` reads them from the disc image and recompiles each beside the executable, its
  functions named `<overlay>_XXXXXXXX`. Calls within an overlay and from it into the executable are direct; anything into
  an overlay goes through `ps2_call`. Several overlays share addresses, so `table.c` lists every overlay function with its
  first four words, and every overlay with up to eight words no other overlay over the same addresses holds. `ps2_call`
  runs the function whose first words are in memory, and when two overlays have the same words there, the one whose check
  words are too. A call into an overlay's memory that isn't a function start stops, naming the overlay.
- **Executables with no section table.** The config gives the code's ranges (`[executable] text`) and the VU microcode's
  (`vutext`, which `vutranslate.py` then reads as it would a `.vutext` section). Gaps between the ranges are holes: no
  function starts there, and they're searched for pointers as data (the microcode isn't).
- **Data kept in the text.** An unreached run of words, or a pointer's target, where anything that would run doesn't decode
  is a table, not a function (Outbreak's libkernel keeps its syscall patch tables between its functions).
- **Self-modifying code** isn't supported by the static path: the recompiled code is what's on the disc. An overlay marked
  `encrypted` (Outbreak's DNAS modules decrypt parts of themselves as they run) still recompiles; its words that don't
  decode become `ps2_undecodable`, which stops the game if one is reached. Code copied into kernel memory by libkernel (its
  patches of the real kernel) is never run: the HLE kernel doesn't need it. `ps2_call` on an address no table has stops with
  the address, which is how missing function starts are found.

## The runtime (`runtime/`)

### Memory

The EE's 4 GB address space is a host reservation. Main memory (32 MB, one shared memory object) is mapped at 0x00000000 and at
its mirrors 0x20000000 (uncached), 0x30000000 (uncached accelerated), 0x80000000 (kseg0) and 0xA0000000 (kseg1); the
scratchpad at 0x70000000. Anything else faults, and the handler prints the guest address and the guest call stack (host stack
frames are named by guest address). Kernel memory below 0x00080000 is ordinary RAM, zeros unless the HLE kernel writes it.

### Hardware registers (`io.cpp`, `dmac.cpp`, `gs.cpp`)

- Timers T0-T3 from the host clock (bus clock 147.456 MHz and its dividers, hblank), with ZRET; reaching COMP and overflowing
  set MODE's flags (cleared by writing ones) and raise the INTC's timer causes where MODE asks, the kernel's timer thread waking
  for the next one (libkernel 2.7's system time counts timer 2's overflows: CTTR's legal screen waited on it forever).
- DMAC: D_CTRL's DMAE honoured (on at the start, as the kernel leaves it): a transfer started or going on while it's off
  waits for it. All ten channels, normal, chain (CNT, NEXT, REF, REFS, REFE, CALL, RET, END, the two-entry address stack, TTE, the
  tag IRQ with TIE) and interleave (SPR) modes; D_CTRL, D_STAT (clear/toggle semantics), D_PCR, D_SQWC. A transfer runs to its
  end when CHCR.STR is written, then the channel's interrupt is raised. VIF0/VIF1/GIF go to the hardware layer; the SIF
  channels belong to the kernel and the IOP (below). The MFIFO (D_CTRL's MFD, D_RBOR, D_RBSR): fromSPR writes into the ring,
  VIF1 or GIF started in chain mode drains it (tags and their data read in the ring, wrapping; REF's data outside it) as far as
  fromSPR has filled it, then stays busy with D_STAT's MEIS set until fromSPR adds more (CTTR's renderer). The VIF1 read-back
  (GS to memory) isn't done.
- INTC: I_STAT, I_MASK; causes raised by the GS (SIGNAL, FINISH, VSINT), the vblank timer, the DMAC and the VIFs.
- A VIF code's interrupt bit stalls the VIF once the code is done and raises its INTC cause: the DMA transfer feeding it stops
  where it is (still busy; the words of a quadword it didn't take wait in its FIFO) until the game cancels the stall
  (VIFn_FBRST's STC), then goes on. VIFn_STAT shows the stall, VIFn_MARK the MARK code's value (or the EE's own), VIFn_CODE the
  last code. CTTR's renderer uploads each texture
  from its VIF1 interrupt handler, between the draws that need it.
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

The vertical blank comes from the timer thread at 59.94 Hz whatever the game is doing, as the TV's does: a game counts them
for its clock (WoC runs as many steps of play in a frame as blanks went by). Its edges (each blank's start and end) are
counted, not flagged: a holder kept from its poll points for longer than a field takes the ones it missed one after another,
each with its handlers, instead of as one (which slowed the game's clock down with the host). The game's thread waiting for
the render thread (below) waits at a poll point, the timer thread running the handlers meanwhile, so the blanks reach the game
as they come: taken late, all at once, a game's swaps at their ends would be just past and let frames through without one.

### The IOP (HLE, `iop/`)

The EE side of the SIF (libkernel's sceSifInitCmd/BindRpc/CallRpc, libcdvd, the file stubs, libpad, libmc) runs recompiled
and unchanged. The runtime plays the IOP at the level of the SIF command protocol: packets the EE sends with SifSetDma land in
a 2 MB IOP memory; INIT_CMD, CHANGE_SADDR, SET_SREG, RESET, RPC_BIND, RPC_CALL and RPC_RDATA are answered the way the IOP's
sifcmd/sifrpc modules answer them (replies written into the EE's command buffer, then the DMAC channel 5 interrupt, one at a
time). RPC servers stand in for the modules:

| Server | Status |
|---|---|
| loadfile (0x80000006) | modules load from the disc, from a buffer in the IOP's memory, or rom0 (none of them runs: each is known by its own name, its `.iopmod`'s); a name finds a loaded one; a game's own module (`IopRegisterModule`) is started by name with its arguments; version calls answer the IOPRP image's release |
| iop heap (0x80000003) | FILEIO's heap server (its functions 1-9 read from IOPRP280's FILEIO): allocations as AllocSysMemory's modes (0 when there's no room), frees, a file loaded into the IOP's memory, sysmem's size, free and block queries; first fit from 64 KB up (no module of the console's takes room). Each bound server has a 32 KB receive buffer and no client buffer, as the modules register |
| fileio (0x80000001) | open, read, lseek, close on the disc image; SDK 2.5's asynchronous completion (result written, semaphore signalled) |
| cdvd (0x80000592-0x8000059C) | init, media mode, disk type, the last error, the tray, disk ready (both libcdvd's), search file, reads |
| padman (0x80000100) | SDK 2.x libpad: port open/close, main mode, pressure mode, actuators; port 0 has a DualShock 2 whose two frames in EE memory are rewritten at every vblank (`iop/padman.cpp`) |
| mcserv (0x80000400) | the libmc of SDKs before 2.6: init, info, open, close, seek, read, write, flush, chdir, getdir, set file info, delete, format; each port's card is a host folder (`iop/mcserv.cpp`) |
| dbcman (0x80001300, 0x8000131B, 0x8000131C) | SDK 2.8's Dbc_Manager, the IOP side of libdbc, which libpad2, libmc2 and the game itself use instead of libpad and libmc (Biohazard Outbreak): its sockets and devices, and its drivers: ds2o_s1/ds2u_s1's DualShock 2 on port 0, mc2_s1's memory card in slot 1 (`iop/dbcman.cpp`, `ds2.cpp`, `mc2.cpp`; below) |
| sdrdrv | libsd's RPC server (sceSdRemote): init, the parameters, switches, addresses and core attributes on the runtime's libsd; the reverb (sceSdSetEffectAttr: its sceSdEffectAttr's words are core, mode, the depths' halfwords, delay, feedback) and its work area's clearing, as LIBSD.IRX does them; the transfers not done (logged) |
| CRI_ADX_Driver | CRI's ADX IOP module (`iop/cri_adx.cpp`, `cri_psm.cpp`, `cri_sj.cpp`; docs/cri_adx.md), known by its name and started as it loads: DTX's RPC server (0x90000200) and its mailboxes with the EE (exchanged at each vertical blank on the game's side), the SJs (stream joints) the EE makes and feeds through SJX, the streams' commands (IOPRNA), and PSM: each stream's 16 bit PCM converted to 48 kHz, mixed and played through an SPU2 core's input (`SdBlockTransLoop`) as the module does. Biohazard Outbreak's two discs |
| a game's own modules | registered by the game (`ps2/iop.h`): RPC servers (WoC's NUSOUND, which drives the SPU2 through libsd, `ps2/sound.h`, below), modules started by name (CTTR's Bink IOP side), a module's thread at each vertical blank (`IopOnVblank`, run on the game's side so it may write the EE's memory as the IOP's DMA does) and their own SIF commands both ways (`IopRegisterCommand`; `IopSendCommand`, with data sent ahead into the EE's memory, from any thread: a command waits in an outbox while the SIF's lock is taken, and goes at the next SIF exchange or vblank) |

The alternative, emulating the IOP's MIPS R3000 and running the real IRX modules, would bring sound and pads for free but needs an
IOP CPU, its kernel and the SPU2 timing; it can come later behind the same SIF interface.

### Sound (`sound/`)

The SPU2 is emulated at its registers (`sound/spu2.cpp`, from the Crash Twinsanity native port, which also emulates the IOP and
runs the real modules there; copied with its origin noted in `spu2.h`): two cores of 24 voices playing PS-ADPCM out of 2 MB of
sound memory, the blocks' loop flags, the gaussian interpolation, the pitch counter, ADSR envelopes and volume sweeps, key on
and off, ENDX, the reverb, the cores' mixing. A game's sound driver, standing in for its IOP module as an RPC server, drives it
the way the module would through libsd (`ps2/sound.h`; `sound/libsd.cpp` makes the register writes LIBSD.IRX makes, read from
its code: each entry's register, the address registers' halves, sceSdInit's set-up) and transfers sample data straight into the
sound memory (sceSdVoiceTrans, done at once).

The output (`sound/output.cpp`) is 48 kHz stereo through an SDL3 audio stream whose callback makes the samples SDL asks for, on
SDL's audio thread; with no device (`--headless`, `--mute`) a thread of its own makes them at the wall clock's pace. A driver's
own thread (the module's, woken by the IOP's hardware timer) is a function the runtime runs every so many samples
(`SoundSetTimer`; another module's alarms `SoundAddTimer`), so it sees the voices where they are in the sound it's making.
libsd's reverb (`SdSetEffectAttr`, `SdClearEffectWorkArea`) is LIBSD.IRX's: the effects off while the registers change, the
preset's registers (its offsets in 8 byte units written as halfword addresses, the echo and delay presets' moved by their
delay), the work area's start below the core's end address (EEA) by the preset's size, the area zeroed when asked. The ten
presets and their sizes are the game's own LIBSD.IRX's, taken from the module as it's loaded (found in its data by their
shape; SDK 2.5's and 2.8's hold the same values): none in the repository. `SdBlockTransLoop` is sceSdBlockTrans's looping
write into a core's input (AutoDMA): a buffer in the IOP's memory played a half at a time, the transfer's interrupt handler run as each half has been taken (CTTR's movies' sound). The SPU2 and the driver's state belong to
whoever holds the sound lock: the making of a few hundred samples at a time, or the RPC server answering a call on the game's
thread; neither waits for anything else. `--audio-dump FILE.wav` writes everything made to a WAV file.

### Graphics (`hw/`)

The Crash Twinsanity native port's hardware layer, copied with its origin noted (`hw/README.md`): the GS's software rasteriser
(checked against PCSX2 pixel for pixel on Twinsanity's screens), its OpenGL renderer, the GIF, VIF0/1 and VU0/1 interpreters with
the pipelines' timing. It is game-neutral: Twinsanity's own renderer reached it through DMA chains, as a recompiled game does.
The runtime wires it: VU1's XGKICK to the GIF, VU0's registers to the recompiled code, the GS's SIGNAL/FINISH to the CSR, and
the PCRTC's picture to the presenter at each vblank (an SDL3 window, or PNGs with `--headless --snapshots`).

### The picture (`display.cpp`)

The GS draws on the GPU: the hardware layer's OpenGL 3.3 renderer (`hw/hw/hwgs.*`) takes each primitive from the GS's front end
and draws it into render targets at a multiple of the PS2's resolution (3x by default: a 640x224 field is drawn at 1920x672),
keeping them and the GS's local memory in step page by page; what it can't draw exactly is left to the software rasteriser.
It runs on the render thread with an OpenGL context of its own, made on the main thread (SDL's windows are the main thread's)
sharing the window's textures. At each vblank the render thread copies the shown picture into one of three textures and
fences the copy; the window (its own context, on the main thread) draws the newest one at 4:3, letterboxed, once the fence is
passed, and fences its drawing so the render thread doesn't copy into a texture still being drawn (the Crash Twinsanity port's
hand-over). Snapshots read the picture back at the internal resolution. Headless, the context has a hidden window.

The settings: `--gs hardware|software`, `--scale 1-8` and `--texture-upscale 0|2|4` (the textures decoded from the game's data
upscaled on the GPU, a bicubic pass), or `$PS2_GS`, `$PS2_GS_SCALE`, `$PS2_TEXTURE_UPSCALE`, or `graphics.txt` in the settings
folder (written with the defaults the first time). The software GS (the PS2's own rules at its resolution) draws when it's asked
for, when there's no OpenGL 3.3 and with `PS2_RENDER_THREAD=0` (the GPU's context belongs to the render thread).

Primitives that read the buffer they draw into (WoC's Warp Room draws about 300 a frame) read one copy of it made when their
batch began, as PCSX2's hardware renderers do, instead of a copy and two render passes each: the Warp Room went from 10-20
frames a second at 3x to 60.

WoC's levels blur the far scenery by its depth: the depth buffer is copied into another buffer's layout and read back as
8 bit texels a pixel at a time (each a byte of another pixel), spread into a blur buffer's alpha by sprites that read the
buffer they draw into, and blended over the frame by it. On the GPU that needs the GS's exact rules where upscaling would
otherwise bend them: texel coordinates on the GS's side of a texel's edge, every sample of an upscaled target reading the
texel its pixel's sample point reads and sprites covering whole pixels' blocks (data at the PS2's resolution read back from
an upscaled target), no second pass for an alpha test whose failing pixels are written as passing ones, and no texture
filtering setting on such data. The effect's buffers are the software GS's byte for byte, at 1x and 3x. Its cost was in
render passes and waits for the GPU: a decode of the 8 bit view for each strip of the buffer drawn, and the textures uploaded
each frame over the effect's buffers read back first. The views are decoded whole, a draw reading its own buffer reads the
decode made when its batch began, and an upload over pages the GPU holds newer is put into memory's copy on the GPU
(`MergeWrites`) instead of read back; Arctic Antics went from 11 frames a second to 60.

CTTR's park (a 30 frames a second game there: its frame sync waits for a second vblank in that mode, 60 in its menus) drew
at 13. Its shadow volumes count into a 16 bit buffer with a wrapping additive blend that writes the alpha bit (0): summed
like WoC's, which don't write it, instead of a copy of the target and two render passes for each of ~400 batches a frame
(24 frames a second). Its glow reads the buffer it draws into as 8 bit texels through a texture's region repeat (about
1,000 channel-shuffle sprites) and adds it to the frame with a region clamp: the shader wraps each texel and filters as
the GS does (`RegionSample`), where the software GS drew them with about ten GPU readbacks a frame (28 frames a second).
Both left every picture the same, the glow's buffer too, at 1x and 3x (`--gs-replay` of the park, below).

A game that draws each field of an interlaced picture on its own (SMODE2's INT and FFMD; WoC does) moves its drawing half a
line on every other field with XYOFFSET (the SDK's sceGsSetHalfOffset), for a TV that shows the fields half a line apart.
Shown one field after another in the same place, the picture shook by half a line at 30 Hz: the GS drops that half line
(`Gs::steadyFields`), so both fields are drawn alike. `PS2_STEADY_FIELDS=0` keeps it.

### The render thread

The GS's side (the GIF's paths, the GS's registers, local memory, drawing and the PCRTC's picture) runs on a thread of its
own (`render.cpp`, from the Twinsanity port's): what reaches the GIF is copied into a queue the render thread plays back in
order, the privileged registers the PCRTC reads are answered from copies at once and set on the GS in their place in the
queue, and SIGNAL and FINISH reach the INTC when the render thread gets to them. The queue is handed over at the end of each
DMA transfer and whenever the game reads CSR, and the game's thread stays at most two pictures (vertical blanks) ahead: it
waits for the render thread after the blank's handlers, the blanks going on meanwhile (above), and a blank taken where no
game thread runs drops its picture instead when the render thread is that far behind. Every working thread
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
`PS2_VU1=interpret` turns the translation off. For WoC: 16 programs, 9,184 instructions, 99.6% of those run translated
(99.8% in Arctic Antics, whose snow's program clears the status flag's sticky bits with FSSET and tests them with FSAND: both
translated where the translation's sticky bits are the interpreter's, `Sim.ready_for`). A game whose programs are bare code
that its own DMA chains upload (CTTR: REF tags into `.vutext`) names them in its config (`[vu1] programs`: each one's EE
addresses and the micro address it's uploaded at): CTTR's ten, 3,961 instructions, about 98% of those run translated in its
park (10 frames a second to 30). FCSET is translated (its clip flag through the FMAC pipeline, as CLIP's), and a branch with
the E bit (the run ends after its delay slot; the next starts where it branched, an entry of its own). A block has up to 64
variants: CTTR's main program's loops settle at 49 pipeline states, and with 32 the rest ran interpreted (its park's
instructions 98.5% translated then, 99.3% now).

### Controllers and memory cards

Port 0's controller is the keyboard and mouse, and the first gamepad SDL3 finds (`input.cpp`). The keyboard and mouse defaults
are the Crash Twinsanity native port's PC controls: W A S D move (a stick, diagonals as a stick's), Space is cross, the mouse's
left button or J square, Left Shift or K circle, Tab or I triangle, Z and C the shoulders, Return start, Left Ctrl toggles
walking, Q and E the right stick, and the arrows the d-pad; Escape opens the PC menus (below). Every one can be changed in the
PC menus' Controls or in the settings folder's `bindings.txt` (written with the defaults the first time). A game can take typed text (its `typing` hook): the keyboard then
types instead (`ps2_take_typed`), and the game's `vblank` hook puts it in its memory. Presses can be scripted with `--press
30:start,31:cross` and typing with `--type 41:NAME<enter>` for headless runs. Scripted presses and
`--snapshot-every`/`--snapshot-at` count the game's time (its vblanks), not the wall clock's, so a script lands on the same
frames however fast the host draws and loads. The memory cards are folders, `port0` and `port1`, in `--memcards DIR` (by default
`~/Library/Application Support/PS2 Native Runtime/<executable>/memcards`): port 0's card is always in, port 1's when its folder
exists. A save is the game's own directory of files, as on the card.

Biohazard Outbreak (SDK 2.8 and 3.0) uses the newer libpad2, libmc2 and libdbc instead of libpad and libmc. They all go through
the Dbc_Manager on the IOP (dbcman.irx), which the runtime stands in for at its RPC protocol, the EE's libraries running
unchanged (the game calls libdbc itself too). dbcman keeps up to 16 devices, each registered by a driver module and connected
when the driver finds its hardware (a type: 1 a pad, 2 a memory card; a port and slot; a name), and up to 16 sockets made by the
EE (the same, with flags saying which of them must match, and two buffers in the EE's memory), and links each socket to a device
that matches it. The sockets' connection words (1 linked) go to a work area the EE gives it (SetWorkAddr). A call on a socket
(ReceiveData, SRData, SendData; SendData2 and SendData3 on servers of their own, with room for 4 and 8 KB, called without
waiting) goes to its device's driver with a command (a class in its top byte, a group, a code), and a driver sends data into
the socket's buffers. The RPC blocks' layouts are in `iop/dbcman.cpp`, taken from the module.

- The DualShock 2 (`iop/ds2.cpp`; ds2o_s1 in File 2, ds2u_s1 in File 1, the same where dbcman sees them): the driver keeps the
  pad in its pressure mode (0x79) and connects it as port 0. At every vblank it sends a 128 byte frame into the linked socket's
  buffer n % 2, n the frame's number at its end (libpad2 takes the newer of the two): the state, the data's length, the pad's
  answer (buttons, sticks, the twelve pressures: padman's, from the same input) and the button profile (which bits and bytes of
  the data the mode has). ReceiveData asks for the mode, the state, the profile and the actuators; SendData sets the actuators
  (rumble isn't done: the call is answered as done).
- The memory card (`iop/mc2.cpp`; mc2_s1 3020, File 2's): mc2_s1 is a page device. The card's file system (directories, files,
  the FAT, the icons, formatting) is libmc2's, on the EE, which sends the driver page reads (their ECC checked), page writes
  (each block rewritten through two backup blocks, as the driver does it, so a write cut short can be finished), block erases
  and copies, and asks for the card's size and the clock. So the card can't be a folder of files as mcserv's are: it's a card
  image, `port0.ps2` in the memory cards' folder, 16384 pages of 528 bytes (data and spare area with the ECC), as PCSX2's .ps2
  cards are laid out. A new one is blank (all FF), and the game offers to format it. File 1's mc2_s1 (2811) has another
  protocol (12 commands, other checks): it's refused at load (logged), and File 1 sees no card.

### The PC menus (`runtime/src/ui/`)

The Crash Twinsanity native port's PC menus (its `ui/` there: the overlay, the options screen's look and logic, the prompts,
the mouse's art), copied with their origin noted in each file and made game-neutral. Escape (any key bound to `menu` in
bindings.txt) or a gamepad's Back button opens the pause menu: Resume, Options, Extras (when the game has any), the game's own
pause actions, Quit (a box asks first). Options has tabs: Gameplay (pause in the background), Display (windowed or fullscreen,
the window's size, the internal resolution 1x-8x, texture filtering, texture upscale, anti-aliasing: FXAA or supersampling 2x
and 4x, the CRT filter, v-sync; a change of the window asks to be kept, going back after 15 s), Sound (the master volume, mute
in the background), Controls (the button prompts' style, what a gamepad's Back is, every action of bindings.txt with its two
keys rebindable by key or mouse button and the gamepad's control shown), then the game's own tabs. A game's settings and actions
(`ps2/runtime.h`) join the tabs they name. Cmd+F and Alt+Enter switch fullscreen at any time. A game that keeps its own pause
menu (`Ps2GameInfo::ownPause`) has no pause menu of the runtime's: the menu key is its Start, a gamepad's Back its Select, the
game opens the Options itself (`ps2_open_settings`, e.g. from its own options menu) with an Extras tab for its Extras and pause
actions, going back from them goes back to the game, and the game just waits while the window isn't in front.

- The pause: `KernelPause` stops the thread holding the processor at its next poll point (it waits in the kernel's service,
  between instructions), and the timer thread makes no vblanks or alarms (`BusCycles`, the game's clock, stands still); when no
  thread holds the processor the timer thread waits instead. Work for the game's side (`KernelOnGameSide`: a game's setting's
  `set`, an action's `run`, the GS's settings put in the render thread's queue) is done by whichever of them is waiting, so a
  change shows at once and the game's code is only ever run where it would be. The sound's output stops with it (the driver's
  timer is the SPU2's samples). What was held as the menus close counts again only once it's let go.
- The picture: the window keeps the copy it drew last, which the render thread doesn't copy into, and draws it again under the
  menus while the game is paused (the presenter turns every few milliseconds then, v-sync pacing it).
- The overlay is drawn by the presenter over the picture at the window's own resolution, in a 1280 x 720 reference space
  scaled to fit and centred: rounded boxes as signed distances, the fonts (Luckiest Guy and Fredoka, `runtime/fonts`, built into
  the program) baked into atlases per size with stb_truetype. The mouse's places are hit regions recorded as each frame is drawn.
- The settings: the display's in `graphics.txt` (as before, with the new ones added), the rest and the game's own in
  `options.txt`. `--settings DIR` uses another folder (tests).
- The game's prompt (`ps2_show_prompt`) is drawn near the picture's bottom in the menus' style while the game plays, fading
  in as it's shown (a quarter of a second) and out as it's taken down (half a second); headless pictures show it as it stands.
- `--ui S:KEY,...` drives the menus from a time of the game's: keys (esc, enter, up, down, left, right, tab, prevtab, q, e,
  back, delete, space), `mouse:XxY`/`click:XxY` in reference pixels, `wheelup`/`wheeldown`, `pad-back`, `wait`, and `shot`,
  which saves the window with the menus as `uiNN.png` in the snapshots' folder (the window's pixels; headless, a hidden
  1280 x 960 window's, with nothing animating). One key a third of a second, by the clock while the game is paused.

### Debugging

`--watch ADDR[:WORDS],...` logs guest memory every `--watch-every` seconds, `--fps` the game's frames and the vblanks of each
second (also the window's title); `--log` turns on the channels' logs; `--audio-dump FILE.wav` keeps the sound. A frame is a
vblank period in which the game presented a picture: an enabled circuit's DISPFB changed (WoC flips between two buffers) or
something was drawn or transferred into the buffer shown (CTTR copies each finished picture into one display buffer it
never moves, which DISPFB alone counted as no frames at all). It's counted on the GS's side, in order with what the game sent.

`--gs-record S:N:FILE` records the GS's side for N vblanks from the game's S-th second: the GS's and the GIF's state (local
memory made current from the GPU first), then the GIF's data and VU1's kicks in order and the PCRTC's registers at each vblank
(`gsrecord.cpp`; with the render thread). `--gs-replay FILE[:PASSES]` plays one back through the GS alone in place of the game,
logging how long its vblanks took to draw; with `--snapshots` it writes each vblank's picture's hash to `replay.txt` there and
saves the pictures `--snapshot-at`/`--snapshot-every` name by the recording's own times. The same recording drawn by two builds
gives the same hashes when nothing that shows changed (a game's own pictures differ from run to run with the frames it skips),
and the renderer's speed is measured without the game's.

### Not done yet

The SPU2's DMA timing, rumble, the
memory card's newer protocols (libmc 2.6 on; libmc2 with File 1's mc2_s1 2811), movies (IPU), the VU flags in macro mode, the FPU's exact rounding.

## Repositories and licences

- `ps2-native-runtime` (this repository): the recompiler, the runtime and the hardware layer. Game-neutral. Licence to be
  decided by the owner; `hw/` came from the Twinsanity native port (same owner).
- One repository per game (`crash-woc-mac`): its recompiler config, its IOP servers, fixes and overrides, the app. Its
  game-specific work is the owner's own code.
- No game data in any repository: executables and generated C stay in build folders, read from the user's own disc at build time.
