#include "features/Weapons.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <atomic>
#include <string_view>

// Weapons, damage and charge.
namespace os::features {

namespace {

// ---- callbacks ----

struct Reg { int reg = 0; };
Reg g_zeroA, g_zeroB, g_zeroC, g_zeroD;
template <Reg* R> void ZeroReg(Context& c) { if (auto* r = GprOf(c, R->reg)) *r = 0; }

Reg g_hull, g_health;
template <Reg* R> void NoLoss(Context& c)
{
    if (auto* r = GprOf(c, R->reg); r && (int32_t)(uint32_t)*r < 0) *r &= ~0xFFFFFFFFull;
}

Reg g_shieldShip, g_shieldSuit;
template <Reg* R> void NoChange(Context& c) { if (auto* r = GprOf(c, R->reg)) *r &= ~0xFFFFFFFFull; }

void ReturnOne(Context& c) { c.xmm0.f32[0] = 1.f; c.rip = RetGadget(); }
void ReturnZeroF(Context& c) { c.xmm0.f32[0] = 0.f; c.rip = RetGadget(); }
void SkipVoid(Context& c) { c.rip = RetGadget(); }

struct { int index, base; int32_t disp; } g_heat;
void ShipHeatCb(Context& c)
{
    auto* i = GprOf(c, g_heat.index);
    auto* b = GprOf(c, g_heat.base);
    if (i && b && (int64_t)*i >= 0 && *i < 16) Wr<float>(*b + *i * 4 + g_heat.disp, 0.f);
}

struct { int base, xmm; int32_t disp; } g_drain;
void LaserDrainCb(Context& c)
{
    if (auto* b = GprOf(c, g_drain.base)) Wr<float>(*b + g_drain.disp, 0.f);
    if (auto* x = XmmOf(c, g_drain.xmm)) x->f32[0] = 0.f;
}

Reg g_exoA, g_exoB;
template <Reg* R> void ZeroXmm(Context& c) { if (auto* x = XmmOf(c, R->reg)) x->f32[0] = 0.f; }

// ---- helpers ----

// Hook just after the site's load and zero the loaded register.
void BindZeroAfter(const char* id, std::initializer_list<std::pair<const char*, Reg*>> sites, std::initializer_list<safetyhook::MidHookFn> fns)
{
    std::vector<safetyhook::MidHook*> hooks;
    auto fn = fns.begin();
    for (auto& [name, reg] : sites) {
        const Target& t = Loc().Resolve(name);
        auto in = t.ok ? Dec(t.site) : std::nullopt;
        if (!in || in->ops.empty() || !IsGpr(in->ops[0].reg)) { log::Warn("Weapons", "{}: {} missing", id, name); ++fn; continue; }
        reg->reg = in->ops[0].reg;
        if (auto* h = Mid(in->rva + in->len, *fn)) hooks.push_back(h);
        ++fn;
    }
    if (hooks.empty()) return Fail(id, "no site found");
    Bound b;
    b.id = id;
    b.toggle = [hooks](bool on) {
        bool ok = true;
        for (auto* h : hooks) ok = (on ? h->enable() : h->disable()).has_value() && ok;
        return ok;
    };
    Ok(std::move(b));
}

// One toggle driving several already-created hooks and NOP patches.
struct Group
{
    std::vector<safetyhook::MidHook*> hooks;
    std::vector<std::pair<uintptr_t, std::vector<uint8_t>>> nops;

    void Nop(uint32_t rva, int insns)
    {
        uint32_t len = 0;
        for (int i = 0; i < insns; ++i) if (auto in = Dec(rva + len)) len += in->len; else return;
        uintptr_t at = Abs(rva);
        nops.push_back({ at, std::vector<uint8_t>((uint8_t*)at, (uint8_t*)at + len) });
    }

    void Hook(uint32_t rva, safetyhook::MidHookFn fn) { if (auto* h = Mid(rva, fn)) hooks.push_back(h); }

