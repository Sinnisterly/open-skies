#include "hooks/Locator.h"
#include "core/AppLog.h"
#include <nlohmann/json.hpp>
#include <Zydis/Zydis.h>
#include <functional>
#include <algorithm>
#include <cctype>
#include <set>

#include "EmbeddedTargets.inc"

using nlohmann::json;

namespace os::hooks {

namespace {

json g_doc;
std::map<std::string, const json*> g_byName;
std::set<std::string> g_busy;

std::optional<Insn> FindBack(const Image& img, uint32_t site, uint32_t maxBack, const std::function<bool(const Insn&)>& pred)
{
    auto f = img.FuncOf(site);
    if (!f) return std::nullopt;
    for (auto& [a, b] : img.FuncRanges(*f)) {
        if (site < a || site >= b) continue;
        auto insns = img.DecodeRange(a, site);
        for (auto it = insns.rbegin(); it != insns.rend(); ++it) {
            if (site - it->rva > maxBack) break;
            if (pred(*it)) return *it;
        }
    }
    return std::nullopt;
}

const char* KindName(Operand::Kind k)
{
    switch (k) { case Operand::Reg: return "reg"; case Operand::Mem: return "mem"; case Operand::Imm: return "imm"; default: return "?"; }
}

std::string RegName(int r)
{
    const char* n = ZydisRegisterGetString((ZydisRegister)r);
    return n ? n : "";
}

}

}

