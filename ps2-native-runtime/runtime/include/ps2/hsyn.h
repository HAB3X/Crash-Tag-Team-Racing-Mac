// Sony's hardware synthesizer for the IOP (modhsyn, "SPU2_Synthesizer_Module", and modmsin, "Midi_Message_Input_Module",
// SDK 2.8), natively, for a game's own sound driver built on them: ports each take MIDI messages (and modhsyn's own 0xF9 and
// 0xFD messages, which address voices by an ID) into a buffer, and each tick plays them through the SPU2's voices from the
// port's sound banks (HD headers in the IOP's memory, their samples already in the sound memory), the voices' pitch and
// volume LFOs, portamento and envelopes kept as the module keeps them. runtime/src/iop/hsyn*.cpp has where each piece was read.
//
// Only the MIDI ports are done: a port of the SE type (modhsyn's sound effect sequences), the bank's SE timbres (its Setb
// chunk), custom LFO waves and velocity tables, reserved voices and the debug buffers are not.
//
// Every call is made with the sound lock held (ps2/sound.h)
#pragma once

#include "ps2/recomp.h"

namespace Ps2::Hsyn
{
// sceHSyn_Init: the ports (each with its MIDI input of 0x800 bytes) and the tick's period in microseconds (how often ATick
// will be called: the LFOs' and portamento's speed)
void Init(int ports, u32 tickMicroseconds);
// sceHSyn_Load: a port's bank (0-15) from its HD in the IOP's memory, its samples (its BD) at a byte address in the sound
// memory. -1 when the HD's chunks aren't SCEI's
int Load(int port, u32 spuAddress, u32 hdAddress, int bank);
// sceHSyn_ATick: every port's messages played, the voices keyed on and off and their pitch and volume updated
void ATick();
int SetVolume(int port, u16 volume);
// sceHSyn_AllSoundOff: every voice of the port cut
int AllSoundOff(int port);
// sceHSyn_GetChStat: each of the port's 16 channels' voices: bit 0 some are keyed, 1 some held by the damper, 2 some releasing
int GetChStat(int port, u8 status[16]);
// sceHSyn_SetOutputMode: 0 mono (every voice centred), 1 stereo
int SetOutputMode(int mode);

// modmsin's sceMSIn_PutMsg (a channel message: status byte then data, low byte first) and sceMSIn_PutHsMsg (0xF9 and 4 bytes,
// or 0xFD and 6): queued for the port's next tick. -1 when the port's buffer is full
int PutMsg(int port, u32 message);
int PutHsMsg(int port, const u8* message);
}
