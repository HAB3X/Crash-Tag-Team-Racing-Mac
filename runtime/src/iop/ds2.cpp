// The DualShock 2 driver of the Dbc_Manager (ds2o_s1 in File 2's MODULES/DRIVE.DAT, ds2u_s1 in File 1's: the same at the
// interface dbcman sees, they differ in how they set the pad up), high level: port 0's DualShock 2, fed from the runtime's
// input (padman.cpp's PadDualShock2Report). Addresses below are ds2o_s1's (its text from 0).
//
// The driver registers a device of type 1 and connects it as port 0, slot 0, with no name, once it has put the pad in its
// pressure mode (0x79), which it keeps. Then, at every vertical blank, it reads the pad and sends a 128 byte frame into the
// linked socket's buffer counter % 2 (0x11AC), counter being the frame's number, from 0 (libpad2 reads the frame with the larger
// number, and ignores a frame numbered 0):
//    0    the pad's state: 1 read (3 when the read failed)
//    1    the data's length + 4; 2 the data's length (18 in mode 0x79); 3 4
//    4    1 when the last SIO2 exchange ended without an error (ds2o's state + 56, set at 0x1554)
//    28   the data (the pad's answer after its first three bytes: buttons, sticks, pressures), then the button profile (4 bytes:
//         which of the data's bits and bytes the mode has; FF FF FF FF in mode 0x79). 28-59 are cleared first; 8-27 and
//         60-123 are never written (zeros)
//    124  the frame's number
// Commands (0x1C3C): the top byte 1, then a group and a code. ReceiveData: group 1 code 2 the mode's ID (4 bytes), group 1
// code 12 the state (1 byte), group 2 code 2 the button profile (4 bytes), group 3 code 2 the actuators (1 byte: 3 when the
// pad has them). SendData: group 3 code 11 sets the actuators.
#include "dbcman.h"

namespace Ps2
{
namespace
{
constexpr u8 ModePressure = 0x79;    // the DualShock 2's mode with pressures, the one the driver keeps
constexpr u32 DataLength = 18;       // the data's bytes in that mode: (0x79 & 15) * 2
constexpr u8 StateRead = 1;
constexpr u8 Actuators = 3;          // group 3 code 2's answer for a pad with actuators (ds2o's 0x1EF4)
const u8 Profile[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // mode 0x79's (ds2o's 0x1EF8)

// ds2o's answers to a command it doesn't know (0x1C3C)
constexpr s32 BadClass = -1;
constexpr s32 BadGroup = -2;
constexpr s32 BadCode = -3;

class DualShock2 final : public DbcDriver
{
public:
    s32 Receive(u32 command, std::vector<u8>& reply) override { return Command(command, nullptr, 0, &reply); }

    s32 SendReceive(u32 command, const u8* data, u32 size, std::vector<u8>& reply) override
    {
        return Command(command, data, size, &reply);
    }

    s32 Send(u32 command, const u8* data, u32& size) override { return Command(command, data, size, nullptr); }

    void Vblank() override
    {
        u8 frame[128] = {};
        frame[0] = StateRead;
        frame[1] = DataLength + 4;
        frame[2] = DataLength;
        frame[3] = 4;
        frame[4] = 1;
        PadDualShock2Report(0, frame + 28);
        std::memcpy(frame + 28 + DataLength, Profile, sizeof(Profile));
        std::memcpy(frame + 124, &frames_, 4);
        DbcSignalData(device, frames_ % 2, frame, sizeof(frame));
        frames_++;
    }

private:
    // ds2o's 0x1C3C: a reply where the call has one (ReceiveData, SRData), the data where it sends some
    s32 Command(u32 command, const u8* data, u32 size, std::vector<u8>* reply)
    {
        if (command >> 24 != 1)
        {
            return BadClass;
        }
        u32 group = (command >> 16) & 0xFF;
        u32 code = command & 0x3FFF;
        bool asks = (group == 1 && (code == 2 || code == 12)) || ((group == 2 || group == 3) && code == 2);
        if (asks && reply == nullptr)
        {
            Fatal("ds2: command %08x sent with SendData: the driver writes its answer through a null pointer", command);
        }
        switch (group)
        {
        case 1:
            if (code == 2)
            {
                const u8 mode[4] = {ModePressure, 0, 0, 0};
                reply->assign(mode, mode + 4);
                return 1;
            }
            if (code == 12)
            {
                reply->assign(1, StateRead);
                return 1;
            }
            return BadCode;
        case 2:
            if (code != 2)
            {
                return BadCode;
            }
            reply->assign(Profile, Profile + 4);
            return 1;
        case 3:
            if (code == 2)
            {
                reply->assign(1, Actuators);
                return 1;
            }
            if (code == 11)
            {
                return SetActuators(command, data, size);
            }
            return BadCode;
        default:
            return BadGroup;
        }
    }

    // ds2o's 0x1758: the actuators' setting (its kind at 8 + the word at 4: 1 the small motor, 2 the big, 3 both)
    s32 SetActuators(u32 command, const u8* data, u32 size)
    {
        if (data == nullptr)
        {
            Fatal("ds2: command %08x with no data: the driver reads the actuators' setting through a null pointer", command);
        }
        u32 at = 0;
        if (size >= 8)
        {
            std::memcpy(&at, data + 4, 4);
        }
        if (size < 8 || at >= size - 8)
        {
            Fatal("ds2: the actuators' setting (%u bytes) has no kind at 8 + %u", size, at);
        }
        u8 kind = data[8 + at];
        if (kind < 1 || kind > 3)
        {
            return -1;
        }
        if (Once("ds2 actuators"))
        {
            Log("ds2: the actuators set (kind %u): rumble isn't done", kind);
        }
        return 1;
    }

public:
    int device = -1;

private:
    u32 frames_ = 0;
};
}

void Ds2ModuleLoaded()
{
    static DualShock2 pad;
    if (pad.device >= 0)
    {
        Log("ds2: a second DualShock 2 driver loaded: port 0 already has its pad");
        return;
    }
    // The device's description (ds2o's 0x1A20): type 1, its second word 0
    pad.device = DbcRegister(1, 0, &pad);
    DbcConnect(pad.device, 0, 0, "");
}
}
