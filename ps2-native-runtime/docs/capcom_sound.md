# Capcom's IOP sound driver (Biohazard Outbreak)

Biohazard Outbreak (File 1) and File 2 load the same sound driver of Capcom's own, the ninth-or-so module of
`MODULES/DRIVE.DAT` (module id 10 in the load order): an IRX with an empty `.iopmod` name, built from tsnddrv.c and tsnddef.c
("SoundDriver : Ver%d.%d (%s %s) (C)CAPCOM", Oct 24 2003); its `.symtab` names every function. It sits on Sony's
modhsyn and modmsin (`ps2/hsyn.h`) and libsd (`ps2/sound.h`); the EE asks it through two RPC servers. The game's repository
stands in for it natively (`src/tsnd.cpp`, started by `IopRegisterUnnamedModule` as the loader sees the string), the offsets
below being the module's.

## Start (TsndLoop 0x48, TsndInitModule 0xC18)

libsd and both cores set up (`spucore_init` 0x27C: core end address EEA 0x1FFFFF, effects enabled, reverb preset 0x100 with
no depths, effect-return volume 0, master volume 0x3FFF both sides; core 1's mix 0xFFCF and its external input volumes
0x7FFF), modhsyn started with 14 ports and a 0x1047 microsecond tick (`sceHSyn_Init`), each port's volume 0x100, 64 bytes of
silence at sound memory 0x5080 whose first block loops on itself (`TsndInitDma` 0xA40), and a hard timer every 200 samples
(0x25800 cycles of 36.864 MHz) that runs `ATick` (0x107C): each ready port's channel states, the sequences, then modhsyn's
tick. The two servers are registered last.

## Server 0, TsndFunc (0x2728): requests

Function 0 takes a stream of requests (a byte and its operands), ended by 0xFF, and answers with 0x40 bytes whose word 0 is
0 (other functions answer with their number). An unknown request ends the stream (`tsndReqError`).

| Request | Operands | Does |
|---|---|---|
| 0x00-0x07 | port (bit 7 its flag), sequence (bit 7), id (bit 7); bit 0 a volume byte, bit 1 a pan byte, bit 2 a bend (2 bytes, high first); a program offset byte | `Tsnd_tqreq` 0x29BC: a sequence of the port's TQ started in a free slot of the 16, or one of lower priority taken over; defaults volume 0x7F, pan 0x40, bend 0x2000; slots playing with the same flagged port, id or sequence are stopped |
| 0x08 | port, sequence, id | the slots playing it told to stop |
| 0x09-0x0F | port, sequence, id; volume (bit 0), pan (bit 1), bend (bit 2: 2 bytes), then a byte unread | `Tsnd_tqchg` 0x2DAC: those of the playing slot changed |
| 0x28 | | `Tsnd_tq_allstop`: every sequence port's slots told to stop |
| 0x29 | volume (2 bytes) | `Tsnd_tq_setmvol`: the volume of every port |
| 0x40 | | every port stopped, one tick waited |
| 0x41 | a byte | mute: both cores' MUTE_ENABLE set when it is 0 |
| 0x42 | a byte | modhsyn's output mode: stereo (not 0) or mono |
| 0x43 | | `allstop_voice` 0x2118: ports stopped, a tick, every voice parked on the silent block and keyed on and off |
| 0x44 | cores (bits 6, 7) and mode (bits 0-5), depth (2), delay, feedback | `treq_ALL_REV` 0x187C: reverb preset set and its work area cleared |
| 0x45 | core mask, volume (2) | the cores' effect return volume (BVOL) |
| 0x47 / 0x48 / 0x49 | port code / port code / autoload index | a port's user gone (the last one frees it) / a port freed / an autoload's ports freed; ports loading are left |
| 0x4A | port code | a port stopped, a tick waited |
| 0x4B | volume (2) | the master volume of both cores |
| 0x4C | port code, volume (2) | a port's volume |
| 0x4D / 0x4E | load id (2), then the HD's, BD's and TQ's EE address and size (6 bytes each) | `treq_ALL_DMALOAD` 0x1AD8 / `DMAAUTOLOAD` 0x1DD0: a load queued for the DMA thread |

A load area is six bytes big-endian: the address (its low nibble is the size's bits 20-23) then the size's bits 12-19 and
4-11. The DMA thread (`TsndDmaLoad` 0x14CC) stops the port, waits a tick, copies the HD into the port's room in the IOP's
memory, the BD to the sound memory (`Tsnd_spuadr_tbl` 0x4050), loads the bank into modhsyn (bank 0) and the TQ, and leaves
the port's state ready. `Tsnd_load_tbl` (0x4108) maps a load id to its sound memory address, its port and the kind of TQ.
Both tables, `Tsnd_hd_size` (0x4088) and `Tsnd_tq_size` (0x40C8) are in `src/tsnd.cpp`.

## TQ: a port's sequences

A TQ has a word 0xE with its sequence count; from 0x10 a word a sequence: its offset (low half) and priority (top byte). A
slot's commands (`TsndTQ_ATick` 0xF9C, one slot a MIDI channel) are bytes below 0x80 a note (with a length wait),
0xE0-0xEB controller changes (volume scaled by the request's, pan offset, bend, program plus the request's offset),
0xEC-0xEE modhsyn's own expression, pan and bend messages (0xF9, or 0xFD by note id), 0xF8 a jump, 0xF9-0xFB reverb
preset, parameter and depth, 0xFD a priority, 0xFE/0xFF wait for the channel's voices and end the slot. All of them become
`Hsyn::PutMsg` and `Hsyn::PutHsMsg` calls.

## Server 1, TsndPutFunc (0x279C): state

The function's low byte (bit 31 only raises the thread's priority):

| n | Operand (word 0) | Answer |
|---|---|---|
| 4 | port (bits 16-22), sequence (8-14), id (0-6) | word 0: 1 if that sequence is playing |
| 6 | a port code | word 0: 0x10 while loading, the port's state (1 sequences ready, 3 loaded without), -1 no such port; word 1 the sequence count when ready |
| 0xC | 0 to wait for all loads | the loads pending |
| 0x10 | S/PDIF mode | 0; the cores' S/PDIF mode set |
| other | | -10 |

## Notes

- A request that waits (a tick, a few microseconds) holds the EE's call as the module's wait would; the DMA thread runs
  while it waits.
- Not done: the MIDI out ports (the module has none: `TsndInitMem(14, 0)`), the debug output.
