#include <cmath>
#include <algorithm>
#include "features/Bind.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include <deque>
#include <map>

namespace os::features {

namespace {
std::vector<Bound> g_out;
std::deque<safetyhook::MidHook> g_mids;
uintptr_t g_ret = 0; // a lone ret, used to skip a function from its entry
std::deque<DataFx> g_data;
}

uintptr_t RetGadget() { return g_ret; }


safetyhook::Xmm* XmmOf(Context& c, int reg)
{
    int i = reg - ZYDIS_REGISTER_XMM0;
    return i >= 0 && i < 16 ? &(&c.xmm0)[i] : nullptr;
}

uintptr_t* GprOf(Context& c, int reg)
{
    switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, (ZydisRegister)reg)) {
    case ZYDIS_REGISTER_RAX: return &c.rax;
    case ZYDIS_REGISTER_RBX: return &c.rbx;
    case ZYDIS_REGISTER_RCX: return &c.rcx;
    case ZYDIS_REGISTER_RDX: return &c.rdx;
    case ZYDIS_REGISTER_RSI: return &c.rsi;
    case ZYDIS_REGISTER_RDI: return &c.rdi;
    case ZYDIS_REGISTER_RBP: return &c.rbp;
    case ZYDIS_REGISTER_R8:  return &c.r8;
    case ZYDIS_REGISTER_R9:  return &c.r9;
    case ZYDIS_REGISTER_R10: return &c.r10;
    case ZYDIS_REGISTER_R11: return &c.r11;
    case ZYDIS_REGISTER_R12: return &c.r12;
    case ZYDIS_REGISTER_R13: return &c.r13;
    case ZYDIS_REGISTER_R14: return &c.r14;
    case ZYDIS_REGISTER_R15: return &c.r15;
    default: return nullptr;
    }
}

bool IsGpr(int reg)
{
    Context c{};
    return GprOf(c, reg) != nullptr;
}

bool IsXmm(int reg) { return reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM15; }

void Fail(const char* id, const std::string& why)
{
    Bound b;
    b.id = id;
    b.reason = why;
    g_out.push_back(std::move(b));
    log::Warn("Feature", "{} unavailable: {}", id, why);
}

void Ok(Bound b) { b.ok = true; g_out.push_back(std::move(b)); }

const Target* Need(const char* name, std::initializer_list<const char*> ids)
{
    const Target& t = Loc().Resolve(name);
    if (t.ok) return &t;
    for (auto* id : ids) Fail(id, t.reason.empty() ? "target not found" : t.reason);
    return nullptr;
}

std::optional<Insn> Dec(uint32_t rva) { return Img().Decode(rva); }
std::optional<Insn> Next(const Insn& i) { return Img().Decode(i.rva + i.len); }

std::optional<uint32_t> ScanFunc(uint32_t func, const char* pat)
{
    auto p = hooks::Pattern::Parse(pat);
    if (!p) return std::nullopt;
    auto hits = Img().Scan(*p, Img().FuncRanges(func), 2);
    if (hits.size() != 1) return std::nullopt;
    return hits[0];
}

// ---- code edits ----

safetyhook::MidHook* Mid(uint32_t rva, safetyhook::MidHookFn fn)
{
    auto h = safetyhook::MidHook::create(reinterpret_cast<void*>(Abs(rva)), fn, safetyhook::MidHook::StartDisabled);
    if (!h) return nullptr;
    g_mids.push_back(std::move(*h));
    return &g_mids.back();
}

std::function<bool(bool)> HookToggle(safetyhook::MidHook* h)
{
    return [h](bool on) { return (on ? h->enable() : h->disable()).has_value(); };
}

bool WriteCode(uintptr_t at, const uint8_t* src, size_t n)
{
    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(reinterpret_cast<void*>(at), src, n);
    VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    return true;
}


void BindMid(const char* id, uint32_t rva, safetyhook::MidHookFn fn)
{
    auto* h = Mid(rva, fn);
    if (!h) return Fail(id, "hook could not be placed");
    Bound b;
    b.id = id;
    b.toggle = HookToggle(h);
    Ok(std::move(b));
}

// NOPs whole instructions starting at the site.
void BindNop(const char* id, uint32_t rva, int insns)
{
    uint32_t len = 0;
    for (int i = 0; i < insns; ++i) {
        auto in = Dec(rva + len);
        if (!in) return Fail(id, "site does not decode");
        len += in->len;
    }
    uintptr_t at = Abs(rva);
    std::vector<uint8_t> orig(reinterpret_cast<uint8_t*>(at), reinterpret_cast<uint8_t*>(at) + len);
    Bound b;
    b.id = id;
    b.toggle = [at, orig](bool on) {
        std::vector<uint8_t> bytes = on ? std::vector<uint8_t>(orig.size(), 0x90) : orig;
        return WriteCode(at, bytes.data(), bytes.size());
    };
    Ok(std::move(b));
}

// ---- globals edits (render thread only) ----

bool ReadField(const Field& f, double& v)
{
    switch (f.type) {
    case 1: { int32_t x; if (!Rd(f.at, x)) return false; v = x; return true; }
    case 2: { uint64_t x; if (!Rd(f.at, x)) return false; v = (double)x; return true; }
    case 3: { uint8_t x; if (!Rd(f.at, x)) return false; v = x; return true; }
    default: { float x; if (!Rd(f.at, x)) return false; v = x; return true; }
    }
}

