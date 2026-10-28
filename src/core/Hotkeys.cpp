#include "core/Hotkeys.h"
#include <windows.h>

namespace os {

Hotkeys& Hotkeys::Get()
{
    static Hotkeys h;
    static bool init = (h.Reset(), true);
    (void)init;
    return h;
}

void Hotkeys::Reset()
{
    m_binds.clear();
    m_binds[MenuId] = { VK_F8, 0 };
}

Chord Hotkeys::Of(const std::string& id) const
{
    auto it = m_binds.find(id);
    return it == m_binds.end() ? Chord{} : it->second;
}

std::vector<std::string> Hotkeys::ConflictsOf(const std::string& id) const
{
    std::vector<std::string> out;
    Chord c = Of(id);
    if (c.Empty()) return out;
    for (auto& [k, v] : m_binds)
        if (k != id && v == c) out.push_back(k);
    return out;
}

uint8_t Hotkeys::CurrentMods()
{
    uint8_t m = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= ModCtrl;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= ModShift;
    if (GetKeyState(VK_MENU) & 0x8000) m |= ModAlt;
    return m;
}

static bool IsModifier(uint16_t vk)
{
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: return true;
    default: return false;
    }
}

std::string Hotkeys::OnKeyDown(uint16_t vk, bool& consumed)
{
    consumed = false;
    if (!m_capturing.empty())
    {
        if (IsModifier(vk)) { consumed = true; return {}; }
        consumed = true;
        if (vk == VK_ESCAPE) { m_capturing.clear(); return {}; }
        if (vk == VK_BACK || vk == VK_DELETE) {
            if (m_capturing != MenuId) m_binds.erase(m_capturing);
            m_capturing.clear();
            return {};
        }
        m_binds[m_capturing] = { vk, CurrentMods() };
        m_capturing.clear();
        return {};
    }
    if (IsModifier(vk)) return {};
    Chord pressed{ vk, CurrentMods() };
    for (auto& [id, c] : m_binds)
        if (c == pressed) { consumed = true; return id; }
    return {};
}

std::string Hotkeys::Name(Chord c)
{
    if (c.Empty()) return {};
    std::string s;
    if (c.mods & ModCtrl) s += "Ctrl + ";
    if (c.mods & ModShift) s += "Shift + ";
    if (c.mods & ModAlt) s += "Alt + ";
    switch (c.vk) {
    case VK_LBUTTON: return s + "Mouse 1";
    case VK_RBUTTON: return s + "Mouse 2";
    case VK_MBUTTON: return s + "Mouse 3";
    case VK_XBUTTON1: return s + "Mouse 4";
    case VK_XBUTTON2: return s + "Mouse 5";
    }
    UINT sc = MapVirtualKeyW(c.vk, MAPVK_VK_TO_VSC);
    switch (c.vk) {
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
        sc |= 0x100; break;
    }
    char buf[64] = {};
    if (GetKeyNameTextA((LONG)(sc << 16), buf, sizeof buf) > 0) return s + buf;
    return s + "Key " + std::to_string(c.vk);
}

}
