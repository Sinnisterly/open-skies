#include "features/GameFeatures.h"
#include "features/Bind.h"
#include "features/ItemsPage.h"
#include "features/Weapons.h"
#include "features/Unlocks.h"
#include "features/Garage.h"
#include "features/Expeditions.h"
#include "features/Flight.h"
#include "features/Building.h"
#include "features/WorldFx.h"
#include "features/WorldActions.h"
#include "game/Catalog.h"
#include "render/ModelPreview.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

// Feature bindings for the core hook targets.
namespace os::features {

namespace {

safetyhook::InlineHook g_engine;

// ---- mid hook state and callbacks (game threads) ----

void SkipRet0(Context& c) { c.rax = 0; c.rip = RetGadget(); }
void SkipRet1(Context& c) { c.rax = 1; c.rip = RetGadget(); }
struct { uint32_t ret = 0; std::vector<std::pair<uintptr_t, uintptr_t>> ranges; } g_overload;
void OverloadCb(Context& c)
{
    uintptr_t ret = 0;
    if (!Rd(c.rsp + g_overload.ret, ret)) return;
    for (auto& [lo, hi] : g_overload.ranges)
        if (ret >= lo && ret < hi) { c.rax &= ~0xFFFFFFFFull; return; }
}

void Ret0(Context& c) { c.rax &= ~0xFFFFFFFFull; }
void RefineTiny(Context& c) { c.xmm0.f32[0] = 1e-8f; }
void ScanFull(Context& c) { c.xmm1.f32[0] = 1.f; }
void NoFuelUse(Context& c) { c.rdx = 0; }
void PlentyCb(Context& c) { c.rax = 999999; }

struct { int dst, base; } g_interact;
void InteractCb(Context& c)
{
    if (auto* b = GprOf(c, g_interact.base)) Wr<float>(*b, 1.f);
    if (auto* x = XmmOf(c, g_interact.dst)) x->f32[0] = 0.f;
}

int g_jetReg;
void JetpackCb(Context& c) { if (auto* x = XmmOf(c, g_jetReg)) x->f32[0] = 0.f; }

// A meter (jetpack fuel, stamina) can drain on several paths in one function: boosts, launches,
// airborne sprinting. Every store of the field that follows a subtraction keeps the value it had.
// One callback per store, since the context does not say which.
struct HeldStore { int base, src; int32_t disp; };
constexpr int kMaxHeld = 16;
HeldStore g_held[kMaxHeld];
int g_heldCount = 0;
template <int N> void KeepValue(Context& c)
{
    auto& f = g_held[N];
    auto* b = GprOf(c, f.base);
    auto* x = XmmOf(c, f.src);
    float cur = 0;
    if (b && x && Rd(*b + f.disp, cur) && x->f32[0] < cur) x->f32[0] = cur;
}
template <int... N> constexpr safetyhook::MidHookFn kKeepFns[] = { &KeepValue<N>... };
const auto& kKeep = kKeepFns<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15>;

// Hooks every draining store of [base+disp] in the function holding `site`, except those in [skipFrom, skipTo).
void HoldDrains(uint32_t site, int64_t disp, std::vector<safetyhook::MidHook*>& hooks, uint32_t skipFrom = 0, uint32_t skipTo = 0)
{
    std::vector<Insn> code;
    if (auto f = Img().FuncOf(site))
        for (auto& r : Img().FuncRanges(*f)) for (auto& x : Img().DecodeRange(r.first, r.second)) code.push_back(x);
    for (size_t k = 4; k < code.size() && disp > 0 && g_heldCount < kMaxHeld; ++k) {
        auto& st = code[k];
        if (st.mnemonic != "movss" || st.ops.size() != 2 || st.ops[0].kind != hooks::Operand::Mem || st.ops[0].disp != disp || !IsXmm(st.ops[1].reg))
            continue;
        if (st.rva >= skipFrom && st.rva < skipTo) continue;
        bool drains = false;
        for (size_t j = k - 4; j < k; ++j) drains = drains || code[j].mnemonic == "subss";
        if (!drains) continue;
        g_held[g_heldCount] = { st.ops[0].base, st.ops[1].reg, (int32_t)st.ops[0].disp };
        if (auto* h = Mid(st.rva, kKeep[g_heldCount])) { hooks.push_back(h); ++g_heldCount; }
    }
}

void BindHooks(const char* id, std::vector<safetyhook::MidHook*> hooks)
{
    if (hooks.empty()) return Fail(id, "hook could not be placed");
    Bound b;
    b.id = id;
    b.toggle = [hooks](bool on) {
        bool ok = true;
        for (auto* h : hooks) ok = (on ? h->enable() : h->disable()).has_value() && ok;
        return ok;
    };
    Ok(std::move(b));
}

std::atomic<float> g_walkMult{ 2.f };
void WalkCb(Context& c) { c.xmm6.f32[0] *= g_walkMult.load(); }

std::atomic<int> g_spawnClass{ 3 };
void SpawnClassCb(Context& c)
{
    int cls = std::clamp(g_spawnClass.load(), 0, 3);
    if (c.rcx) Wr<int32_t>(c.rcx + 0x100, cls);
    c.rax = (uint64_t)cls;
    c.rip = RetGadget();
}

struct
{
    int base;
    int32_t hp, flag;
    uintptr_t skip;
    std::atomic<bool> instant, waste, oneShot;
    safetyhook::MidHook* hook;
} g_mine;

void MineCb(Context& c)
{
    auto* b = GprOf(c, g_mine.base);
    if (!b) return;
    uintptr_t o = *b;
    if (g_mine.waste) {
        uintptr_t src = 0;
        uint32_t kind = 0;
        if (Rd(o + 0xA8, src) && src && Rd(src + 0x20, kind) && kind == 1) {
            Wr<uint32_t>(o + g_mine.hp, 0);
            Wr<uint8_t>(o + g_mine.flag, 1);
            c.r15 = 0;
            c.rip = g_mine.skip;
            return;
        }
    }
    uint32_t living = 0;
    Rd(o + 0x268, living);
    if (g_mine.oneShot || (g_mine.instant && !living)) {
        uint64_t full;
        if (Rd(o + g_mine.hp - 0xA0, full)) {
            Wr<uint64_t>(o + g_mine.hp, full);
            Wr<uint8_t>(o + g_mine.flag, 1);
            c.rip = g_mine.skip;
        }
    }
}

int g_javReg;
void JavelinCb(Context& c) { if (auto* x = XmmOf(c, g_javReg)) x->f32[0] = 3.f; }

struct { int dst, src; } g_bolt;
void BoltCb(Context& c)
{
    auto* d = GprOf(c, g_bolt.dst);
    auto* s = GprOf(c, g_bolt.src);
    if (d && s) *d = (uint32_t)(*d + *s); // undo the shot
}

struct { int reg; uintptr_t max; } g_runic;
void RunicCb(Context& c)
{
    uint32_t m;
    if (auto* r = GprOf(c, g_runic.reg); r && Rd(g_runic.max, m)) *r = m;
}

struct { int reg, base; int32_t disp; } g_fuel;
void RefinerFuelCb(Context& c)
{
    auto* r = GprOf(c, g_fuel.reg);
    auto* b = GprOf(c, g_fuel.base);
    uint32_t max;
    if (r && b && Rd(*b + g_fuel.disp + 8, max)) *r = (*r & ~0xFFFFFFFFull) | max;
}

struct { int base, src; uintptr_t skip; } g_topo;
void TopoCb(Context& c)
{
    auto* b = GprOf(c, g_topo.base);
    auto* s = GprOf(c, g_topo.src);
    if (!b || !s) return;
    Wr<uint32_t>(*b + 0x3C, (uint32_t)*s);
    c.rip = g_topo.skip;
}

struct { int base; std::atomic<uintptr_t> entry; } g_stats;
void StatsCb(Context& c)
{
    auto* b = GprOf(c, g_stats.base);
    uint32_t k;
    if (b && Rd(*b + 0xC0, k) && k) g_stats.entry = *b;
}

// cGcStatsManager: groups at +0xA8 (count +0xA4), stride 0x60.
// Each group: u64 address at +0, a stat map with keys at +0x28 and info bytes at +0x30, 0x18 per entry, value at +0x10.
int64_t g_statsMgr = 0x307938; // Data.StatsManager
using UpdateStatFn = void (*)(void* mgr, const char* id16, int32_t value, bool remote, uint64_t address);
UpdateStatFn g_updateStat = nullptr;

uintptr_t FindStat(const char* id, uint64_t address)
{
    uintptr_t d = game::Data();
    if (!d) return 0;
    uintptr_t mgr = d + g_statsMgr, groups = 0;
    uint32_t n = 0;
    if (!Rd(mgr + 0xA4, n) || !Rd(mgr + 0xA8, groups) || !groups || n > 4096) return 0;
    for (uint32_t gi = 0; gi < n; ++gi) {
        uintptr_t g = groups + gi * 0x60, keys = 0, info = 0;
        uint64_t addr = 1;
        if (!Rd(g, addr) || addr != address || !Rd(g + 0x28, keys) || !Rd(g + 0x30, info) || !keys || info <= keys) continue;
        size_t slots = (info - keys) / 0x18;
        if (slots > 100000) continue;
        for (size_t i = 0; i < slots; ++i) {
            uint8_t used = 0;
            if (!Rd(info + i, used) || !used) continue;
            uintptr_t e = keys + i * 0x18;
            char k[17] = {};
            for (int j = 0; j < 16 && Rd(e + j, k[j]) && k[j]; ++j) {}
            if (!strcmp(k, id)) return e;
        }
    }
    return 0;
}

// Cached per stat id, checked against the key each use since the map can rehash.
uintptr_t StatEntry(const char* id)
{
    static std::mutex mx;
    static std::unordered_map<std::string, uintptr_t> cache;
    std::lock_guard lk(mx);
    uintptr_t& e = cache[id];
    char k[17] = {};
    if (e) for (int j = 0; j < 16 && Rd(e + j, k[j]) && k[j]; ++j) {}
    if (!e || strcmp(k, id)) e = FindStat(id, 0);
    return e;
}

// Standing.Change(unused, eMissionFaction, delta, noGainScale, systemAddress); ~0 is global only.
using ChangeStandingFn = void (*)(void*, int32_t faction, int32_t delta, bool noGainScale, uint64_t address);
ChangeStandingFn g_changeStanding = nullptr;
int64_t g_curSystem = 0x72B610; // Data.SolarSystem
constexpr uint32_t kSysRace = 0x2534, kSysAddress = 0x26B0, kFactionIds = 0x73F8;

std::string Id16At(uintptr_t a)
{
    char k[17] = {};
    for (int j = 0; j < 16 && Rd(a + j, k[j]) && k[j]; ++j) {}
    return k;
}

// Stat id of a faction from the game's own FactionStandingIDs table, or the fallback.
std::string StandingId(int faction, const char* fallback)
{
    uintptr_t rm = game::Reality(), p = 0;
    for (uintptr_t base : { rm ? rm + kFactionIds : 0, rm && Rd(rm, p) && p ? p + kFactionIds : 0 }) {
        if (!base) continue;
        std::string s = Id16At(base + faction * 0x10);
        if (s.find("STAND") != std::string::npos) return s;
    }
    return fallback;
}

// The stat entry of a faction in one system's SYSTEM_STATS group.
uintptr_t SystemStat(const std::string& id, uint64_t address) { return FindStat(id.c_str(), address); }

// Race of the current system and its stats key, or false out of a system.
bool HereSystem(int& faction, uint64_t& address)
{
    uintptr_t d = game::Data(), sys = 0;
    int32_t race = -1;
    if (!d || !Rd(d + g_curSystem, sys) || !sys || !Rd(sys + kSysRace, race) || !Rd(sys + kSysAddress, address) || !address) return false;
    // eAlienRace to eMissionFaction: Traders Gek, Warriors Vy'keen, Explorers Korvax, Autophage keeps 8
    static const int map[] = { 0, 2, 1 };
    if (race >= 0 && race < 3) faction = map[race];
    else if (race == 8) faction = 8;
    else return false;
    return true;
}

const char* kFactionFallback[] = { "TRA_STANDING", "EXP_STANDING", "WAR_STANDING", "TGUILD_STAND", "WGUILD_STAND", "EGUILD_STAND", "", "PIR_STAND", "BUI_STANDING" };

void BindLocalStanding()
{
    Bound b;
    b.id = "eco.local";
    b.get = [](int64_t& v) {
        int f = -1;
        uint64_t addr = 0;
        if (!HereSystem(f, addr)) return false;
        int32_t n = 0;
        uintptr_t e = SystemStat(StandingId(f, kFactionFallback[f]), addr);
        v = e && Rd(e + 0x10, n) ? std::max(n, 0) : 0; // no entry yet reads as 0, like the game
        return true;
    };
    b.set = [](int64_t v) {
        if (!g_changeStanding) return false;
        game::Post([v] {
            int f = -1;
            uint64_t addr = 0;
            if (!HereSystem(f, addr)) return;
            std::string id = StandingId(f, kFactionFallback[f]);
            int32_t cur = 0, glob = 0;
            if (uintptr_t e = SystemStat(id, addr)) Rd(e + 0x10, cur);
            uintptr_t ge = FindStat(id.c_str(), 0);
            bool hadGlobal = ge && Rd(ge + 0x10, glob);
            if ((int32_t)v == cur) return;
            g_changeStanding(nullptr, f, (int32_t)v - cur, true, addr);
            // the change moves global by the same amount, so put global back
            if (uintptr_t e = FindStat(id.c_str(), 0)) Wr<int32_t>(e + 0x10, hadGlobal ? glob : 0);
        });
        return true;
    };
    Ok(std::move(b));
}

struct { int base; int32_t amount; std::atomic<uintptr_t> slot; } g_moved;
void MovedCb(Context& c) { if (auto* b = GprOf(c, g_moved.base)) g_moved.slot = *b; }

std::vector<uint32_t> g_engFields;
std::atomic<float> g_engMult{ 4.f };
void EngineHk(void* a, void* b, uint8_t* out, float t)
{
    g_engine.call<void>(a, b, out, t);
    float m = g_engMult.load();
    for (uint32_t off : g_engFields) {
        float v;
        if (Rd(uintptr_t(out + off), v)) Wr(uintptr_t(out + off), v * m);
    }
}


// ---- per feature ----

void Exosuit()
{
    if (auto* t = Need("Exosuit.NoDamage", { "exo.no_damage" })) BindMid("exo.no_damage", t->func, SkipRet0);
    if (auto* t = Need("Exosuit.EnvSuit", { "exo.hazard" })) BindMid("exo.hazard", t->func, SkipRet0);
    if (auto* t = Need("Exosuit.Stamina", { "exo.stamina" })) {
        std::vector<safetyhook::MidHook*> hooks;
        HoldDrains(t->site, t->offset ? *t->offset : -1, hooks); // sprinting and the other stamina costs
        BindHooks("exo.stamina", std::move(hooks));
    }
    if (auto* t = Need("Exosuit.MarineJet", { "exo.marine_jetpack" })) BindNop("exo.marine_jetpack", t->site, 1);

    if (auto* t = Need("Exosuit.Jetpack", { "exo.jetpack" })) {
        auto in = Dec(t->site);
        if (!in || !IsXmm(in->ops[0].reg)) Fail("exo.jetpack", "unexpected register");
        else {
            g_jetReg = in->ops[0].reg;
            std::vector<safetyhook::MidHook*> hooks;
            if (auto* h = Mid(in->rva + in->len, JetpackCb)) hooks.push_back(h);
            size_t before = hooks.size();
            HoldDrains(t->site, t->offset ? *t->offset : -1, hooks, t->site, t->site + 24); // boosts and launches; the main drain is above
            log::Info("Feature", "Jetpack: {} extra fuel drains held", hooks.size() - before);
            BindHooks("exo.jetpack", std::move(hooks));
        }
    }

    if (auto* t = Need("Movement.WalkRun", { "move.run_speed" })) {
        auto* h = Mid(t->site, WalkCb);
        if (!h) Fail("move.run_speed", "hook could not be placed");
        else {
            Bound b;
            b.id = "move.run_speed";
            b.toggle = HookToggle(h);
            b.slider = [](int i, float v) { if (i == 0) g_walkMult = v; };
            Ok(std::move(b));
        }
    }

    // Jetpack power edits three cGcPlayerGlobals fields: lift, air, speed.
    std::vector<Field> jet;
    std::string why = "no globals field found";
    int slider = 0;
    for (const char* name : { "Movement.JetpackUp", "Movement.JetpackAerial", "Movement.JetpackSpeed" }) {
        const Target& t = Loc().Resolve(name);
        if (t.ok && t.global) jet.push_back({ t.global, slider });
        else if (jet.empty() && !t.reason.empty()) why = t.reason;
        ++slider;
    }
    if (jet.empty()) Fail("move.jetpack_power", why);
    else BindData("move.jetpack_power", std::move(jet), 3);
}

void MultiTool()
{
    if (auto* t = Need("MultiTool.MiningDamage", { "mt.instant_mining", "mt.waste_guard", "mt.one_shot" })) {
        auto a = Dec(t->site);
        auto b = a ? Next(*a) : std::nullopt;
        if (!a || !b || b->ops.empty() || b->ops[0].kind != hooks::Operand::Mem || !IsGpr(a->ops[0].base)) {
            for (auto* id : { "mt.instant_mining", "mt.waste_guard", "mt.one_shot" }) Fail(id, "unexpected code at the site");
        }
        else {
            g_mine.base = a->ops[0].base;
            g_mine.hp = (int32_t)a->ops[0].disp;
            g_mine.flag = (int32_t)b->ops[0].disp;
            g_mine.skip = Abs(b->rva + b->len);
            g_mine.hook = Mid(t->site, MineCb);
            if (!g_mine.hook) for (auto* id : { "mt.instant_mining", "mt.waste_guard", "mt.one_shot" }) Fail(id, "hook could not be placed");
            else {
                auto flag = [](std::atomic<bool>* v) {
                    return [v](bool on) {
                        *v = on;
                        bool any = g_mine.instant || g_mine.waste || g_mine.oneShot;
                        if (any == g_mine.hook->enabled()) return true;
                        return (any ? g_mine.hook->enable() : g_mine.hook->disable()).has_value();
                    };
                };
                Bound x; x.id = "mt.instant_mining"; x.toggle = flag(&g_mine.instant); Ok(std::move(x));
                Bound y; y.id = "mt.waste_guard"; y.toggle = flag(&g_mine.waste); Ok(std::move(y));
                Bound z; z.id = "mt.one_shot"; z.toggle = flag(&g_mine.oneShot); Ok(std::move(z));
            }
        }
    }


    if (auto* t = Need("MultiTool.JavelinCharge", { "mt.javelin" })) {
        auto a = Dec(t->site);
        auto b = a ? Next(*a) : std::nullopt;
        auto add = b ? Next(*b) : std::nullopt;
        if (!add || add->mnemonic != "addss" || !IsXmm(add->ops[0].reg)) Fail("mt.javelin", "unexpected code at the site");
        else { g_javReg = add->ops[0].reg; BindMid("mt.javelin", add->rva, JavelinCb); }
    }

    if (auto* t = Need("MultiTool.BoltcasterAmmo", { "mt.boltcaster" })) {
        auto a = Dec(t->site);
        if (!a || a->ops.size() != 2 || !IsGpr(a->ops[0].reg) || !IsGpr(a->ops[1].reg)) Fail("mt.boltcaster", "unexpected code at the site");
        else { g_bolt = { a->ops[0].reg, a->ops[1].reg }; BindMid("mt.boltcaster", a->rva + a->len, BoltCb); }
    }

    if (auto* t = Need("MultiTool.ScanInstant", { "mt.scan_instant" })) BindMid("mt.scan_instant", t->func, ScanFull);
    if (auto* t = Need("MultiTool.ScanTrue", { "mt.scan_look" })) BindNop("mt.scan_look", t->site, 1);

    if (auto* t = Need("MultiTool.TopoScanner", { "mt.topo_scanner" })) {
        auto a = Dec(t->site);
        auto b = a ? Next(*a) : std::nullopt;
        if (!b || b->mnemonic != "mov" || !IsGpr(a->ops[0].base) || !IsGpr(a->ops[1].reg)) Fail("mt.topo_scanner", "unexpected code at the site");
        else { g_topo = { a->ops[0].base, a->ops[1].reg, Abs(b->rva + b->len) }; BindMid("mt.topo_scanner", t->site, TopoCb); }
    }

    if (auto* t = Need("MultiTool.RunicLens", { "mt.runic_lens" })) {
        // test, cmovg, cmp, cmovge [max], then the store
        std::optional<Insn> in = Dec(t->site);
        uintptr_t max = 0;
        for (int i = 0; in && i < 4; ++i) {
            if (in->ripTarget && in->mnemonic.starts_with("cmov")) max = Abs(*in->ripTarget);
            in = Next(*in);
        }
        if (!in || !max || in->mnemonic != "mov" || in->ops.size() != 2 || !IsGpr(in->ops[1].reg)) Fail("mt.runic_lens", "unexpected code at the site");
        else { g_runic = { in->ops[1].reg, max }; BindMid("mt.runic_lens", in->rva, RunicCb); }
    }
}

void Starship()
{
    if (auto* t = Need("Starship.RocketCooldown", { "ship.rockets" })) BindNop("ship.rockets", t->site, 1);
    if (auto* t = Need("Starship.PhotonHeat", { "ship.photon_heat" })) BindNop("ship.photon_heat", t->site, 1);

    if (auto* t = Need("Movement.ShipEngine", { "ship.engine_speed" })) {
        for (auto& [name, off] : t->fields) g_engFields.push_back(off);
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t->func)), reinterpret_cast<void*>(&EngineHk),
                                                safetyhook::InlineHook::StartDisabled);
        if (!h || g_engFields.empty()) Fail("ship.engine_speed", "hook could not be placed");
        else {
            g_engine = std::move(*h);
            Bound b;
            b.id = "ship.engine_speed";
            b.toggle = [](bool on) { return (on ? g_engine.enable() : g_engine.disable()).has_value(); };
            b.slider = [](int i, float v) { if (i == 0) g_engMult = v; };
            Ok(std::move(b));
        }
    }

    if (auto* t = Need("Movement.PulseSpeed", { "ship.pulse_speed" })) {
        if (!t->global) Fail("ship.pulse_speed", "no globals field found");
        else BindData("ship.pulse_speed", { { t->global, 0 } }, 1);
    }
}

