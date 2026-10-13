# The hardware layer

The PS2's graphics hardware, emulated: the GS (`gs/`, a software rasteriser with the GS's own rules, and `hw/`, the OpenGL
renderer), the GIF's three paths (`ee/gif.*`), VIF0 and VIF1 (`ee/vif.*`), VU0 and VU1 in micro mode (`ee/vu.*`), and the
dispatcher of VU1's translated programs (`ee/vu1translate.*`, `ee/vu1jit.h`; the translator is `recomp/vutranslate.py`).

These files come from the Crash Twinsanity native port (`src/platform/native/graphics/` in twinsanity-native, branch
`features`, commit 31f884080a447446ab02f2b9a741e7eecf1a49aa of 2026-10-13), where they were written for that port. They are copied
here unchanged except for:

- `common.h` is this repository's own (the integer and float type names)
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

Fixes made in either place should be copied to the other.
