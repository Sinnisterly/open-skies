#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os {

enum : uint8_t { ModCtrl = 1, ModShift = 2, ModAlt = 4 };

struct Chord
{
    uint16_t vk = 0;
    uint8_t mods = 0;
    bool Empty() const { return vk == 0; }
    bool operator==(const Chord&) const = default;
};

class Hotkeys
{
public:
    static Hotkeys& Get();

    static constexpr const char* MenuId = "menu.open";
    static constexpr const char* PanicId = "menu.panic";

    void Set(const std::string& id, Chord c) { if (c.Empty()) m_binds.erase(id); else m_binds[id] = c; }
    Chord Of(const std::string& id) const;
    std::vector<std::string> ConflictsOf(const std::string& id) const;
    const std::map<std::string, Chord>& All() const { return m_binds; }
    void Reset();

    static std::string Name(Chord c);
    static uint8_t CurrentMods();

    // Rebind flow: Begin, then the next key press lands via OnKeyDown.
    void BeginCapture(const std::string& id) { m_capturing = id; }
    void CancelCapture() { m_capturing.clear(); }
    const std::string& Capturing() const { return m_capturing; }

    // Returns the feature id to trigger, or empty. Sets consumed when the key was used.
    std::string OnKeyDown(uint16_t vk, bool& consumed);

private:
    std::map<std::string, Chord> m_binds;
    std::string m_capturing;
};

}
