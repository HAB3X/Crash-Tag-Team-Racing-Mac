// --control FIFO: commands taken while the game runs (a script of keys can't look at the picture; this can: write a command,
// ask for a snapshot, read it, decide the next). One command a line:
//   key NAME[+NAME] [HOLD]   keys (bindings.txt's names: W, Left Shift, Mouse Left) held HOLD game seconds (0.3)
//   tap|down|up NAME[+NAME]  SDL key or mouse button events (tap: pressed and let go at once, as a very short press)
//   mouse X Y                the mouse over the picture (0-1 across and down), wheel DY: the wheel
//   spacing FRAMES           ps2_set_tap_spacing: the least time between two taps' starts
//   pads                     the presses the pad server took and the frames showed (--log input has each)
//   snap NAME                a picture, NAME.<n>.png in the --snapshots folder, at the next vblank
//   peek ADDR [WORDS]        guest words (hex address) to the log
//   ram FILE                 the 32 MB of main memory written to FILE (to look for what the game loaded: texts, tables)
//   record VBLANKS FILE      --gs-record from the next vblank
//   stop                     the run ends as --run-seconds ends it (the VU1 counts and reports are logged)
//   quit                     the runtime stops at once
#include "runtime.h"

#include "ps2/runtime.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <sstream>
#include <thread>

namespace Ps2
{
namespace
{
// NAME[+NAME] as the key names (those with a space are written with an underscore: Left_Shift)
std::vector<std::string> Names(const std::string& joined)
{
    std::vector<std::string> keys;
    size_t start = 0;
    while (start <= joined.size())
    {
        size_t plus = joined.find('+', start);
        std::string one = joined.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        std::replace(one.begin(), one.end(), '_', ' ');
        if (!one.empty())
        {
            keys.push_back(one);
        }
        if (plus == std::string::npos)
        {
            break;
        }
        start = plus + 1;
    }
    return keys;
}

void Handle(const std::string& line)
{
    std::istringstream in(line);
    std::string command;
    in >> command;
    if (command == "key")
    {
        std::string names;
        double hold = 0.3;
        in >> names;
        std::string h;
        if (in >> h)
        {
            hold = atof(h.c_str());
        }
        std::vector<std::string> keys = Names(names);
        InputLiveKeys(keys, hold);
    }
    else if (command == "tap" || command == "down" || command == "up")
    {
        std::string names;
        in >> names;
        for (const std::string& name : Names(names))
        {
            InputLiveButton(name, command != "up", command != "down");
        }
    }
    else if (command == "mouse")
    {
        float x = 0, y = 0;
        in >> x >> y;
        InputLiveMouse(x, y);
    }
    else if (command == "move")
    {
        float dx = 0, dy = 0;
        in >> dx >> dy;
        InputLiveMove(dx, dy);
    }
    else if (command == "wheel")
    {
        float lines = 0;
        in >> lines;
        InputLiveWheel(lines);
    }
    else if (command == "spacing")
    {
        int frames = 0;
        in >> frames;
        ps2_set_tap_spacing(frames);
    }
    else if (command == "pads")
    {
        Log("input: %llu presses seen by the pad server, %llu shown to the game", static_cast<unsigned long long>(PadPressesSeen()),
            static_cast<unsigned long long>(PadPressesShown()));
    }
    else if (command == "snap")
    {
        std::string name;
        in >> name;
        GsLiveSnapshot(name.empty() ? "live" : name);
    }
    else if (command == "peek")
    {
        std::string address, words = "1";
        in >> address >> words;
        u32 at = static_cast<u32>(strtoul(address.c_str(), nullptr, 16));
        std::string line;
        for (int n = 0; n < atoi(words.c_str()); n++)
        {
            char item[16];
            snprintf(item, sizeof(item), " %08x", Read32(at + 4u * static_cast<u32>(n)));
            line += item;
        }
        Log("peek %08x:%s", at, line.c_str());
    }
    else if (command == "ram" || command == "dump")
    {
        std::string file;
        in >> file;
        FILE* f = fopen(file.c_str(), "wb");
        if (!f || fwrite(Ram(), 1, 32u << 20, f) != (32u << 20))
        {
            Log("--control: cannot write main memory to %s", file.c_str());
        }
        if (f)
        {
            fclose(f);
        }
    }
    else if (command == "record")
    {
        u64 vblanks = 0;
        std::string file;
        in >> vblanks >> file;
        GsLiveRecord(file, vblanks);
    }
    else if (command == "stop")
    {
        GsLiveStop();
    }
    else if (command == "quit")
    {
        _exit(0);
    }
    else if (!command.empty())
    {
        Log("--control: no command \"%s\"", command.c_str());
    }
}
}

void ControlStart(const std::string& path)
{
    unlink(path.c_str());
    if (mkfifo(path.c_str(), 0600) != 0)
    {
        Log("--control: cannot make %s", path.c_str());
        return;
    }
    std::thread([path] {
        for (;;)
        {
            // Opened for reading and writing, so it never sees an end of file when a writer closes
            int fd = open(path.c_str(), O_RDWR);
            if (fd < 0)
            {
                return;
            }
            std::string line;
            char c;
            while (read(fd, &c, 1) == 1)
            {
                if (c == '\n')
                {
                    Handle(line);
                    line.clear();
                }
                else
                {
                    line += c;
                }
            }
            close(fd);
        }
    }).detach();
}
}