namespace os::hooks {

Locator& Locator::Get()
{
    static Locator l;
    return l;
}

bool Locator::Init()
{
    if (m_ready) return true;
    if (!m_img.Load()) { log::Error("Locator", "Could not read NMS.exe image"); return false; }
    // Merge every embedded target file; the first definition of a name wins.
    g_doc = json{ { "targets", json::array() } };
    int build = 0;
    for (const char* text : kTargetFiles) {
        json part = json::parse(text, nullptr, false);
        if (part.is_discarded() || !part.contains("targets")) { log::Error("Locator", "A target file is invalid, skipped"); continue; }
        if (!build) build = part.value("game_build", 0);
        for (auto& t : part["targets"]) g_doc["targets"].push_back(t);
    }
    for (auto& t : g_doc["targets"]) {
        std::string n = t.value("name", "");
        if (n.empty() || g_byName.count(n)) { if (!n.empty()) log::Warn("Locator", "Duplicate target {} ignored", n); continue; }
        g_byName[n] = &t;
    }
    if (g_byName.empty()) { log::Error("Locator", "No targets loaded"); return false; }
    log::Info("Locator", "{} targets loaded (written for build {})", g_byName.size(), build);
    m_ready = true;
    return true;
}

static std::vector<uint32_t> Hop(const Image& img, const std::vector<uint32_t>& funcs, const json& h)
{
    std::set<uint32_t> out;
    for (uint32_t f : funcs) {
        if (h.contains("callees")) for (uint32_t g : img.Callees(f, h["callees"].get<int>())) out.insert(g);
        else if (h.contains("callers")) for (uint32_t g : img.Callers(f)) out.insert(g);
    }
    return { out.begin(), out.end() };
}

static std::vector<uint32_t> ApplySite(const Image& img, const json& t, uint32_t func)
{
    json site = t.value("site", json{ { "kind", "entry" } });
    auto ranges = img.FuncRanges(func);
    if (site.value("kind", "entry") == "entry") {
        if (site.contains("verify")) {
            auto p = Pattern::Parse(site["verify"].get<std::string>());
            if (!p || img.Scan(*p, ranges, 1).empty()) return {};
        }
        return { func };
    }
    auto p = Pattern::Parse(site["pattern"].get<std::string>());
    if (!p) return {};
    auto hits = img.Scan(*p, ranges, 8);
    if (site.contains("index")) {
        size_t i = site["index"].get<size_t>();
        hits = i < hits.size() ? std::vector<uint32_t>{ hits[i] } : std::vector<uint32_t>{};
    }
    int off = site.value("offset", 0);
    for (auto& h : hits) h += off;
    return hits;
}

const Target& Locator::Resolve(const std::string& name)
{
    if (auto it = m_cache.find(name); it != m_cache.end()) return it->second;
    Target& out = m_cache[name];
    out.name = name;
    auto jt = g_byName.find(name);
    if (!m_ready || jt == g_byName.end()) { out.reason = "unknown target"; return out; }
    if (g_busy.count(name)) { out.reason = "anchor cycle"; return out; }
    g_busy.insert(name);
    const json& t = *jt->second;

    std::optional<uint32_t> site;
    int idx = 0;
    for (auto& a : t["anchors"]) {
        std::vector<uint32_t> hits;
        try {
            std::string k = a["kind"];
            std::string yields = "func";
            std::vector<uint32_t> base;
            if (k == "aob" || k == "sig") {
                auto p = Pattern::Parse(a["pattern"].get<std::string>());
                if (p) for (uint32_t h : m_img.Scan(*p, m_img.Code(), 16)) base.push_back(h + a.value("offset", 0));
                yields = k == "sig" ? "func" : a.value("yields", "site");
                if (yields == "owner") {
                    std::set<uint32_t> fs;
                    for (uint32_t h : base) if (auto f = m_img.FuncOf(h)) fs.insert(*f);
                    base.assign(fs.begin(), fs.end());
                    yields = "func";
                }
            }
            else if (k == "rtti_vfunc") {
                std::set<uint32_t> fs;
                for (uint32_t vt : m_img.Vtables(a["class"].get<std::string>())) {
                    uint64_t v = m_img.Read<uint64_t>(vt + 8 * a["index"].get<uint32_t>());
                    if (v < m_img.Base()) continue;
                    uint32_t f = (uint32_t)(v - m_img.Base());
                    if (!m_img.InCode(f)) continue;
                    fs.insert(m_img.FuncOf(f).value_or(f));
                }
                base.assign(fs.begin(), fs.end());
            }
            else if (k == "string_xref") {
                std::set<uint32_t> fs;
                for (uint32_t s : m_img.FindString(a["string"].get<std::string>()))
                    for (uint32_t r : m_img.RipRefs(s))
                        if (auto f = m_img.FuncOf(r)) fs.insert(*f);
                base.assign(fs.begin(), fs.end());
            }
            else if (k == "target") {
                const Target& dep = Resolve(a["name"].get<std::string>());
                if (dep.ok) base.push_back(dep.func);
            }
            if (yields == "site") hits = base;
            else {
                if (a.contains("hops")) for (auto& h : a["hops"]) base = Hop(m_img, base, h);
                std::set<uint32_t> sites;
                for (uint32_t f : base) for (uint32_t s : ApplySite(m_img, t, f)) sites.insert(s);
                hits.assign(sites.begin(), sites.end());
            }
        }
        catch (...) { hits.clear(); }
        log::Debug("Locator", "{} anchor #{} {} -> {} hit(s)", name, idx, a.value("kind", "?"), hits.size());
        if (hits.size() == 1) { site = hits[0]; out.anchor = idx; break; }
        ++idx;
    }
    g_busy.erase(name);

    if (!site) { out.reason = "no anchor found a unique match"; log::Warn("Locator", "{}: {}", name, out.reason); return out; }
    out.site = *site;
    bool entry = t.value("site", json{ { "kind", "entry" } }).value("kind", "entry") == "entry";
    out.func = entry ? *site : m_img.FuncOf(*site).value_or(*site);

    // Shape check, then pull the struct offset out of the code.
    auto in = m_img.Decode(*site);
    if (!in) { out.reason = "site does not decode"; return out; }
    out.text = in->mnemonic;
    if (t.contains("shape")) {
        const json& sh = t["shape"];
        if (sh.contains("mnemonic") && sh["mnemonic"].get<std::string>() != in->mnemonic) {
            out.reason = "expected " + sh["mnemonic"].get<std::string>() + ", found " + in->mnemonic;
            log::Warn("Locator", "{}: {}", name, out.reason);
            return out;
        }
        if (sh.contains("operands")) {
            std::vector<std::string> want = sh["operands"];
            bool same = want.size() == in->ops.size();
            for (size_t i = 0; same && i < want.size(); ++i) same = want[i] == KindName(in->ops[i].kind);
            if (!same) { out.reason = "operand shape changed"; log::Warn("Locator", "{}: {}", name, out.reason); return out; }
        }
        if (sh.contains("offset_operand")) {
            std::optional<Insn> src = in;
            for (int i = 0; src && i < sh.value("offset_insn", 0); ++i) src = m_img.Decode(src->rva + src->len);
            size_t oi = sh["offset_operand"].get<size_t>();
            if (!src || oi >= src->ops.size()) { out.reason = "offset operand missing"; return out; }
            if (src->ops[oi].kind == Operand::Mem) out.offset = src->ops[oi].disp;
            else if (src->ops[oi].kind == Operand::Imm) out.offset = src->ops[oi].imm;
            else { out.reason = "offset operand missing"; return out; }
        }
        if (sh.contains("pointer_from")) {
            std::string reg = sh["pointer_from"].get<std::string>();
            reg = reg.substr(reg.find("lea ") + 4);
            reg = reg.substr(0, reg.find(','));
            auto lea = FindBack(m_img, *site, 0x40, [&](const Insn& i) {
                return i.mnemonic == "lea" && i.ops.size() == 2 && i.ops[0].kind == Operand::Reg && RegName(i.ops[0].reg) == reg;
            });
            if (!lea || lea->ops[1].disp == 0) { out.reason = "pointer setup not found"; return out; }
            out.offset = lea->ops[1].disp;
        }
    }
    if (t.contains("shape") && t["shape"].contains("rip_operand") && !t.contains("global_field")) {
        if (!in->ripTarget) { out.reason = "site no longer reads a global"; return out; }
        out.global = m_img.Base() + *in->ripTarget;
    }
    if (t.contains("base_load")) {
        std::string reg = t["base_load"]["register"];
        auto mv = FindBack(m_img, *site, t["base_load"].value("max_back", 32), [&](const Insn& i) {
            return i.mnemonic == "mov" && i.ops.size() == 2 && i.ops[0].kind == Operand::Reg && RegName(i.ops[0].reg) == reg && i.ripTarget;
        });
        if (!mv) { out.reason = "base pointer load not found"; return out; }
        out.global = m_img.Base() + *mv->ripTarget;
    }
    if (t.contains("global_field")) {
        const json& gf = t["global_field"];
        auto off = FieldOffset(gf["class"], gf["field"]);
        auto at = m_img.Decode(*site + gf.value("site_delta", 0));
        if (!off) { out.reason = "field " + gf["field"].get<std::string>() + " not in the game metadata"; log::Warn("Locator", "{}: {}", name, out.reason); return out; }
        if (!at || !at->ripTarget) { out.reason = "site no longer reads a global"; log::Warn("Locator", "{}: {}", name, out.reason); return out; }
        uintptr_t inst = GlobalsInstance(gf["class"]);
        uintptr_t read = m_img.Base() + *at->ripTarget;
        if (inst && inst + *off != read) { out.reason = "site no longer reads the named field"; return out; }
        out.global = read;
        out.offset = *off;
    }
    if (t.contains("struct_fields")) {
        const json& sf = t["struct_fields"];
        for (auto& f : sf["fields"]) {
            auto off = FieldOffset(sf["class"], f);
            if (!off) { out.reason = "struct field " + f.get<std::string>() + " not found"; return out; }
            out.fields[f] = *off;
        }
    }
    out.ok = true;
    log::Info("Locator", "{} at NMS.exe+{:X} via anchor #{} ({}{})", name, out.site, out.anchor, out.text,
              out.offset ? std::format(", off 0x{:X}", *out.offset) : "");
    return out;
}

std::optional<uint32_t> Locator::FieldOffset(const std::string& cls, const std::string& field)
{
    for (uint32_t s : m_img.FindString(cls)) {
        for (uint32_t rec : m_img.PtrRefs(s)) {
            if (m_img.SectionOf(rec) != ".rdata") continue;
            uint64_t members = m_img.Read<uint64_t>(rec + 24);
            uint32_t count = m_img.Read<uint32_t>(rec + 32);
            if (members < m_img.Base() || count > 5000) continue;
            uint32_t mt = (uint32_t)(members - m_img.Base());
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t e = mt + i * 0x58;
                uint64_t np = m_img.Read<uint64_t>(e);
                if (np < m_img.Base() || np >= m_img.Base() + m_img.Size()) continue;
                if (strcmp((const char*)np, field.c_str()) == 0) return m_img.Read<uint32_t>(e + 0x1C);
            }
        }
    }
    return std::nullopt;
}

