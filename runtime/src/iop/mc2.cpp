// The memory card driver of the Dbc_Manager (mc2_s1 3020, "PsIImc2_s1  3020", File 2's MODULES/DRIVE.DAT), high level: the
// card in the first slot. Addresses below are the module's (its text from 0).
//
// mc2_s1 is a page device: the file system (directories, files, the FAT, formatting) is libmc2's on the EE, which sends the
// driver page reads, page writes and block erases. So the card is the card's pages: an image in --memcards, port0.ps2, 16384
// pages of 528 bytes (512 of data, then the spare area: 3 bytes of ECC for each 128, 4 unused), 8,650,752 bytes, the layout of
// PCSX2's .ps2 cards. A new image is a blank card (every byte FF), which the game formats itself.
//
// The driver registers a device of type 2 and connects it as port 2, slot 0, with no name, when the card is found. libmc2 sends
// each command with SendData3 (0x1F64): the command is the call's, the request its data:
//    0 the first page, 4 the pages, 8 the page's size, 12 the pages of a block, 16 the card's pages, 20 the first backup block,
//    24 the second backup block, 28 the block copied from, 32 a block, 36 non-zero for pages with ECC, 40 the pages' data
// and the answer is a 128 byte status (0 a count of the commands, 4 the device, 8 the result: 1 done, 0 failed, 2 the card was
// changed; 12 a command's data), sent into the socket's two buffers in turn; a read sends it to the first buffer instead, after
// the pages from its offset 128. Commands: 1 read (pages, their ECC checked and corrected), 2 write (pages, through the backup
// blocks: the block is copied to the first, its number written into the second's first two pages, the block erased and written
// with the new pages and the old ones, the second backup block erased), 3 the card's size (data: the page's bytes, the block's
// pages, the card's pages), 4 the card's check (2 while it counts as changed), 5 the write the second backup block records (2
// and its block, or 1), 6 that write finished from the first backup block, 7 a block copied (28 to 32), 8 one page read with its
// spare area, 9 a block erased, 10 the clock (sceCdReadClock's 8 bytes), 11 the same page written to pages (the block erased at
// each block's first page), 12 the card probed, 13 the card no longer counts as changed. It counts as changed from the driver's
// start until 13 (every other command but 4 answers 2 meanwhile, its status otherwise unchanged).
#include "dbcman.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace Ps2
{
namespace
{
constexpr u32 PageBytes = 512;
constexpr u32 BlockPages = 16;
constexpr u32 CardPages = 16384;
constexpr u32 RawPage = 528;              // a page and its spare area, in the image
constexpr u32 EccBytes = PageBytes / 128 * 3;
constexpr u32 CardPort = 2;               // the slot's SIO2 port, as the driver connects it

enum : u32
{
    Read = 1,
    Write = 2,
    Spec = 3,
    Check = 4,
    Pending = 5,
    Finish = 6,
    CopyBlockCmd = 7,
    ReadRaw = 8,
    Erase = 9,
    Clock = 10,
    Fill = 11,
    Probe = 12,
    Unchange = 13,
};

// The ECC of 128 bytes, as the driver's 0x2AEC makes it: a column parity byte from a table (each bit the parity of some of a
// byte's bits: 0x55, 0x33, 0x0F, -, 0xAA, 0xCC, 0xF0, all; the driver's table at 0x34E4) and two line parities
u8 ColumnParity(u8 b)
{
    static const u8 masks[8] = {0x55, 0x33, 0x0F, 0x00, 0xAA, 0xCC, 0xF0, 0xFF};
    u8 v = 0;
    for (int bit = 0; bit < 8; bit++)
    {
        v |= static_cast<u8>((__builtin_popcount(b & masks[bit]) & 1) << bit);
    }
    return v;
}

void MakeEcc(const u8* data, u32 size, u8* ecc)
{
    for (u32 chunk = 0; chunk < size; chunk += 128, ecc += 3)
    {
        u8 column = 0, line0 = 0, line1 = 0;
        for (u32 i = 0; i < 128; i++)
        {
            u8 p = ColumnParity(data[chunk + i]);
            column ^= p;
            if (p & 0x80)
            {
                line0 ^= static_cast<u8>(~i);
                line1 ^= static_cast<u8>(i);
            }
        }
        ecc[0] = ~column & 0x77;
        ecc[1] = ~line0 & 0x7F;
        ecc[2] = ~line1 & 0x7F;
    }
}

// The driver's 0x2B88: a page's ECC (after its data) checked, a single wrong bit corrected; an erased ECC (all FF) passes. The
// ECC made from the data is left after it, as the driver leaves it
bool CheckEcc(u8* page, u32 size, u32 eccSize)
{
    if (eccSize == 0)
    {
        return true;
    }
    std::array<u8, EccBytes> stored;
    std::memcpy(stored.data(), page + size, eccSize);
    MakeEcc(page, size, page + size);
    if (std::all_of(stored.begin(), stored.begin() + 12, [](u8 b) { return b == 0xFF; }))
    {
        return true;
    }
    const u8* made = page + size;
    for (u32 chunk = 0; chunk < size; chunk += 128, made += 3)
    {
        u32 n = chunk / 128 * 3;
        u8 column = stored[n] ^ made[0];
        u8 line0 = stored[n + 1] ^ made[1];
        u8 line1 = stored[n + 2] ^ made[2];
        if (column == 0 && line0 == 0 && line1 == 0)
        {
            continue;
        }
        if ((line0 ^ line1) != 0x7F || ((column & 0x0F) ^ (column >> 4)) != 7)
        {
            return false;
        }
        page[chunk + line1] ^= static_cast<u8>(1 << (column >> 4));
    }
    return true;
}

u8 Bcd(int v) { return static_cast<u8>((v / 10) << 4 | (v % 10)); }

class Card final : public DbcDriver
{
public:
    int device = -1;

    s32 Receive(u32 command, std::vector<u8>&) override
    {
        Fatal("mc2: ReceiveData %08x: libmc2 never asks it (the driver's function 3 only answers 1)", command);
    }

    s32 SendReceive(u32 command, const u8*, u32, std::vector<u8>&) override
    {
        Fatal("mc2: SRData %08x: libmc2 never sends it (the driver would take it as SendData)", command);
    }

    s32 Send(u32 command, const u8* request, u32& size) override
    {
        req_ = request;
        size = 8320; // the driver writes it (0x2014)
        return Command(command);
    }

private:
    u32 Req(u32 at) const
    {
        u32 v;
        std::memcpy(&v, req_ + at, 4);
        return v;
    }
    s32 Result() const
    {
        s32 v;
        std::memcpy(&v, status_.data() + 8, 4);
        return v;
    }
    void SetResult(s32 v) { std::memcpy(status_.data() + 8, &v, 4); }
    void SetData(u32 at, const void* data, u32 size) { std::memcpy(status_.data() + 12 + at, data, size); }

    s32 Command(u32 command)
    {
        if (changed_ && command != Check && command != Unchange)
        {
            SetResult(2);
            return SendStatus();
        }
        std::fill(status_.begin() + 12, status_.end(), 0);
        SetResult(1);
        u32 dev = static_cast<u32>(device);
        std::memcpy(status_.data() + 4, &dev, 4);
        commands_++;
        std::memcpy(status_.data(), &commands_, 4);
        eccSize_ = Req(36) != 0 || command == ReadRaw ? EccBytes : 0;
        rawSize_ = PageBytes + eccSize_;
        switch (command)
        {
        case Read:
        case ReadRaw:
        case Write:
        case Pending:
        case Finish:
        case Fill:
            CheckGeometry(command);
            if (Req(16) < Req(0) + Req(4) || (Req(20) + 1) * BlockPages > Req(16) || (Req(24) + 1) * BlockPages > Req(16))
            {
                SetResult(0);
                return SendStatus();
            }
            break;
        case CopyBlockCmd:
        case Erase:
            CheckGeometry(command);
            break;
        default:
            break;
        }
        switch (command)
        {
        case Read:
        case ReadRaw:
            return ReadPages(command);
        case Write:
            WritePages();
            break;
        case Spec:
        {
            const u16 sizes[2] = {PageBytes, BlockPages};
            SetData(0, sizes, 4);
            SetData(4, &CardPages, 4);
            break;
        }
        case Check:
            SetResult(changed_ ? 2 : 1);
            break;
        case Pending:
        {
            s32 block;
            SetResult(PendingBlock(block));
            SetData(0, &block, 4);
            break;
        }
        case Finish:
            SetResult(FinishPending());
            break;
        case CopyBlockCmd:
            SetResult(CopyBlock(static_cast<s32>(Req(32)), static_cast<s32>(Req(28))));
            break;
        case Erase:
            EraseBlock(Req(32) * BlockPages);
            break;
        case Clock:
            ReadClock();
            break;
        case Fill:
            FillPages();
            break;
        case Probe:
            // The card answers the probe with the termination code the driver gave it when it found the card: present
            break;
        case Unchange:
            changed_ = false;
            break;
        default:
            if (Once("mc2 command " + std::to_string(command)))
            {
                Log("mc2: command %u isn't the driver's: it answers 1 and does nothing", command);
            }
            break;
        }
        return SendStatus();
    }

    // The status into the socket's buffers, each in turn (0x1DCC)
    s32 SendStatus()
    {
        DbcSignalData(device, nextBuffer_, status_.data(), 128);
        nextBuffer_ ^= 1;
        return Result();
    }

    void CheckGeometry(u32 command) const
    {
        if (Req(8) != PageBytes || Req(12) != BlockPages || Req(16) != CardPages)
        {
            Fatal("mc2: command %u for a card of %u byte pages, %u to a block, %u pages; this card's are %u, %u, %u", command,
                  Req(8), Req(12), Req(16), PageBytes, BlockPages, CardPages);
        }
    }

    // 1: pages read, their ECC checked, sent from the first buffer's offset 128; 8: one page with its spare area at 128. The
    // status goes to the first buffer's start
    s32 ReadPages(u32 command)
    {
        u32 count = command == ReadRaw ? 1 : Req(4);
        u32 offset = 128;
        for (u32 i = 0; i < count; i++)
        {
            ReadPage(Req(0) + i, buffer_.data());
            if (command == Read)
            {
                if (!CheckEcc(buffer_.data(), PageBytes, eccSize_))
                {
                    Log("mc2: page %u's ECC is wrong beyond correcting", Req(0) + i);
                    SetResult(0);
                }
                DbcSignalData2(device, offset, buffer_.data(), PageBytes);
                offset += PageBytes;
            }
            else
            {
                // The driver sends 640 bytes from its page buffer: what follows the page's 524 there is unk (zeros here)
                std::array<u8, 640> sent = {};
                std::memcpy(sent.data(), buffer_.data(), rawSize_);
                DbcSignalData2(device, offset, sent.data(), sent.size());
            }
        }
        DbcSignalData2(device, 0, status_.data(), 128);
        return Result();
    }

    // 2 (0x22D8)
    void WritePages()
    {
        u32 page = Req(0);
        u32 end = Req(0) + Req(4);
        u32 backup1 = Req(20), backup2 = Req(24);
        const u8* data = req_ + 40;
        do
        {
            u32 block = page - page % BlockPages;
            bool viaBackup = block != backup1 * BlockPages && block != backup2 * BlockPages && backup2 != 0;
            if (viaBackup)
            {
                s32 finished = FinishPending();
                SetResult(finished);
                if (finished == 0)
                {
                    return;
                }
                WriteMarker(backup2 * BlockPages, block / BlockPages);
                if (CopyBlock(static_cast<s32>(backup1), static_cast<s32>(block / BlockPages)) == 0)
                {
                    SetResult(0);
                    return;
                }
                WriteMarker(backup2 * BlockPages + 1, block / BlockPages);
            }
            EraseBlock(block);
            SetResult(1);
            for (u32 i = 0; i < BlockPages; i++)
            {
                if (i == page % BlockPages && page < end)
                {
                    std::memcpy(buffer_.data(), data, PageBytes);
                    MakeEcc(buffer_.data(), eccSize_ ? PageBytes : 0, buffer_.data() + PageBytes);
                    WritePage(page, buffer_.data());
                    page++;
                    data += PageBytes;
                }
                else if (viaBackup)
                {
                    ReadPage(backup1 * BlockPages + i, buffer_.data());
                    WritePage(block + i, buffer_.data());
                }
            }
            if (viaBackup)
            {
                EraseBlock(backup2 * BlockPages);
            }
        } while (page < end);
    }

    // The second backup block's first two pages name the block a write was copying (the rest of each is whatever the driver's
    // page buffer held: here too)
    void WriteMarker(u32 page, u32 block)
    {
        std::memcpy(buffer_.data(), &block, 4);
        MakeEcc(buffer_.data(), eccSize_ ? PageBytes : 0, buffer_.data() + PageBytes);
        WritePage(page, buffer_.data());
    }

    // 5 (0x2D20): 2 and the block when the second backup block's first two pages name the same one, 1 when it's erased (or
    // made so), 0 when it names a block past the card
    s32 PendingBlock(s32& block)
    {
        block = -1;
        u32 backup2 = Req(24) * BlockPages;
        ReadPage(backup2, buffer_.data());
        s32 first;
        std::memcpy(&first, buffer_.data(), 4);
        if (first == -1)
        {
            return 1;
        }
        if ((static_cast<s64>(first) + 1) * BlockPages > Req(16))
        {
            return 0;
        }
        ReadPage(backup2 + 1, buffer_.data());
        s32 second;
        std::memcpy(&second, buffer_.data(), 4);
        if (second != -1 && second == first)
        {
            block = first;
            return 2;
        }
        EraseBlock(backup2);
        return 1;
    }

    // 6 (0x2F24): a pending write finished, the block copied back from the first backup block
    s32 FinishPending()
    {
        s32 block;
        s32 pending = PendingBlock(block);
        if (pending != 2)
        {
            return pending;
        }
        if ((static_cast<s64>(block) + 1) * BlockPages > Req(16) || block < 0 ||
            CopyBlock(block, static_cast<s32>(Req(20))) == 0)
        {
            return 0;
        }
        EraseBlock(Req(24) * BlockPages);
        return 2;
    }

    // 0x2F88: a block erased and the other's pages written into it (0 for blocks past the card)
    s32 CopyBlock(s32 to, s32 from)
    {
        if ((static_cast<s64>(to) + 1) * BlockPages > Req(16) || to < 0 ||
            (static_cast<s64>(from) + 1) * BlockPages > Req(16) || from < 0)
        {
            return 0;
        }
        EraseBlock(static_cast<u32>(to) * BlockPages);
        for (u32 i = 0; i < BlockPages; i++)
        {
            ReadPage(static_cast<u32>(from) * BlockPages + i, buffer_.data());
            WritePage(static_cast<u32>(to) * BlockPages + i, buffer_.data());
        }
        return 1;
    }

    // 11 (0x2800)
    void FillPages()
    {
        for (u32 i = 0; i < Req(4); i++)
        {
            u32 page = Req(0) + i;
            if (page % 16 == 0)
            {
                EraseBlock(page);
            }
            std::memcpy(buffer_.data(), req_ + 40, PageBytes);
            MakeEcc(buffer_.data(), eccSize_ ? PageBytes : 0, buffer_.data() + PageBytes);
            WritePage(page, buffer_.data());
        }
    }

    // 10: sceCdReadClock's answer (1) and its sceCdCLOCK: status, second, minute, hour, 0, day, month, year (BCD), the
    // console's clock keeping Japan's time
    void ReadClock()
    {
        std::time_t now = std::time(nullptr) + 9 * 3600;
        std::tm t;
        gmtime_r(&now, &t);
        const u8 clock[8] = {0, Bcd(t.tm_sec), Bcd(t.tm_min), Bcd(t.tm_hour), 0, Bcd(t.tm_mday), Bcd(t.tm_mon + 1),
                             Bcd(t.tm_year % 100)};
        SetData(0, clock, 8);
    }

    // --- the image. Writes are the driver's own: it erases a block before writing its pages, and a page written is its bytes
    std::fstream& Image()
    {
        if (image_.is_open())
        {
            return image_;
        }
        fs::path path = fs::path(g_options.memcardDir) / "port0.ps2";
        std::error_code ec;
        if (!fs::exists(path, ec))
        {
            fs::create_directories(path.parent_path(), ec);
            std::ofstream blank(path, std::ios::binary);
            std::vector<u8> ff(RawPage * BlockPages, 0xFF);
            for (u32 b = 0; b < CardPages / BlockPages; b++)
            {
                blank.write(reinterpret_cast<const char*>(ff.data()), static_cast<std::streamsize>(ff.size()));
            }
            if (!blank)
            {
                Fatal("mc2: can't make the card %s", path.c_str());
            }
            Log("mc2: a blank card made: %s", path.c_str());
        }
        u64 size = fs::file_size(path, ec);
        if (ec || size != u64(RawPage) * CardPages)
        {
            Fatal("mc2: the card %s is %llu bytes; an 8 MB card's image is %u (16384 pages of 528)", path.c_str(),
                  static_cast<unsigned long long>(size), RawPage * CardPages);
        }
        image_.open(path, std::ios::binary | std::ios::in | std::ios::out);
        if (!image_)
        {
            Fatal("mc2: can't open the card %s", path.c_str());
        }
        Log("mc2: the card in slot 1 is %s", path.c_str());
        return image_;
    }

    void CheckPage(u32 page) const
    {
        if (page >= CardPages)
        {
            Fatal("mc2: page %u is past the card's %u", page, CardPages);
        }
    }

    // A page's first rawSize_ bytes (its data, and its ECC when the command has it)
    void ReadPage(u32 page, u8* to)
    {
        CheckPage(page);
        std::fstream& f = Image();
        f.seekg(static_cast<std::streamoff>(page) * RawPage);
        f.read(reinterpret_cast<char*>(to), rawSize_);
        if (!f)
        {
            Fatal("mc2: reading page %u of the card failed", page);
        }
    }

    void WritePage(u32 page, const u8* from)
    {
        CheckPage(page);
        std::fstream& f = Image();
        f.seekp(static_cast<std::streamoff>(page) * RawPage);
        f.write(reinterpret_cast<const char*>(from), rawSize_);
        f.flush();
        if (!f)
        {
            Fatal("mc2: writing page %u of the card failed", page);
        }
    }

    void EraseBlock(u32 page)
    {
        if (page % BlockPages != 0)
        {
            Fatal("mc2: an erase at page %u, not a block's first", page);
        }
        CheckPage(page);
        static const std::vector<u8> erased(RawPage * BlockPages, 0xFF);
        std::fstream& f = Image();
        f.seekp(static_cast<std::streamoff>(page) * RawPage);
        f.write(reinterpret_cast<const char*>(erased.data()), static_cast<std::streamsize>(erased.size()));
        f.flush();
        if (!f)
        {
            Fatal("mc2: erasing block %u of the card failed", page / BlockPages);
        }
    }

    const u8* req_ = nullptr;
    std::array<u8, 128> status_ = {};
    u32 commands_ = 0;
    u32 nextBuffer_ = 0;
    bool changed_ = true; // as the driver starts (0x0194), and again whenever it finds a card
    u32 eccSize_ = 0;
    u32 rawSize_ = PageBytes;
    std::array<u8, RawPage> buffer_ = {}; // the driver's page buffer (0x4438)
    std::fstream image_;
};
}

void Mc2ModuleLoaded(const u8* elf, size_t size)
{
    static const char version[] = "PsIImc2_s1  3020";
    if (std::search(elf, elf + size, version, version + sizeof(version) - 1) == elf + size)
    {
        Log("mc2: this mc2_s1 isn't 3020, the one whose card protocol is done (File 2's): no card in slot 1");
        return;
    }
    static Card card;
    if (card.device >= 0)
    {
        Log("mc2: a second mc2_s1 loaded: slot 1 already has its card");
        return;
    }
    // The device's description (0x1BC0): type 2, its second word 0
    card.device = DbcRegister(2, 0, &card);
    DbcConnect(card.device, CardPort, 0, "");
}
}
