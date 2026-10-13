#include "png.h"

#include <cstdio>
#include <vector>

namespace
{
uint32_t Crc(const uint8_t* data, size_t size, uint32_t crc = 0)
{
    static uint32_t table[256];
    if (table[1] == 0)
    {
        for (uint32_t n = 0; n < 256; n++)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }

            table[n] = c;
        }
    }

    crc = ~crc;
    for (size_t i = 0; i < size; i++)
    {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return ~crc;
}

void Put32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
{
    Put32(out, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    Put32(out, Crc(body.data(), body.size()));
}
}

bool WritePng(const std::string& path, const uint32_t* pixels, uint32_t width, uint32_t height)
{
    std::vector<uint8_t> raw;
    raw.reserve((width * 4 + 1) * height);
    for (uint32_t y = 0; y < height; y++)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; x++)
        {
            uint32_t p = pixels[y * width + x];
            raw.push_back(static_cast<uint8_t>(p));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(255);
        }
    }

    // zlib: a header, stored blocks of up to 65535 bytes, Adler-32
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t at = 0; at < raw.size() || raw.empty();)
    {
        size_t size = raw.size() - at < 65535 ? raw.size() - at : 65535;
        bool last = at + size == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(size));
        z.push_back(static_cast<uint8_t>(size >> 8));
        z.push_back(static_cast<uint8_t>(~size));
        z.push_back(static_cast<uint8_t>(~size >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(at), raw.begin() + static_cast<long>(at + size));
        at += size;
        if (last)
        {
            break;
        }
    }

    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : raw)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }

    Put32(z, b << 16 | a);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> header;
    Put32(header, width);
    Put32(header, height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    Chunk(out, "IHDR", header);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        return false;
    }

    std::fwrite(out.data(), 1, out.size(), file);
    std::fclose(file);
    return true;
}
