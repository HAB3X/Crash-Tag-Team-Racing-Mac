// --control FIFO: commands taken while the game runs (a script of keys can't look at the picture; this can: write a command,
// ask for a snapshot, read it, decide the next). One command a line:
//   key NAME[+NAME] [HOLD]   keys (bindings.txt's names: W, Left Shift, Mouse Left) held HOLD game seconds (0.3)
//   snap NAME                a picture, NAME.<n>.png in the --snapshots folder, at the next vblank
//   record VBLANKS FILE      --gs-record from the next vblank
//   stop                     the run ends as --run-seconds ends it (the VU1 counts and reports are logged)
//   quit                     the runtime stops at once
#include "runtime.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sstream>
#include <thread>

namespace Ps2
{
namespace
{
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
        std::vector<std::string> keys;
        size_t start = 0;
        while (start <= names.size())
        {
            size_t plus = names.find('+', start);
            std::string one = names.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            // names with a space are written with an underscore (Left_Shift)
            for (char& c : one)
            {
                if (c == '_')
                {
                    c = ' ';
                }
            }
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
        InputLiveKeys(keys, hold);
    }
    else if (command == "snap")
    {
        std::string name;
        in >> name;
        GsLiveSnapshot(name.empty() ? "live" : name);
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
