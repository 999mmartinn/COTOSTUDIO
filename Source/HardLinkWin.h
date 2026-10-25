#pragma once

/** Windows only: a second name for a file on the same drive (no extra space used).
    Kept apart from JUCE so <windows.h> doesn't meet the JUCE headers. */
namespace hardlink
{
    bool create (const wchar_t* newPath, const wchar_t* existingPath);
}