    void Bind(const char* id)
    {
        if (hooks.empty() && nops.empty()) return Fail(id, "no site found");
        Bound b;
        b.id = id;
        auto hk = hooks;
        auto np = nops;
        b.toggle = [hk, np](bool on) {
            bool ok = true;
            for (auto* h : hk) ok = (on ? h->enable() : h->disable()).has_value() && ok;
            for (auto& [at, orig] : np) {
                std::vector<uint8_t> bytes = on ? std::vector<uint8_t>(orig.size(), 0x90) : orig;
                ok = WriteCode(at, bytes.data(), bytes.size()) && ok;
            }
            return ok;
        };
        Ok(std::move(b));
    }
};

const Target* Opt(const char* name)
{
    const Target& t = Loc().Resolve(name);
    return t.ok ? &t : nullptr;
}

// ---- tech charge fill (game thread) ----

constexpr uint32_t kSuitInv = 0x910, kShipInv = 0x7458, kShipTech = 0xAB28, kVehicleTech = 0x6460, kPrimaryShip = 0x182A0, kStride = 0x248;
std::atomic<bool> g_fill{ false }, g_repair{ false }, g_life{ false }, g_terrain{ false }, g_hyper{ false }, g_stacks{ false };
std::atomic<bool> g_shipFill{ false }, g_shipFix{ false };
uint32_t g_elSize = 0x30, g_elAmount = 0x18, g_elDamage = 0x1C, g_elMax = 0x20, g_elType = 0x24;

// Tech kept charged by the single-item toggles, by id prefix.
bool WantCharge(uintptr_t e, bool ship)
{
    if (g_fill || (ship && g_shipFill)) return true;
    char id[17] = {};
    for (int i = 0; i < 16; ++i) if (!Rd(e + i, id[i])) return false;
    std::string_view v(id);
    if (g_life && (v.starts_with("ENERGY") || v.starts_with("PROTECT"))) return true;
    if (g_terrain && v.starts_with("TERRAIN")) return true;
    if (g_hyper && v.find("HYPERDRIVE") != std::string_view::npos) return true;
    return false;
}

// Tops off substance and product stacks to the cap the game computed.
void TopStacks(uintptr_t s)
{
    int16_t w = 0, h = 0;
    uintptr_t arr = 0;
    int32_t n = 0;
    if (!Rd(s + 0x80, w) || !Rd(s + 0x82, h) || w < 1 || w > 16 || h < 1 || h > 16) return;
    if (!Rd(s + 0x90, arr) || !Rd(s + 0x8C, n) || !arr || n <= 0 || n > w * h) return;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t e = arr + (uintptr_t)i * g_elSize;
        int32_t type = -1, amount = 0, max = 0;
        if (!Rd(e + g_elType, type) || (type != 0 && type != 2)) continue;
        if (Rd(e + g_elAmount, amount) && Rd(e + g_elMax, max) && max > 0 && amount > 0 && amount < max) Wr<int32_t>(e + g_elAmount, max);
    }
}

void FillStore(uintptr_t s, bool ship = false)
{
    int16_t w = 0, h = 0;
    uintptr_t arr = 0;
    int32_t n = 0;
    if (!Rd(s + 0x80, w) || !Rd(s + 0x82, h) || w < 1 || w > 16 || h < 1 || h > 16) return;
    if (!Rd(s + 0x90, arr) || !Rd(s + 0x8C, n) || !arr || n <= 0 || n > w * h) return;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t e = arr + (uintptr_t)i * g_elSize;
        int32_t type = -1, amount = 0, max = 0;
        if (!Rd(e + g_elType, type) || type != 1) continue;
        if (!Rd(e + g_elAmount, amount) || !Rd(e + g_elMax, max) || max <= 0) continue;
        // Out of range charge reads as full but fails launch checks, so put it back in range.
        if (amount < 0 || amount > max || (amount < max && WantCharge(e, ship))) Wr<int32_t>(e + g_elAmount, max);
        float dmg = 0;
        if (g_repair && Rd(e + g_elDamage, dmg) && dmg > 0) Wr<float>(e + g_elDamage, 0.f);
    }
}

void FillAll()
{
    uintptr_t ps = game::PlayerState();
    if (!ps) return;
    uint32_t ship = 0;
    Rd(ps + kPrimaryShip, ship);
    FillStore(ps + kSuitInv + 1 * kStride); // Suit_Tech
    FillStore(ps + kSuitInv + 3 * kStride); // Weapon
    if (ship < 12) FillStore(ps + kShipTech + ship * kStride, true);
    g_shipFix = false;
    for (uint32_t v = 0; v < 7; ++v) FillStore(ps + kVehicleTech + v * kStride);
    if (g_stacks) {
        TopStacks(ps + kSuitInv);
        TopStacks(ps + kSuitInv + 2 * kStride);
        if (ship < 12) TopStacks(ps + kShipInv + ship * kStride);
    }
}

}