uintptr_t Locator::GlobalsInstance(const std::string& cls)
{
    if (auto it = m_instances.find(cls); it != m_instances.end()) return it->second;
    std::string shortName = cls[0] == 'c' ? cls.substr(1) : cls;
    std::string lower = shortName;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)tolower(c); });
    std::set<uint32_t> found;
    for (const std::string& path : { "/" + shortName + ".global.mbin", "/" + lower + ".global.mbin", "/" + shortName + ".globals.mbin", "/" + shortName + ".mbin" }) {
        Pattern p;
        for (char c : path) p.bytes.push_back((uint8_t)c);
        p.bytes.push_back(0);
        for (uint32_t h : m_img.Scan(p, m_img.ReadOnly(), 4)) {
            uint32_t start = h;
            while (start > 0 && *m_img.At(start - 1) != 0) --start;
            for (uint32_t slot : m_img.PtrRefs(start))
                for (uint32_t r : m_img.RipRefs(slot)) {
                    auto mv = m_img.Decode(r);
                    if (!mv || mv->mnemonic != "mov" || mv->ops.empty() || RegName(mv->ops[0].reg) != "rdx") continue;
                    auto lea = m_img.Decode(r + mv->len);
                    if (lea && lea->mnemonic == "lea" && RegName(lea->ops[0].reg) == "rcx" && lea->ripTarget) found.insert(*lea->ripTarget);
                }
        }
    }
    uintptr_t inst = found.size() == 1 ? m_img.Base() + *found.begin() : 0;
    if (!inst) log::Warn("Locator", "{} instance not found ({} candidates)", cls, found.size());
    m_instances[cls] = inst;
    return inst;
}

}
