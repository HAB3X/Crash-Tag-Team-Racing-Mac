#pragma once

// A PNG writer for the pictures the graphics dump and the harness keeps (RGBA bytes, no compression: stored deflate blocks)
#include <cstdint>
#include <string>

bool WritePng(const std::string& path, const uint32_t* pixels, uint32_t width, uint32_t height);