void Exocraft()
{
    if (auto* t = Need("Exocraft.Fuel", { "vehicle.fuel" })) BindMid("vehicle.fuel", t->func, NoFuelUse);
    if (auto* t = Need("Exocraft.Boost", { "vehicle.boost" })) BindNop("vehicle.boost", t->site, 1);
    if (auto* t = Need("Exocraft.NautilonBoost", { "vehicle.nautilon" })) BindNop("vehicle.nautilon", t->site, 1);
}

void Crafting()
{
    if (auto* t = Need("Crafting.NoConsume", { "craft.free" })) BindNop("craft.free", t->site, 1);
    // Recipes use the affordability check; repairs and installs read the item count directly.
    if (auto* t = Need("Crafting.AlwaysHaveResources", { "craft.always" })) {
        const Target& cnt = Loc().Resolve("Crafting.ItemCount");
        auto tail = cnt.ok ? ScanFunc(cnt.func, "E8 ?? ?? ?? ?? 48 83 C4 ?? 5B C3") : std::nullopt;
        auto* a = Mid(t->func, SkipRet1);
        auto* b = tail ? Mid(*tail + 5, PlentyCb) : nullptr;
        if (!a) Fail("craft.always", "hook could not be placed");
        else {
            if (!b) log::Warn("Feature", "craft.always: item count hook missing, repairs keep their cost");
            Bound x;
            x.id = "craft.always";
            x.toggle = [a, b](bool on) {
                bool ok = (on ? a->enable() : a->disable()).has_value();
                if (b) ok = (on ? b->enable() : b->disable()).has_value() && ok;
                return ok;
            };
            Ok(std::move(x));
        }
    }
    if (auto* t = Need("Crafting.RefinerNoConsume", { "refiner.free" })) BindNop("refiner.free", t->site, 1);

    if (auto* t = Need("Crafting.RefinerFuel", { "refiner.fuel" })) {
        auto a = Dec(t->site);
        auto st = a ? Next(*a) : std::nullopt;
        if (!st || st->mnemonic != "mov" || st->ops.size() != 2 || st->ops[0].kind != hooks::Operand::Mem || !IsGpr(st->ops[1].reg))
            Fail("refiner.fuel", "unexpected code at the site");
        else { g_fuel = { st->ops[1].reg, st->ops[0].base, (int32_t)st->ops[0].disp }; BindMid("refiner.fuel", t->site, RefinerFuelCb); }
    }

    // Postfix on CalculateRefineTime: hook the epilogue right after the divide.
    if (auto* t = Need("Crafting.RefinerInstant", { "refiner.instant" })) {
        auto at = ScanFunc(t->func, "0F 5B C9 F3 0F 5E C1 48 83 C4");
        if (!at) Fail("refiner.instant", "epilogue not found");
        else BindMid("refiner.instant", *at + 7, RefineTiny);
    }
}

