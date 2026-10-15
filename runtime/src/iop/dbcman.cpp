// The Dbc_Manager (dbcman.irx 3.1, "PsIIdbcman  2800"), high level: its RPC servers, its device and socket tables, and the
// SIF DMA of its drivers' data into the EE's memory. Taken from the module as Biohazard Outbreak loads it (MODULES/DRIVE.DAT;
// File 1's and File 2's are the same); addresses below are the module's (dbcman.irx's text from 0).
//
// RPC 0x80001300 (its handler at 0x1C70): a 640 byte block, the answer the block itself with results written in:
//   0x80001301 CreateSocket   in: flags 0, type 4, port 8, slot 12, unk_0x10 16, name 20 (16 bytes), the EE's two buffers 40
//                              and 44; out: the socket's number (or < 0) at 36
//   0x80001302 DeleteSocket   socket 0; result 4
//   0x80001303 GetDepNumber   socket 0; the linked device's number (-12 none) at 4
//   0x80001304 SetWorkAddr    the EE's work area at 4 (the socket table's connection words go there); 1 at 0
//   0x80001315 InitSocket     socket 0; result 4 (the drivers' functions 0 and 1 do nothing: ds2o 0x1B70, mc2 0x1E2C)
//   0x80001316 ResetSocket    socket 0; result 4
//   0x80001317 GetDeviceStatus  not supported by this dbcman (it prints so): 0 at 4
//   0x80001318 SRData         socket 0, command 4, the data's length 8 and the reply's 12, the data 16, the reply 528 (512
//                              bytes each); result 1040
//   0x80001319 SendData       socket 0, command 4, length 8, data 12; result 524
//   0x8000131A ReceiveData    socket 0, command 4; the reply's length 8 and bytes 12; result 524
//   0x8000131D GetDeviceStatus2  port 16, slot 20: the socket linked to the device found there (-22 none linked, -12 no
//                              device) at 4
//   0x80001363 the module's version: 0x301 at 0 (libdbc wants 3.x)
// RPC 0x8000131B and 0x8000131C: SendData from libdbc's SendData2 (a 4240 byte block, result at 4236) and SendData3 (8336,
// result at 8332), called without waiting (libmc2 sends its card commands so)
#include "dbcman.h"

#include <mutex>

