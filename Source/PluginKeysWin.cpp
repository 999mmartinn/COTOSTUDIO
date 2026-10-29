#include "PluginKeysWin.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <set>
#include <cwchar>
#include <cwctype>

namespace pluginkeys
{
    void (*onSpace)() = nullptr;

    static std::set<HWND> watched;
    static HHOOK hook = nullptr;
    static bool tookSpaceDown = false;

    /** The keyboard is in a native window inside one of our plugin windows (the plugin's
        editor), and nobody is typing text there. */
    static bool focusIsInPluginEditor()
    {
        const HWND focus = GetFocus();

        if (focus == nullptr)
            return false;

        bool inside = false;

        for (auto top : watched)
            if (focus != top && IsChild (top, focus))   // (the window itself: JUCE gets the key anyway)
                inside = true;

        if (! inside)
            return false;

        // A text field: a classic edit control, or anything showing a text caret.
        wchar_t cls[64] = {};
        GetClassNameW (focus, cls, 63);

        for (auto* p = cls; *p != 0; ++p)
            *p = (wchar_t) towlower (*p);

        if (std::wcsstr (cls, L"edit") != nullptr)
            return false;

        GUITHREADINFO info {};
        info.cbSize = sizeof (info);

        if (GetGUIThreadInfo (GetCurrentThreadId(), &info) && info.hwndCaret != nullptr)
            return false;

        return true;
    }

    static LRESULT CALLBACK keyHook (int code, WPARAM key, LPARAM flags)
    {
        if (code == HC_ACTION && key == VK_SPACE)
        {
            const bool released = (flags & 0x80000000) != 0;
            const bool repeat = (flags & 0x40000000) != 0;

            if (released)
            {
                if (tookSpaceDown)
                {
                    tookSpaceDown = false;
                    return 1;   // (its release too, so the plugin never sees half a key press)
                }
            }
            else if (GetKeyState (VK_CONTROL) >= 0 && GetKeyState (VK_MENU) >= 0 && focusIsInPluginEditor())
            {
                tookSpaceDown = true;

                if (! repeat && onSpace != nullptr)
                    onSpace();

                return 1;
            }
        }

        return CallNextHookEx (hook, code, key, flags);
    }

    void watch (void* topLevelWindow)
    {
        if (topLevelWindow == nullptr)
            return;

        watched.insert ((HWND) topLevelWindow);

        if (hook == nullptr)
            hook = SetWindowsHookExW (WH_KEYBOARD, keyHook, nullptr, GetCurrentThreadId());
    }

    void unwatch (void* topLevelWindow)
    {
        watched.erase ((HWND) topLevelWindow);

        if (watched.empty() && hook != nullptr)
        {
            UnhookWindowsHookEx (hook);
            hook = nullptr;
            tookSpaceDown = false;
        }
    }
}