void Inventory()
{
    // Postfix on the tech group count: zero the result in the epilogue, but only for the stat sum.
    // Adjacency bonuses, proc tech tiers and "has upgrade" mission checks ask the same count.
    if (auto* t = Need("Inventory.TechOverloads", { "inv.no_overload" })) {
        auto at = ScanFunc(t->func, "8B 40 ?? 48 83 C4 20");
        const Target& st = Loc().Resolve("Inventory.AdjustStat");
        uint32_t frame = 0;
        for (auto in = at ? Dec(t->func) : std::nullopt; in && in->rva < *at; in = Next(*in)) { // push reg / sub rsp, imm
            if (in->mnemonic == "push") frame += 8;
            if (in->mnemonic == "sub" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm) { frame += (uint32_t)in->ops[1].imm; break; }
        }
        if (!at) Fail("inv.no_overload", "epilogue not found");
        else if (!st.ok) Fail("inv.no_overload", st.reason);
        else {
            g_overload.ret = frame; // return address above the frame at the epilogue
            for (auto& r : Img().FuncRanges(st.func)) g_overload.ranges.push_back({ Abs(r.first), Abs(r.second) });
            BindMid("inv.no_overload", *at + 3, OverloadCb);
        }
    }

    if (auto* t = Need("Inventory.LastMovedItem", { "inv.last_moved" })) {
        auto a = Dec(t->site);
        safetyhook::MidHook* h = nullptr;
        if (a && !a->ops.empty() && IsGpr(a->ops[0].base)) {
            g_moved.base = a->ops[0].base;
            g_moved.amount = (int32_t)a->ops[0].disp + 8;
            h = Mid(t->site, MovedCb);
        }
        if (!h || !h->enable()) Fail("inv.last_moved", "hook could not be placed");
        else {
            Bound b;
            b.id = "inv.last_moved";
            b.get = [](int64_t& v) {
                uintptr_t s = g_moved.slot;
                int32_t n;
                if (!s || !Rd(s + g_moved.amount, n)) return false;
                v = std::max(n, 0);
                return true;
            };
            b.set = [](int64_t v) { uintptr_t s = g_moved.slot; return s && Wr<int32_t>(s + g_moved.amount, (int32_t)v); };
            Ok(std::move(b));
        }
    }
}

