# CRI's ADX IOP driver

CRI Middleware's ADX sound library splits its work between the EE and the IOP: the EE reads the ADX streams and decodes
them (CRI's 4-bit ADPCM) to 16 bit PCM, and an IOP module, `CRI_ADX_Driver` ("CRI ADX Driver Ver.8.82", built Aug 11 2003),
mixes that PCM and plays it through an SPU2 core's input. The runtime stands in for the module natively
(`runtime/src/iop/cri_adx.cpp`, `cri_psm.cpp`, `cri_sj.cpp`); the EE's libraries run recompiled and unchanged.

What's here was read from the module itself (Biohazard Outbreak File 2's `MODULES/DRIVE.DAT`, its tenth module; its
`.symtab` names every function) and from the EE's side in SLPM_656.92 (ADXT/PS2EE 8.92, DTX Ver.1.04, SJX 1.05, SJRMT 1.01,
ps2rna). Addresses `0x....` without a prefix are offsets in the IRX; EE addresses are File 2's. Biohazard Outbreak (File 1,
SLPM_654.28) loads the same module name and stops at the same place without it.

## The pieces

| Module piece | What it is |
|---|---|
| DTX (0x610-0xD40) | An RPC server, `0x90000200`, and up to 8 mailboxes between the EE and the IOP |
| SJ (SJRBF 0x511C, SJUNI 0x5AB4, SJMEM 0x8AA4) | Stream joints: queues of chunks (an address and a size) in four lanes, lane 0 room and lane 1 data by convention |
| SJRMT (0x6838) | SJs on the IOP made and driven from the EE through DTX's remote calls |
| SJX (0x6E78) | Joins an EE SJ to an IOP SJ through mailbox 0: chunks of room go to the EE, chunks of data come back |
| IOPRNA (0x13D8) | The EE's streams on the IOP: made by remote calls, commanded through mailbox 1 |
| PSM, PSMSLT, PSMMIX, PSMOUT, PSMTBF (0x1530-0x4EB0), RCV (0x6F70-0x8960) | The mixer and the output |

The module's thread (`iop_main_proc` 0x60) waits for the end of each vertical blank and runs `ADXI_ExecServer`: the streams
being flushed have their data thrown away (`IOPRNA_ExecHndl` 0xF80), then every mailbox is exchanged (`DTX_ExecHndl` 0xA44).
The output runs from the SPU2 transfer's interrupt (`psmout_int_func` 0x43D0, then `PSMOUT_ExecHndl` 0x3C88 on its thread).

## DTX: the RPC server and the mailboxes

`sceSifCallRpc` to `0x90000200` (`dtx_rpc_func` 0x610), the answer being the call's own buffer as the function left it:

| Function | Arguments (words) | Answer |
|---|---|---|
| 2 `DTX_Create` | ID (0-7), the EE's buffer (64-byte aligned), the IOP's (32-byte aligned), their size (64*N) | the IOP's handle, 0 if refused |
| 3 `DTX_Destroy` | the handle | |
| 0x400 + n | a remote call (below), its arguments | its results in the first words |

