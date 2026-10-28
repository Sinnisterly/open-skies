#pragma once
#include "hooks/Image.h"
#include <map>
#include <optional>
#include <string>

namespace os::hooks {

struct Target
{
    std::string name;
    bool ok = false;
    uint32_t site = 0, func = 0;   // RVAs
    int anchor = -1;               // index of the anchor that hit
    std::string text;              // decoded site instruction
    std::optional<int64_t> offset; // struct offset read from the code
    uintptr_t global = 0;          // data targets: absolute address of the field or base pointer slot
    std::map<std::string, uint32_t> fields; // struct_fields resolved by name
    std::string reason;            // why it is unavailable
};

// Resolves the named targets from game/targets.json against the running game.
class Locator
{
public:
    static Locator& Get();
    bool Init();
    const Target& Resolve(const std::string& name);
    const Image& Img() const { return m_img; }
    void Finish() { m_img.DropIndexes(); }

    // Gc*Globals metadata, by libMBIN field name.
    std::optional<uint32_t> FieldOffset(const std::string& cls, const std::string& field);
    uintptr_t GlobalsInstance(const std::string& cls);

private:
    Image m_img;
    std::map<std::string, Target> m_cache;
    std::map<std::string, uintptr_t> m_instances;
    bool m_ready = false;
};

}