void Economy()
{
    if (auto* t = Need("Economy.Units", { "eco.units", "eco.nanites", "eco.quicksilver" })) {
        if (!t->global || !t->offset) {
            for (auto* id : { "eco.units", "eco.nanites", "eco.quicksilver" }) Fail(id, "wallet pointer not found");
        }
        else {
            uintptr_t slot = t->global;
            int64_t off = *t->offset;
            int k = 0;
            for (auto* id : { "eco.units", "eco.nanites", "eco.quicksilver" }) {
                int64_t at = off + 4 * k++;
                Bound b;
                b.id = id;
                b.get = [slot, at](int64_t& v) {
                    uintptr_t p;
                    uint32_t n;
                    if (!Rd(slot, p) || !p || !Rd(p + at, n)) return false;
                    v = n;
                    return true;
                };
                b.set = [slot, at](int64_t v) {
                    uintptr_t p;
                    return Rd(slot, p) && p && Wr<uint32_t>(p + at, (uint32_t)v);
                };
                Ok(std::move(b));
            }
        }
    }

    // Standings: the GLOBAL_STATS entries by stat id, set through the game's UpdateStat so ranks refresh.
    // The deltas from the captured stat entry are the fallback. faction is eMissionFaction.
    struct Stand { const char* id; const char* stat; int32_t delta; int faction; };
    static const Stand kStand[] = {
        { "eco.gek", "TRA_STANDING", 0x658, 0 }, { "eco.vykeen", "WAR_STANDING", 0x10 - 0x2E08, 2 }, { "eco.korvax", "EXP_STANDING", 0x970, 1 },
        { "eco.merchants", "TGUILD_STAND", 0x10 - 0x1920, 3 }, { "eco.mercenaries", "WGUILD_STAND", 0xF40, 4 },
        { "eco.explorers", "EGUILD_STAND", 0x10 - 0x2B50, 5 }, { "eco.outlaws", "PIR_STAND", 0x448, 7 },
    };
    if (const Target& m = Loc().Resolve("Data.StatsManager"); m.ok && m.offset) g_statsMgr = *m.offset;
    if (const Target& s = Loc().Resolve("Data.SolarSystem"); s.ok && s.offset) g_curSystem = *s.offset;
    if (const Target& u = Loc().Resolve("Stats.UpdateStatInt"); u.ok) g_updateStat = (UpdateStatFn)Abs(u.func);
    else log::Warn("Stats", "UpdateStat missing: {}", u.reason);
    if (const Target& c = Loc().Resolve("Standing.Change"); c.ok) g_changeStanding = (ChangeStandingFn)Abs(c.func);
    else log::Warn("Stats", "Standing.Change missing: {}", c.reason);
    BindLocalStanding();
    const Target& t = Loc().Resolve("Economy.Standings");
    auto a = t.ok ? Dec(t.site) : std::nullopt;
    if (a && a->ops.size() == 2 && IsGpr(a->ops[1].base)) {
        g_stats.base = a->ops[1].base;
        if (auto* h = Mid(t.site, StatsCb)) (void)h->enable();
    }
    for (auto& s : kStand) {
        Stand st = s;
        Bound b;
        b.id = st.id;
        b.get = [st](int64_t& v) {
            int32_t n;
            if (uintptr_t e = StatEntry(StandingId(st.faction, st.stat).c_str()); e && Rd(e + 0x10, n)) { v = std::max(n, 0); return true; }
            uintptr_t c = g_stats.entry;
            if (!c || !Rd(c + st.delta, n)) return false;
            v = std::max(n, 0);
            return true;
        };
        b.set = [st](int64_t v) {
            game::Post([st, v] {
                int32_t want = (int32_t)v, now = 0;
                char id16[16] = {};
                strncpy_s(id16, StandingId(st.faction, st.stat).c_str(), _TRUNCATE);
                uintptr_t e = StatEntry(StandingId(st.faction, st.stat).c_str());
                if (g_changeStanding) {
                    // the game's own standing change: exact delta, rank messages, works before the first gain
                    int32_t cur = 0;
                    if (e) Rd(e + 0x10, cur);
                    if (want != cur) g_changeStanding(nullptr, st.faction, want - cur, true, ~0ull);
                    e = StatEntry(StandingId(st.faction, st.stat).c_str());
                }
                else if (g_updateStat) {
                    g_updateStat((void*)(game::Data() + g_statsMgr), id16, want, false, 0);
                    e = StatEntry(StandingId(st.faction, st.stat).c_str());
                    // an Add stat lands on old + want, so step back by the difference
                    if (e && Rd(e + 0x10, now) && now != want) g_updateStat((void*)(game::Data() + g_statsMgr), id16, want - now, false, 0);
                    e = StatEntry(StandingId(st.faction, st.stat).c_str());
                }
                if (e && Rd(e + 0x10, now) && now == want) return;
                if (!e) e = g_stats.entry ? g_stats.entry + st.delta - 0x10 : 0;
                if (e) Wr<int32_t>(e + 0x10, want);
                log::Info("Stats", "{} set by direct write", st.stat);
            });
            return true;
        };
        Ok(std::move(b));
    }
}

