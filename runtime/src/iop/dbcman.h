// The Dbc_Manager (dbcman.irx 3.1) and its device drivers, high level: the IOP side of SDK 2.8's libdbc, which libpad2 and
// libmc2 (and the game itself) talk to on the EE. dbcman keeps a table of devices (each found by a driver: ds2o_s1/ds2u_s1's
// DualShock 2, mc2_s1's memory card) and one of sockets (made by the EE's libraries), links each socket to a device that
// matches it, and passes the EE's calls on a socket to its device's driver; a driver sends its data into the socket's buffers in
// the EE's memory (SIF DMA on the console). DESIGN.md has the protocol.
#pragma once

#include "iop.h"

#include <string>
#include <vector>

namespace Ps2
{
// A device driver: what dbcman calls it for, on a socket linked to its device. A command is the libdbc caller's: its top byte
// a device class, bits 16-23 a group, the low 14 bits a code (each driver its own); the answer goes back to the EE as the
// call's result
class DbcDriver
{
public:
    virtual ~DbcDriver() = default;
    // ReceiveData (the driver's function 3): the reply's bytes (dbcman takes at most 512)
    virtual s32 Receive(u32 command, std::vector<u8>& reply) = 0;
    // SRData (function 4): data sent and a reply
    virtual s32 SendReceive(u32 command, const u8* data, u32 size, std::vector<u8>& reply) = 0;
    // SendData (function 5, and SendData2's and SendData3's): data sent; its length, which the driver may rewrite
    virtual s32 Send(u32 command, const u8* data, u32& size) = 0;
    // Each vertical blank (a driver's own thread waits for them on the console)
    virtual void Vblank() {}
};

// A driver registered (dbcman's export 4): its device's type (1 a pad, 2 a memory card) and the second word of its
// description (unk_0x04, compared with a socket's unk_0x10 when the socket asks for it); the device's number
int DbcRegister(u32 type, u32 unk04, DbcDriver* driver);
// The device found on a port (export 6): linked to the first unlinked socket that matches it
void DbcConnect(int device, u32 port, u32 slot, const std::string& name);
// The device's data into its socket's buffers in the EE's memory: one of its two buffers (export 8), or the first at an
// offset (export 9; a multiple of 64). Nothing is sent while no socket is linked to the device
void DbcSignalData(int device, u32 buffer, const void* data, u32 size);
void DbcSignalData2(int device, u32 offset, const void* data, u32 size);

// The drivers, started as the game loads their modules (DbcModuleLoaded): ds2.cpp, mc2.cpp
void Ds2ModuleLoaded();
void Mc2ModuleLoaded(const u8* elf, size_t size);
}