bool WriteField(const Field& f, double v)
{
    switch (f.type) {
    case 1: return Wr<int32_t>(f.at, (int32_t)std::llround(v));
    case 2: return Wr<uint64_t>(f.at, (uint64_t)std::max(0.0, v));
    case 3: return Wr<uint8_t>(f.at, v != 0 ? 1 : 0);
    default: return Wr<float>(f.at, (float)v);
    }
}

// Several features may edit the same field. Each address keeps one original and is
// written as the original with every active feature's change applied in turn.
namespace {
struct Shared { double orig = 0, written = 0; uint8_t type = 0; int users = 0; };
std::map<uintptr_t, Shared> g_shared;
}

static double Want(const Field& f, const DataFx& d, double base)
{
    float m = f.slider >= 0 && f.slider < (int)d.mult.size() ? d.mult[f.slider] : 1.f;
    double v = f.map ? f.map(m) : (f.inverse ? base / std::max(m, 0.001f) : base * m) * f.k;
    if (f.type == 0) v = (double)(float)v;
    if (f.type == 1) v = (double)std::llround(v);
    if (f.type == 2) v = (double)(uint64_t)std::max(0.0, v);
    if (f.type == 3) v = v != 0 ? 1 : 0;
    return v;
}

static void Recompute(uintptr_t at)
{
    auto it = g_shared.find(at);
    if (it == g_shared.end()) return;
    double v = it->second.orig;
    for (auto& d : g_data) {
        if (!d.on) continue;
        for (auto& f : d.fields)
            if (f.at == at) v = Want(f, d, v);
    }
    Field w;
    w.at = at;
    w.type = it->second.type;
    WriteField(w, v);
    it->second.written = v;
}

static void DataWrite(DataFx& d)
{
    for (auto& f : d.fields) Recompute(f.at);
}

void BindData(const char* id, std::vector<Field> fields, size_t sliders)
{
    if (fields.empty()) return Fail(id, "no globals field found");
    g_data.push_back({ std::move(fields), std::vector<float>(sliders, 1.f) });
    DataFx* d = &g_data.back();
    Bound b;
    b.id = id;
    b.toggle = [d](bool on) {
        if (on == d->on) return true;
        if (on) {
            for (auto& f : d->fields) {
                Shared& s = g_shared[f.at];
                if (s.users == 0) {
                    if (!ReadField(f, s.orig)) { g_shared.erase(f.at); return false; }
                    s.written = s.orig;
                    s.type = f.type;
                }
                ++s.users;
            }
            d->on = true;
            DataWrite(*d);
        }
        else {
            d->on = false;
            for (auto& f : d->fields) {
                auto it = g_shared.find(f.at);
                if (it == g_shared.end()) continue;
                if (--it->second.users > 0) { Recompute(f.at); continue; }
                WriteField(f, it->second.orig);
                g_shared.erase(it);
            }
        }
        return true;
    };
    b.slider = [d](int i, float v) {
        if (i < 0 || i >= (int)d->mult.size()) return;
        d->mult[i] = v;
        if (d->on) DataWrite(*d);
    };
    Ok(std::move(b));
}

bool MakeRet()
{
    void* p = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) return false;
    *static_cast<uint8_t*>(p) = 0xC3;
    DWORD old;
    VirtualProtect(p, 4096, PAGE_EXECUTE_READ, &old);
    g_ret = reinterpret_cast<uintptr_t>(p);
    return true;
}

void DataTick()
{
    for (auto& [at, sh] : g_shared) {
        Field f;
        f.at = at;
        f.type = sh.type;
        double cur;
        if (!ReadField(f, cur) || cur == sh.written) continue;
        sh.orig = cur; // the game reloaded its globals
        Recompute(at);
    }
}

void Publish()
{
    Registry::Get().Post([out = std::move(g_out)]() mutable {
        auto& r = Registry::Get();
        for (auto& b : out) {
            Feature* f = r.Find(b.id);
            if (!f || f->avail == Avail::Planned) continue;
            if (!b.ok) {
                f->avail = Avail::Unavailable;
                f->reason = b.reason;
                f->on = false;
                continue;
            }
            f->onToggle = std::move(b.toggle);
            f->onSlider = std::move(b.slider);
            f->getNumber = std::move(b.get);
            f->setNumber = std::move(b.set);
            f->onAction = std::move(b.action);
            f->avail = Avail::Ready;
            f->reason.clear();
            if (f->onSlider) for (size_t i = 0; i < f->values.size(); ++i) f->onSlider((int)i, f->values[i]);
            if (f->on && f->onToggle) {
                bool ok = false;
                try { ok = f->onToggle(true); } catch (...) {}
                if (!ok) { f->on = false; log::Warn("Feature", "{} could not be restored", f->id); }
            }
        }
        for (auto* f : r.All())
            if (f->avail == Avail::Pending) { f->avail = Avail::Unavailable; f->reason = "Not wired yet."; f->on = false; }
        log::Info("Feature", "{} of {} features ready", r.ReadyCount(), r.TotalCount());
    });
    g_out.clear();
}

}
