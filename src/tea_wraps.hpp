#ifndef TEA_WRAPS_HPP
#define TEA_WRAPS_HPP

#include <windows.h>
#include <filesystem>
#include <iosfwd>

int RunTea(HINSTANCE instance, int show);

void SleepThread(float f);

namespace tea
{
void PreserveUnreadableFile(const std::filesystem::path& path);
}

struct Coordinates
{
    int pos_x;
    int pos_y;

    Coordinates(int x = 0, int y = 0);

    bool operator==(const Coordinates& other) const;
};

std::ostream& operator<<(std::ostream& out, const Coordinates& cords);

#endif
