#pragma once

// What the hardware layer asks of the program it's in (the Twinsanity port's native.h, the part it uses): the folder its settings
// are kept in (the replacement textures are under it, in textures/)

#include <string>

namespace Native
{
std::string SettingsFolder();
}
