#include "hooks/Meta.h"
#include "hooks/Locator.h"
#include <windows.h>
#include <map>
#include <memory>
#include <mutex>

namespace os::hooks::meta {

namespace {

std::mutex g_mx;
std::map<std::string, std::unique_ptr<ClassInfo>> g_byName;
std::map<uintptr_t, ClassInfo*> g_byRecord;

template <class T> bool Rd(uintptr_t a, T& v)
{
    __try { v = *reinterpret_cast<const volatile T*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool InImage(uintptr_t p)
{
    const Image& img = Locator::Get().Img();
    return p >= img.Base() && p < img.Base() + img.Size();
}

std::string Str(uintptr_t p)
{
    std::string s;
    char c;
    while (InImage(p) && s.size() < 256 && Rd(p++, c) && c) s += c;
    return s;
}

ClassInfo* Load(uintptr_t rec)
{
    if (auto it = g_byRecord.find(rec); it != g_byRecord.end()) return it->second;
    uintptr_t namePtr = 0, members = 0;
    uint32_t count = 0, size = 0;
    if (!Rd(rec, namePtr) || !Rd(rec + 0x18, members) || !Rd(rec + 0x20, count) || !Rd(rec + 0x24, size)) return nullptr;
    if (!InImage(namePtr) || !InImage(members) || count > 5000) return nullptr;
    auto ci = std::make_unique<ClassInfo>();
    ci->name = Str(namePtr);
    ci->size = size;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t e = members + i * 0x58, np = 0;
        Member m;
        if (!Rd(e, np) || !InImage(np)) continue; // not filled yet
        m.name = Str(np);
        Rd(e + 0x0C, m.type);
        Rd(e + 0x14, m.size);
        Rd(e + 0x18, m.count);
        Rd(e + 0x1C, m.offset);
        uintptr_t nested = 0;
        if (Rd(e + 0x20, nested) && InImage(nested)) m.cls = nested;
        uintptr_t et = 0;
        uint32_t en = 0;
        if (m.type == Enum && Rd(e + 0x30, et) && Rd(e + 0x38, en) && InImage(et) && en < 4096) {
            for (uint32_t k = 0; k < en; ++k) {
                int64_t v = 0;
                uintptr_t np2 = 0;
                if (!Rd(et + k * 16, v) || !Rd(et + k * 16 + 8, np2) || !InImage(np2)) break;
                m.enums.emplace_back((int)v, Str(np2));
            }
        }
        ci->members.push_back(std::move(m));
    }
    ClassInfo* raw = ci.get();
    g_byRecord[rec] = raw;
    g_byName[ci->name] = std::move(ci);
    return raw;
}

}

const Member* ClassInfo::Find(const std::string& n) const
{
    for (auto& m : members) if (m.name == n) return &m;
    return nullptr;
}

const ClassInfo* FromRecord(uintptr_t record)
{
    std::lock_guard lk(g_mx);
    return Load(record);
}

const ClassInfo* Get(const std::string& cls)
{
    std::lock_guard lk(g_mx);
    if (auto it = g_byName.find(cls); it != g_byName.end() && !it->second->members.empty()) return it->second.get();
    const Image& img = Locator::Get().Img();
    for (uint32_t s : img.FindString(cls))
        for (uint32_t r : img.PtrRefs(s)) {
            if (img.SectionOf(r) != ".rdata") continue;
            if (ClassInfo* ci = Load(img.Base() + r); ci && ci->name == cls && !ci->members.empty()) return ci;
        }
    return nullptr;
}

std::vector<std::pair<int, std::string>> EnumNames(const std::string& cls, const std::string& field)
{
    auto m = Path(cls, field);
    if (!m) return {};
    if (!m->enums.empty()) return m->enums;
    if (const ClassInfo* inner = m->cls ? FromRecord(m->cls) : nullptr)
        for (auto& im : inner->members)
            if (!im.enums.empty()) return im.enums;
    return {};
}

std::optional<Member> Path(const std::string& cls, const std::string& path)
{
    const ClassInfo* ci = Get(cls);
    uint32_t base = 0;
    size_t pos = 0;
    while (ci) {
        size_t dot = path.find('.', pos);
        const Member* m = ci->Find(path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos));
        if (!m) return std::nullopt;
        if (dot == std::string::npos) {
            Member out = *m;
            out.offset += base;
            return out;
        }
        base += m->offset;
        ci = m->cls ? FromRecord(m->cls) : nullptr;
        pos = dot + 1;
    }
    return std::nullopt;
}

}