void InstallWeapons()
{
    if (auto* ci = hooks::meta::Get("cGcInventoryElement")) {
        g_elSize = ci->size;
        if (auto* m = ci->Find("Amount")) g_elAmount = m->offset;
        if (auto* m = ci->Find("DamageFactor")) g_elDamage = m->offset;
        if (auto* m = ci->Find("MaxAmount")) g_elMax = m->offset;
        if (auto* m = ci->Find("Type")) g_elType = m->offset;
    }

    // Multi-Tool clip: the per-shot decrement covers every clip mode, not just the boltcaster.
    {
        Group g;
        if (auto* t = Opt("Weapons.ChargedShotAmmo")) g.Nop(t->site, 1);
        if (auto* t = Opt("Weapons.ChargeFactor"))
            if (auto f = Img().FuncOf(t->site)) g.Hook(*f, ReturnOne);
        g.Bind("mt.charge");
    }
    BindZeroAfter("mt.free_reload", { { "Weapons.ReloadFreeAmmo", &g_zeroA }, { "Weapons.ReloadFreeCharge", &g_zeroB },
                                      { "Weapons.FireDrainA", &g_zeroC }, { "Weapons.FireDrainB", &g_zeroD } },
                  { ZeroReg<&g_zeroA>, ZeroReg<&g_zeroB>, ZeroReg<&g_zeroC>, ZeroReg<&g_zeroD> });

    // Mining beam: heat, overheat flag, charge drain timer.
    {
        Group g;
        if (auto* t = Opt("MultiTool.BeamHeat")) g.Nop(t->site, 2);
        if (auto* t = Opt("Weapons.LaserOverheatFlag")) g.Nop(t->site, 1);
        if (auto* t = Opt("Weapons.LaserDrain")) {
            auto a = Dec(t->site);
            auto st = a ? Next(*a) : std::nullopt;
            auto call = st ? Next(*st) : std::nullopt;
            if (call && call->isCall && st->ops.size() == 2 && IsGpr(st->ops[0].base) && IsXmm(st->ops[1].reg)) {
                g_drain = { st->ops[0].base, st->ops[1].reg, (int32_t)st->ops[0].disp };
                g.Hook(call->rva, LaserDrainCb);
            }
        }
        g.Bind("mt.beam_heat");
    }

    // Starship: heat for all 7 weapons, plus the HUD bars.
    {
        Group g;
        if (auto* t = Opt("Weapons.ShipHeat")) {
            auto a = Dec(t->site);
            auto mv = a ? Next(*a) : std::nullopt;
            if (mv && mv->ops.size() == 2 && mv->ops[1].kind == hooks::Operand::Mem && IsGpr(a->ops[0].reg) && IsGpr(mv->ops[1].base)) {
                g_heat = { a->ops[0].reg, mv->ops[1].base, (int32_t)mv->ops[1].disp };
                g.Hook(mv->rva, ShipHeatCb);
            }
        }
        for (const char* n : { "Weapons.ShipHeatFactor", "Weapons.ShipOverheatProgress" })
            if (auto* t = Opt(n)) if (auto f = Img().FuncOf(t->site)) g.Hook(*f, ReturnZeroF);
        g.Bind("ship.no_heat");
    }

    auto addFloor = [](const char* id, const char* target, Reg* reg, safetyhook::MidHookFn fn) {
        auto* t = Need(target, { id });
        if (!t) return;
        auto in = Dec(t->site);
        if (!in || in->ops.size() < 2 || !IsGpr(in->ops[1].reg)) {
            // lea ecx,[rdx+rbx] form: the damage register is the index
            if (in && in->mnemonic == "lea") { reg->reg = ZYDIS_REGISTER_EBX; BindMid(id, t->site, fn); return; }
            return Fail(id, "unexpected code at the site");
        }
        reg->reg = in->ops[1].reg;
        BindMid(id, t->site, fn);
    };
    addFloor("ship.hull", "Weapons.ShipHullFloor", &g_hull, NoLoss<&g_hull>);
    addFloor("exo.health", "Weapons.PlayerHealthFloor", &g_health, NoLoss<&g_health>);
    addFloor("exo.shield", "Weapons.PlayerShieldFloor", &g_shieldSuit, NoChange<&g_shieldSuit>);
    addFloor("ship.shield", "Weapons.ShipShieldFloor", &g_shieldShip, NoChange<&g_shieldShip>);

    if (auto* t = Need("Weapons.NoTechBreak", { "exo.no_break" }))
        if (auto f = Img().FuncOf(t->site)) BindMid("exo.no_break", *f, SkipVoid);

    // Exocraft weapons: the drained units come from cvttss2si; zero its source.
    {
        Group g;
        int i = 0;
        for (const char* n : { "Weapons.ExoLaserDrain", "Weapons.ExoGunDrain" }) {
            Reg* r = i++ == 0 ? &g_exoA : &g_exoB;
            if (auto* t = Opt(n)) {
                auto in = Dec(t->site);
                if (in && in->ops.size() == 2 && IsXmm(in->ops[1].reg)) {
                    r->reg = in->ops[1].reg;
                    g.Hook(t->site, r == &g_exoA ? ZeroXmm<&g_exoA> : ZeroXmm<&g_exoB>);
                }
            }
        }
        g.Bind("vehicle.weapons");
    }

    // Frigates: damage chance on failed events (GcFleetGlobals vector2).
    if (auto* t = Need("Weapons.FrigateDamageChance", { "fleet.no_damage" })) {
        if (!t->global) Fail("fleet.no_damage", "globals field not found");
        else {
            uintptr_t at = t->global;
            auto saved = std::make_shared<std::pair<float, float>>();
            Bound b;
            b.id = "fleet.no_damage";
            b.toggle = [at, saved](bool on) {
                if (on) {
                    if (!Rd(at, saved->first) || !Rd(at + 4, saved->second)) return false;
                    return Wr(at, 0.f) && Wr(at + 4, 0.f);
                }
                return Wr(at, saved->first) && Wr(at + 4, saved->second);
            };
            Ok(std::move(b));
        }
    }

    // Difficulty FuelUse = Free (the game's own option), offset decoded at the reload site.
    if (auto* t = Need("Weapons.ReloadFreeAmmo", { "inv.free_fuel" })) {
        auto in = Dec(t->site);
        if (!in || in->ops.size() != 2 || in->ops[1].kind != hooks::Operand::Mem) Fail("inv.free_fuel", "unexpected code at the site");
        else {
            int64_t off = in->ops[1].disp;
            auto saved = std::make_shared<int32_t>(-1);
            Bound b;
            b.id = "inv.free_fuel";
            b.toggle = [off, saved](bool on) {
                uintptr_t d = game::Data();
                if (!d) return false;
                if (on) { if (!Rd(d + off, *saved)) return false; return Wr<int32_t>(d + off, 0); }
                return *saved < 0 || Wr<int32_t>(d + off, *saved);
            };
            Ok(std::move(b));
        }
    }

    // Tech charge and repair, refreshed on the game thread.
    {
        Bound b;
        b.id = "inv.tech_charge";
        b.toggle = [](bool on) { g_fill = on; return true; };
        Ok(std::move(b));
        // Ship only. Either way one pass runs, which repairs charge left broken by older builds.
        Bound s;
        s.id = "ship.tech_charge";
        s.toggle = [](bool on) { g_shipFill = on; g_shipFix = true; return true; };
        Ok(std::move(s));
        Bound r;
        r.id = "inv.tech_repair";
        r.toggle = [](bool on) { g_repair = on; return true; };
        Ok(std::move(r));
        for (auto [id, flag] : { std::pair{ "exo.life_support", &g_life }, { "mt.terrain", &g_terrain }, { "ship.hyperdrive", &g_hyper }, { "inv.max_stacks", &g_stacks } }) {
            Bound x;
            x.id = id;
            x.toggle = [flag](bool on) { *flag = on; return true; };
            Ok(std::move(x));
        }
    }
}

void WeaponsTick()
{
    if ((g_fill || g_shipFill || g_shipFix || g_repair || g_life || g_terrain || g_hyper || g_stacks) && game::FrameHookReady()) game::Post(FillAll);
}

}
