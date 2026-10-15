// A CRI Sofdec movie's (.sfd) video: an MPEG program stream (pack headers 0x000001BA, MPEG-1's or MPEG-2's), its video in
// stream 0xE0; the rest (CRI's ADX sound in a private stream, padding) skipped
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace IpuTest
{
// The video elementary stream, or an error naming what's wrong with the file
bool DemuxVideo(const std::vector<uint8_t>& file, std::vector<uint8_t>& video, std::string& error);
}
