# The hardware layer

The PS2's graphics hardware, emulated: the GS (`gs/`, a software rasteriser with the GS's own rules, and `hw/`, the OpenGL
renderer), the GIF's three paths (`ee/gif.*`), VIF0 and VIF1 (`ee/vif.*`), VU0 and VU1 in micro mode (`ee/vu.*`), and the
dispatcher of VU1's translated programs (`ee/vu1translate.*`, `ee/vu1jit.h`; the translator is `recomp/vutranslate.py`).

These files come from the Crash Twinsanity native port (`src/platform/native/graphics/` in twinsanity-native, branch
`features`, commit 31f884080a447446ab02f2b9a741e7eecf1a49aa of 2026-10-13), where they were written for that port. They are copied
here unchanged except for:

- `common.h` is this repository's own (the integer and float type names)
- `hardware.h` and `native.h` are this repository's own, in place of the port's: the set of hardware the OpenGL renderer finds
  the GS through (`NativeGraphics::GetHardware`) and the settings folder its replacement textures are under
  (`Native::SettingsFolder`), both answered by the runtime (`runtime/src/display.cpp`)
- the port's DMA chain runner (`ee/dma.*`) and its 64 bit host address slots (`ee/address.*`) are left out: a recompiled game
  has real PS2 addresses, and the runtime's DMA controller (`runtime/src/dmac.cpp`) runs the channels
- `gs/gs.h` and `gs/gs.cpp`: `Gs::eventHook`, told of SIGNAL, FINISH and LABEL writes (the runtime's CSR and the GS interrupt)
- `gs/gsdraw.cpp`: `Wrap` keeps the region clamp's and region repeat's texel inside the decoded level (a region past the
  texture's size read past it)
- `ee/vu1translate.*`, `ee/vu1jit.h` and the translator (`native/tools/vu1_translate.py` and `vudecode.py` there,
  `recomp/vutranslate.py` and `recomp/vudecode.py` here), copied later from the same commit: the programs are read from the
  executable's `.vutext` packets instead of Twinsanity's program list, the settings are `$PS2_VU1` and `$PS2_VU0`, VU0's
  tables are empty, and the translation follows the `vu.cpp` change below
- `ee/vu.cpp`: LQI, LQD, SQI and SQD's increments and decrements keep the register's old value for the branch read rule, as
  the integer ALU's writes do (Wrath of Cortex's model program loops forever otherwise)
- `gs/gs.h` and `gs/gs.cpp`: `Gs::steadyFields`, field rendering's odd fields drawn without their half line (XYOFFSET's OFY
  half a line past a whole one taken as the whole one while SMODE2 is interlaced and in field mode): shown one field after
  another on a computer's screen, the half line made the picture shake

- `hw/hwgs.cpp`: the primitives of a batch that read the target they draw into read one copy of it made when the batch began
  (`selfCopyFor_`, as PCSX2's hardware renderers do), instead of a copy of the whole target and two render passes for each
  primitive; WoC's Warp Room has about 300 such primitives a frame and went from 10-20 frames a second at 3x to 60.
  `TWIN_HW_EXACT_FEEDBACK=1` copies for each primitive again
- `gs/workers.cpp`: the workers ask macOS for the performance cores (QoS user interactive): rows are shared out evenly, so
  a worker on an efficiency core held every primitive up
- `hw/hwgs.cpp`, for Wrath of Cortex's depth-of-field effect in its levels (stripes across the far scenery, 11 frames a
  second): texel coordinates nudged a 256th of a texel off the edges the GPU's interpolation falls short of; unfiltered
  textures decoded from upscaled targets read at the GS's sample point by every sample, their sprites covering whole
  blocks (`BitNativeTexel`), and left unfiltered by the filtering setting; no second pass for an alpha test whose AFAIL
  writes all a pass writes; big textures over pages memory's copy has from a target decoded whole; a draw reading its own
  target as other texels reads the decode made when its batch began (`readsBatchStart_`); a decoded texture's region walked
  for its pages only when the whole texture has upscaled pages (`LevelPages`); a transfer into pages the GPU holds newer
  merged into memory's copy on the GPU (`BeforeCpuWriteRect`, `MergeWrites`) and a CPU read of a rectangle the CPU wrote
  there since not read back (`BeforeCpuReadRect`), targets loading the merged pages together, and textures there found by
  their content (`CpuWroteLevels`)
- `gs/backend.h`, `gs/gs.cpp`: `Backend::BeforeCpuWriteRect` and `BeforeCpuReadRect` (the rectangle a transfer writes or a
  read reads); a CLUT's load prepares only the pixels it reads
- `hw/gl.*`: `Uniform4i`
- `ee/vu.*`, `ee/vu1translate.cpp`, `recomp/vutranslate.py`: VU1's FSSET translated, and FSAND of the sticky status bits
  where the translation's are the interpreter's (`startStatus` carries FSSET's bits, `anyEntry` is an entry since the
  start or FSSET; the dispatcher doesn't enter with FSSET in flight)

Fixes made in either place should be copied to the other.