namespace Ps2
{
namespace
{
constexpr int Slots = 16; // sockets and devices, each

// Results dbcman answers
constexpr s32 NoDevice = -12;   // no device linked, or none found
constexpr s32 BadSocket = -22;  // no such socket
constexpr s32 NotLinked = -23;  // a socket with no device

// What a socket accepts (flags: 1 the type, 2 the port and slot, 4 unk_0x10, 8 the name) and a device is
struct Identity
{
    u32 type = 0;
    u32 port = 0;
    u32 slot = 0;
    u32 unk10 = 0; // a socket's word 16, a device's description's second word
    std::string name;
};

struct Socket
{
    bool made = false;
    u32 flags = 0;
    Identity wants;
    u32 buffers[2] = {};
    int device = -1;
};

struct Device
{
    DbcDriver* driver = nullptr; // registered
    bool connected = false;
    Identity is;
    int socket = -1;
};

std::recursive_mutex g_lock;
Socket g_sockets[Slots];
Device g_devices[Slots];
u32 g_workArea = 0;

bool Matches(const Socket& s, const Device& d)
{
    return (!(s.flags & 1) || s.wants.type == d.is.type) &&
           (!(s.flags & 2) || (s.wants.port == d.is.port && s.wants.slot == d.is.slot)) &&
           (!(s.flags & 4) || s.wants.unk10 == d.is.unk10) && (!(s.flags & 8) || s.wants.name == d.is.name);
}

u8* EeBuffer(u32 address, u32 size)
{
    u32 at = address & 0x1FFFFFFF;
    if (at + size > RamSize)
    {
        Fatal("dbcman: %u bytes at %08x are outside the EE's memory", size, address);
    }
    return EeMemory(at);
}

// The sockets' connection words (1 linked) into the EE's work area, as dbcman's 0x00A4 sends them (it sends 128 bytes: these
// 64, then 64 of its own tables, which libdbc never reads: sceDbcGetDepNumber copies the first 64)
void SendConnections()
{
    if (g_workArea == 0)
    {
        return;
    }
    u32 words[Slots];
    for (int i = 0; i < Slots; i++)
    {
        words[i] = g_sockets[i].device >= 0 ? 1 : 0;
    }
    std::memcpy(EeBuffer(g_workArea, sizeof(words)), words, sizeof(words));
}

void Link(int socket, int device)
{
    g_sockets[socket].device = device;
    g_devices[device].socket = socket;
    PS2_LOG(LogIop, "dbcman: socket %d linked to device %d (type %u, port %u)", socket, device, g_devices[device].is.type,
            g_devices[device].is.port);
    SendConnections();
}

s32 CreateSocket(const RpcCall& c)
{
    int n = 0;
    while (n < Slots && g_sockets[n].made)
    {
        n++;
    }
    if (n == Slots)
    {
        return -21;
    }
    Socket& s = g_sockets[n];
    s = Socket();
    s.made = true;
    s.flags = c.In32(0);
    s.wants.type = c.In32(4);
    s.wants.port = c.In32(8);
    s.wants.slot = c.In32(12);
    s.wants.unk10 = c.In32(16);
    s.wants.name = c.InString(20, 16);
    s.buffers[0] = c.In32(40);
    s.buffers[1] = c.In32(44);
    PS2_LOG(LogIop, "dbcman: socket %d made (flags %x, type %u, port %u, slot %u, buffers %08x %08x)", n, s.flags,
            s.wants.type, s.wants.port, s.wants.slot, s.buffers[0], s.buffers[1]);
    for (int d = 0; d < Slots; d++)
    {
        if (g_devices[d].connected && g_devices[d].socket < 0 && Matches(s, g_devices[d]))
        {
            Link(n, d);
            break;
        }
    }
    return n;
}

s32 DeleteSocket(u32 socket)
{
    if (socket >= Slots)
    {
        Fatal("dbcman: DeleteSocket %u: there are %d sockets (dbcman would write past its table)", socket, Slots);
    }
    Socket& s = g_sockets[socket];
    if (!s.made)
    {
        return BadSocket;
    }
    if (s.device >= 0)
    {
        g_devices[s.device].socket = -1;
    }
    s = Socket();
    SendConnections();
    return 1;
}

// The device linked to a socket made, for the calls passed on to its driver: its number, or the call's result
s32 LinkedDevice(u32 socket)
{
    if (socket >= Slots || !g_sockets[socket].made)
    {
        return BadSocket;
    }
    return g_sockets[socket].device >= 0 ? g_sockets[socket].device : NotLinked;
}

s32 GetDeviceStatus2(u32 port, u32 slot)
{
    for (int d = 0; d < Slots; d++)
    {
        const Device& dev = g_devices[d];
        if (dev.connected && dev.is.port == port && dev.is.slot == slot)
        {
            return dev.socket >= 0 ? dev.socket : BadSocket;
        }
    }
    return NoDevice;
}

// The calls on a socket's device: 640 byte blocks (SRData, SendData, ReceiveData)
void SocketCall(RpcCall& c, u32 fno)
{
    s32 device = LinkedDevice(c.In32(0));
    u32 resultAt = fno == 0x80001318 ? 1040 : 524;
    if (device < 0)
    {
        c.Out32(resultAt, static_cast<u32>(device));
        return;
    }
    DbcDriver& driver = *g_devices[device].driver;
    u32 command = c.In32(4);
    std::vector<u8> reply;
    s32 result;
    switch (fno)
    {
    case 0x80001318:
    {
        u32 size = std::min<u32>(c.In32(8), 512);
        result = driver.SendReceive(command, c.in + 16, size, reply);
        if (reply.size() > 512)
        {
            Fatal("dbcman: a driver's reply of %zu bytes (the block has room for 512)", reply.size());
        }
        c.Out32(12, static_cast<u32>(reply.size()));
        std::memcpy(c.out.data() + 528, reply.data(), reply.size());
        break;
    }
    case 0x80001319:
    {
        u32 size = c.In32(8);
        result = driver.Send(command, c.in + 12, size);
        c.Out32(8, size);
        break;
    }
    default: // 0x8000131A
        result = driver.Receive(command, reply);
        if (reply.size() > 512)
        {
            Fatal("dbcman: a driver's reply of %zu bytes (the block has room for 512)", reply.size());
        }
        c.Out32(8, static_cast<u32>(reply.size()));
        std::memcpy(c.out.data() + 12, reply.data(), reply.size());
        break;
    }
    c.Out32(resultAt, static_cast<u32>(result));
}

// The answer is the block itself with the results written in
void Answer(RpcCall& c, u32 size)
{
    c.out.assign(c.in, c.in + std::min(c.inSize, size));
    c.out.resize(std::max(c.recvSize, size));
}

void Serve(RpcCall& c)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Answer(c, 640);
    switch (c.fno)
    {
    case 0x80001301:
        c.Out32(36, static_cast<u32>(CreateSocket(c)));
        break;
    case 0x80001302:
        c.Out32(4, static_cast<u32>(DeleteSocket(c.In32(0))));
        break;
    case 0x80001303:
    {
        u32 socket = c.In32(0);
        s32 device = socket < Slots ? g_sockets[socket].device : -1;
        c.Out32(4, static_cast<u32>(device >= 0 ? device : NoDevice));
        break;
    }
    case 0x80001304:
        g_workArea = c.In32(4);
        SendConnections();
        c.Out32(0, 1);
        break;
    case 0x80001315:
    case 0x80001316:
    {
        s32 device = LinkedDevice(c.In32(0));
        c.Out32(4, static_cast<u32>(device < 0 ? device : 1));
        break;
    }
    case 0x80001317:
        if (Once("dbcman status"))
        {
            Log("dbcman: GetDeviceStatus: \"status function is not support\" (dbcman's own answer): 0");
        }
        c.Out32(4, 0);
        break;
    case 0x80001318:
    case 0x80001319:
    case 0x8000131A:
        SocketCall(c, c.fno);
        break;
    case 0x8000131D:
        c.Out32(4, static_cast<u32>(GetDeviceStatus2(c.In32(16), c.In32(20))));
        break;
    case 0x80001363:
        c.Out32(0, 0x301);
        break;
    default:
        // dbcman's table sends any other number back untouched
        if (Once("dbcman fno " + std::to_string(c.fno)))
        {
            Log("dbcman: RPC function %#x isn't dbcman's: the block goes back as it came", c.fno);
        }
        break;
    }
}

// SendData2 (0x8000131B) and SendData3 (0x8000131C): the SendData block with more room
void ServeSend(RpcCall& c, u32 size)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Answer(c, size);
    u32 resultAt = size - 4;
    if (c.inSize > size)
    {
        Log("dbcman: send_data buffer over (length=%u)", c.inSize);
        return;
    }
    s32 device = LinkedDevice(c.In32(0));
    if (device < 0)
    {
        c.Out32(resultAt, static_cast<u32>(device));
        return;
    }
    u32 length = c.In32(8);
    if (length > size - 16)
    {
        Fatal("dbcman: SendData of %u bytes in a %u byte block", length, size);
    }
    s32 result = g_devices[device].driver->Send(c.In32(4), c.in + 12, length);
    c.Out32(8, length);
    c.Out32(resultAt, static_cast<u32>(result));
}
}