A mailbox is two buffers of the same size, the EE's and the IOP's; the last 64 bytes are a trailer whose word at 0x3C is the
count of exchanges. Each side sends its whole buffer to the other's with SIF DMA, its count one more than it last saw, then
waits until the other side's count passes its own; it then takes the other's buffer in (its receive callback), and sends its
own (its send callback). The EE sends first (EE `DTX_Create` 0x138C38 leaves it ready to send; the IOP's starts waiting). The
IOP looks once a vertical blank; the EE's `DTX_ExecServer` (0x139228) runs on its own schedule and polls the trailer through
uncached memory. The stand-in exchanges on the game's side at each vertical blank (`IopOnVblank`), so the IOP's writes into
the EE's memory never race the EE's code.

Mailbox 0 is SJX's, mailbox 1 IOPRNA's (bound by `sjx_reint_rmt` and `ioprna_reinit_rmt`, and as the module starts;
`DTX_Destroy` clears the binding). A buffer holds a word count at 0 and 16 byte entries from 0x10, at most 128.

## Remote calls (`DTX_SetUrpcCbf`'s table)

| n | Function | Arguments → answer |
|---|---|---|
| 0x00 | `sjx_create_rmt` | the EE's SJ, the IOP's SJ, 1 if the EE sends data, the EE's SJX object → the IOP's SJX object |
| 0x01 | `sjx_destroy_rmt` | the IOP's SJX object |
| 0x02 | `sjx_reset_rmt` | the object, a 16 bit count sent with each chunk afterwards |
| 0x03 | `sjx_reint_rmt` | binds mailbox 0 to SJX |
| 0x08 | `ioprna_create_rmt` | channels, the core (checked < 2, otherwise unused), each channel's IOP SJ → the stream |
| 0x09 | `ioprna_destroy_rmt` | the stream |
| 0x0A | `ioprna_reinit_rmt` | channels, a work area in the IOP's memory, its size: each channel's 0x400 byte ring; binds mailbox 1 |
| 0x0B | `ioprna_restore_iopsnd_rmt` | a core: PSM's core set, PSM finished and started again |
| 0x0C / 0x0D | `ioprna_init_iopsnd_rmt` / `finish` | PSM started (counted) / finished |
| 0x0E / 0x0F | Dolby Digital set up / shut down | not done (logged) |
| 0x20 | `sjrmt_rbf_create` | buffer, size, extra → an SJRBF |
| 0x21 | `sjrmt_mem_create` | buffer, size → an SJMEM (not done: logged) |
| 0x22 | `sjrmt_uni_create` | mode, work area, its size → an SJUNI |
| 0x23-0x2A | destroy, uuid (not done), reset, get, unget, put, data, is-get | the SJ and the lane, as the SJ functions |

## The streams' data: SJX

ps2rna's start-up (EE 0x13D80C) makes 16 PSJs, each an IOP SJUNI (mode 1, 0x100 bytes of links) and an EE SJUNI joined by SJX
with "the EE sends data"; the EE SJUNI starts with one 16 KB chunk of the IOP's memory as room. The EE's ADX decoder takes
room chunks from its SJ, writes PCM into the IOP's memory with SIF DMA, and puts them back as data. Then:

- EE → IOP (`sjx_sndcbf` 0x140C78 on the EE): each of the EE's data chunks as an entry `{0, lane, count, the IOP's SJX
  object, address, size}`; the IOP (`sjx_rcvcbf_iop` 0x6B3C) puts each into its SJ's lane 1.
- IOP → EE (`sjx_sndcbf_iop` 0x6BE8): every chunk of each joined IOP SJ's lane 0 (the room the mixer has freed) as
  `{0, 0, count, the EE's SJX object, address, size}`; the EE (0x140BD8) puts each into its SJ's lane 0.

## The streams' commands: IOPRNA

The EE (0x13DCC0) sends `{halfword command, the stream, a, b}` entries in mailbox 1 when something changed; the IOP sends
none. Commands (`ioprna_rcvcbf` 0xD40): 0 start, 1 stop, 2 start if a is 1 else stop, 3 channels played (a), 4 rate (a),
5 volume (b, 0-255, every channel), 8 flush (a: the inputs' data thrown away each vertical blank), 9 pan (channel a, pan b,
-15 to 15), 10 mix (a 1: core 0's mixer; else core 1's), 6 and 7 nothing.

## Mixing and output: PSM

- Each stream channel has a converter (RCV) from its input SJ's lane 1 to a 0x400 byte ring (an SJRBF in the work area
  `ioprna_reinit_rmt` gave): at most 512 and at least 32 samples a time, as many as the input has and the ring has room for.
  Same rate: a copy. Twice the rate: each sample makes the midpoint from the one before, then itself (`rcv_twofold`
  0x7F04). The module also converts three- and fourfold and by pitch steps; neither Outbreak disc needs it (File 2's ADX
  files are 48 kHz stereo and 24 kHz mono, File 1's also 48 kHz mono), so those stop the game with the rates named.
- Each core has a mixer of 32 taps (a channel's ring and its left and right volumes 0-255) and an output: a 0x1000 byte
  buffer, as the SPU2's core input reads it (0x200 bytes of left, then 0x200 of right, round), played with
  `sceSdBlockTrans`'s loop (`SdBlockTransLoop` here) with the core's input volume at 0x7FFF. When a half has been played,
  the converters run and the mixer makes 512 samples into it, a block of 256 at a time: each tap with a whole block in its
  ring is added (255 as is, 128 and 64 by shifts, others times v / 256), clipped to ±0x7FFF; with none, silence.
- A channel's volumes: its volume times the pan table (31 levels, 255 at the far side to 0) / 255, rounded; a stereo
  stream's channels have their own pans, a mono stream uses the first. Panning uses the first channel's volume (as the
  module does). The core whose output plays is the module's `spucore=` argument (1 by default); streams mix into it.

## What isn't done

Dolby Digital (the stream type that sends one stream straight out of core 0), SJMEM, SJRMT's uuid, RCV's conversions other
than copy and twofold, and the module loaded twice (an IOP reset after it has started) all stop or log, naming what's missing.