void World()
{
    if (auto* t = Need("General.InstantInteract", { "world.interact" })) {
        auto a = Dec(t->site);
        if (!a || a->ops.size() != 2 || !IsXmm(a->ops[0].reg) || !IsGpr(a->ops[1].base)) Fail("world.interact", "unexpected code at the site");
        else { g_interact = { a->ops[0].reg, a->ops[1].base }; BindMid("world.interact", t->site, InteractCb); }
    }
    if (auto* t = Need("Inventory.GenerateClass", { "world.spawn_class" })) {
        auto* h = Mid(t->func, SpawnClassCb);
        if (!h) Fail("world.spawn_class", "hook could not be placed");
        else {
            Bound b;
            b.id = "world.spawn_class";
            b.toggle = HookToggle(h);
            b.slider = [](int i, float v) { if (i == 0) g_spawnClass = std::clamp((int)(v + 0.5f), 0, 3); };
            Ok(std::move(b));
        }
    }
}

}

void Install()
{
    if (!Loc().Init() || !MakeRet()) {
        for (auto* f : Registry::Get().All())
            if (f->avail == Avail::Pending) Fail(f->id.c_str(), "Could not read the game code.");
        Publish();
        return;
    }
    game::InstallFrameHook();
    InstallLogoSkip();
    game::InitGraph();
    for (auto* step : { &Exosuit, &MultiTool, &Starship, &Exocraft, &Crafting, &Inventory, &Economy, &World }) {
        try { step(); }
        catch (const std::exception& e) { log::Error("Feature", "Binding step failed: {}", e.what()); }
        catch (...) { log::Error("Feature", "Binding step failed"); }
    }
    try { InstallWeapons(); }
    catch (...) { log::Error("Feature", "Weapons binding failed"); }
    try { InstallExpeditions(); }
    catch (...) { log::Error("Feature", "Expeditions page failed"); }
    try { InstallGarage(); }
    catch (...) { log::Error("Feature", "Garage page failed"); }
    try { InstallUnlocks(); }
    catch (...) { log::Error("Feature", "Unlocks page failed"); }
    try { InstallItemsPage(); }
    catch (...) { log::Error("Feature", "Items page failed"); }
    try { InstallWorldFx(); }
    catch (...) { log::Error("Feature", "World and tuner binding failed"); }
    try { InstallWorldActions(); }
    catch (...) { log::Error("Feature", "World actions binding failed"); }
    try { InstallTeleport(); }
    catch (...) { log::Error("Feature", "Teleport page failed"); }
    try { InstallScout(); }
    catch (...) { log::Error("Feature", "Scout page failed"); }
    try { InstallStorage(); }
    catch (...) { log::Error("Feature", "Storage page failed"); }
    try { InstallEffects(); }
    catch (...) { log::Error("Feature", "Effects page failed"); }
    try { InstallFlight(); }
    catch (...) { log::Error("Feature", "Flight binding failed"); }
    try { InstallCreatures(); }
    catch (...) { log::Error("Feature", "Creature binding failed"); }
    try { render::preview::Install(); }
    catch (...) { log::Error("Feature", "Item preview failed"); }
    try { game::prices::Install(); }
    catch (...) { log::Error("Feature", "System prices failed"); }
    try { InstallEncounters(); }
    catch (...) { log::Error("Feature", "Encounter binding failed"); }
    try { InstallBuilding(); }
    catch (...) { log::Error("Feature", "Building binding failed"); }
    try { InstallBlueprints(); }
    catch (...) { log::Error("Feature", "Blueprints page failed"); }
    try { InstallBases(); }
    catch (...) { log::Error("Feature", "Bases page failed"); }
    Loc().Finish();
    Publish();
}

void Tick()
{
    static int frame = 0;
    ++frame;
    if (frame % 15 == 0) WeaponsTick();
    if (frame % 60 == 0) DataTick();
    if (frame % 10 == 0) WorldActionsTick();
}

}
