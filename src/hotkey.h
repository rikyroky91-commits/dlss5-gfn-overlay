#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace gfn {

struct Hotkey {
    UINT modifiers = 0;
    UINT virtual_key = 0;
};

// Parses "ctrl+alt+F1", "alt+F4", "shift+p". Returns false on anything it does
// not recognise, so a typo in the config is reported rather than silently
// registering the wrong key.
bool ParseHotkey(const std::wstring& text, Hotkey* hotkey);

// Registers a global hotkey on the calling thread. Hotkeys arrive as WM_HOTKEY
// messages with `id` in wParam, so the thread that registers must be the one
// pumping messages.
bool RegisterGlobalHotkey(int id, const Hotkey& hotkey, std::string* error);
void UnregisterGlobalHotkey(int id);

}  // namespace gfn