int DbcRegister(u32 type, u32 unk04, DbcDriver* driver)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int d = 0; d < Slots; d++)
    {
        if (g_devices[d].driver == nullptr)
        {
            g_devices[d] = Device();
            g_devices[d].driver = driver;
            g_devices[d].is.type = type;
            g_devices[d].is.unk10 = unk04;
            return d;
        }
    }
    Fatal("dbcman: a driver of type %u: the %d devices' room is taken", type, Slots);
}

void DbcConnect(int device, u32 port, u32 slot, const std::string& name)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Device& d = g_devices[device];
    d.connected = true;
    d.is.port = port;
    d.is.slot = slot;
    d.is.name = name;
    Log("dbcman: device %d (type %u) connected on port %u, slot %u", device, d.is.type, port, slot);
    for (int s = 0; s < Slots; s++)
    {
        if (g_sockets[s].made && g_sockets[s].device < 0 && Matches(g_sockets[s], d))
        {
            Link(s, device);
            break;
        }
    }
}

void DbcSignalData(int device, u32 buffer, const void* data, u32 size)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int socket = g_devices[device].socket;
    if (socket < 0)
    {
        return;
    }
    if (size % 128 != 0 || buffer > 1)
    {
        Fatal("dbcman: SignalData of %u bytes into buffer %u (128 byte multiples, buffer 0 or 1)", size, buffer);
    }
    std::memcpy(EeBuffer(g_sockets[socket].buffers[buffer], size), data, size);
}

void DbcSignalData2(int device, u32 offset, const void* data, u32 size)
{
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    int socket = g_devices[device].socket;
    if (socket < 0)
    {
        return;
    }
    if (size % 128 != 0 || offset % 64 != 0)
    {
        Fatal("dbcman: SignalData2 of %u bytes at offset %u (128 byte multiples at 64 byte offsets)", size, offset);
    }
    std::memcpy(EeBuffer(g_sockets[socket].buffers[0] + offset, size), data, size);
}

void DbcServe(RpcCall& c)
{
    switch (c.sid)
    {
    case 0x80001300:
        Serve(c);
        break;
    case 0x8000131B:
        ServeSend(c, 4240);
        break;
    default: // 0x8000131C
        ServeSend(c, 8336);
        break;
    }
}

void DbcModuleLoaded(const std::string& name, const u8* elf, size_t size)
{
    if (name == "ds2o_s1" || name == "ds2u_s1")
    {
        Ds2ModuleLoaded();
    }
    else if (name == "mc2_s1")
    {
        Mc2ModuleLoaded(elf, size);
    }
}
}
