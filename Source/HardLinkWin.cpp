#include "HardLinkWin.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace hardlink
{
    bool create (const wchar_t* newPath, const wchar_t* existingPath)
    {
        return CreateHardLinkW (newPath, existingPath, nullptr) != FALSE;
    }
}
