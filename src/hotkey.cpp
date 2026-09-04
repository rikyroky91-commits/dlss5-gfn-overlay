#include "hotkey.h"

#include <algorithm>
#include <cwctype>
#include <vector>

namespace gfn {
namespace {

std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t c : text) {
        if (c == separator) {
            parts.push_back(current);
            current.clear();
        } else if (c != L' ' && c != L'\t') {
            current.push_back(static_cast<wchar_t>(std::towlower(c)));
        }
    }
    parts.push_back(current);
    return parts;
}

}  // namespace

bool ParseHotkey(const std::wstring& text, Hotkey* hotkey) {
    if (text.empty()) return false;

    Hotkey parsed{};
    const std::vector<std::wstring> parts = Split(text, L'+');
    bool key_set = false;

    for (const std::wstring& part : parts) {
        if (part.empty()) return false;
        if (part == L"ctrl" || part == L"control") {
            parsed.modifiers |= MOD_CONTROL;
        } else if (part == L"alt") {
            parsed.modifiers |= MOD_ALT;
        } else if (part == L"shift") {
            parsed.modifiers |= MOD_SHIFT;
        } else if (part == L"win") {
            parsed.modifiers |= MOD_WIN;
        } else if (key_set) {
            return false;  // two non-modifier keys
        } else if (part.size() >= 2 && part[0] == L'f' && std::iswdigit(part[1])) {
            const int number = std::stoi(part.substr(1));
            if (number < 1 || number > 24) return false;
            parsed.virtual_key = VK_F1 + (number - 1);
            key_set = true;
        } else if (part.size() == 1 && std::iswalnum(part[0])) {
            parsed.virtual_key = static_cast<UINT>(std::towupper(part[0]));
            key_set = true;
        } else if (part == L"space") {
            parsed.virtual_key = VK_SPACE;
            key_set = true;
        } else if (part == L"insert") {
            parsed.virtual_key = VK_INSERT;
            key_set = true;
        } else if (part == L"home") {
            parsed.virtual_key = VK_HOME;
            key_set = true;
        } else if (part == L"end") {
            parsed.virtual_key = VK_END;
            key_set = true;
        } else if (part == L"pause") {
            parsed.virtual_key = VK_PAUSE;
            key_set = true;
        } else {
            return false;
        }
    }

    if (!key_set) return false;
    // MOD_NOREPEAT keeps a held key from firing the toggle dozens of times.
    parsed.modifiers |= MOD_NOREPEAT;
    *hotkey = parsed;
    return true;
}

bool RegisterGlobalHotkey(int id, const Hotkey& hotkey, std::string* error) {
    if (RegisterHotKey(nullptr, id, hotkey.modifiers, hotkey.virtual_key)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_HOTKEY_ALREADY_REGISTERED) {
        *error = "another application already owns this key combination";
    } else {
        *error = "RegisterHotKey failed with error " + std::to_string(code);
    }
    return false;
}

void UnregisterGlobalHotkey(int id) { UnregisterHotKey(nullptr, id); }

}  // namespace gfn
