#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/Settings.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <intrin.h>

namespace os::features {

namespace {

namespace meta = hooks::meta;

// One globals field. Offsets come from the game's reflection data; the fallback is used
// only when that data is empty and the class size still matches.
struct G
{
    const char* cls;   // e.g. "GcSkyGlobals"
    const char* path;  // e.g. "Control.SpaceEngine.MaxSpeed"
    int slider = 0;
    bool inverse = false;
    double k = 1;
    double (*map)(float) = nullptr;
    uint32_t fallback = 0, size = 0;
    int8_t type = -1; // fallback type, see Field
};

int TypeOf(uint32_t t, uint32_t esz)
{
    switch (t) {
    case meta::Float: return 0;
    case meta::Int32: return 1;
    case meta::UInt64: return 2;
    case meta::Bool: return 3;
    case meta::StaticArray: return esz == 8 ? 2 : esz == 1 ? 3 : 1;
    case 0x27: return 0; // Vector2f
    default: return -1;
    }
}

bool Add(const G& g, std::vector<Field>& out, std::string& why)
{
    std::string cls = std::string("c") + g.cls;
    uintptr_t inst = Loc().GlobalsInstance(cls);
    if (!inst) { why = std::string(g.cls) + " not found"; return false; }
    uint32_t off = 0, esz = 4, count = 1;
    int type = -1;
    if (auto m = meta::Path(cls, g.path)) {
        off = m->offset;
        esz = m->size ? m->size : 4;
        type = TypeOf(m->type, esz);
        if (m->type == meta::StaticArray) {
            count = m->count;
            if (g.type >= 0) type = g.type; // the element type is not in the metadata
        }
        if (m->type == 0x27) { count = 2; esz = 4; }
    }
    else if (g.fallback) {
        auto* ci = meta::Get(cls);
        if (ci && ci->size != g.size) { why = std::string(g.path) + " not found"; return false; }
        off = g.fallback;
        type = g.type < 0 ? 0 : g.type;
        esz = type == 2 ? 8 : type == 3 ? 1 : 4;
    }
    else { why = std::string(g.path) + " not found"; return false; }
    if (type < 0) { why = std::string(g.path) + " has an unexpected type"; return false; }
    for (uint32_t i = 0; i < count && i < 256; ++i) {
        Field f;
        f.at = inst + off + i * esz;
        f.slider = g.slider;
        f.type = (uint8_t)type;
        f.map = g.map;
        f.inverse = g.inverse;
        f.k = g.k;
        out.push_back(f);
    }
    return true;
}

// Binds a feature to globals fields. Fields that are missing are skipped as long as one resolves.
void Data(const char* id, std::initializer_list<G> list, size_t sliders)
{
    std::vector<Field> fields;
    std::string why;
    for (const G& g : list) {
        std::string w;
        if (!Add(g, fields, w)) {
            log::Warn("Tuner", "{}: {}", id, w);
            if (why.empty()) why = w;
        }
    }
    if (fields.empty()) return Fail(id, why.empty() ? "no globals field found" : why);
    BindData(id, std::move(fields), sliders);
}

double One(float) { return 1; }
double Zero(float) { return 0; }
double SunAngle(float hour) { return std::fmod((hour - 6.f) * 15.f + 720.f, 360.f); }

constexpr uint32_t kPlayerG = 0x1BB0, kShipG = 0x1EC0, kPlayGame = 0x1CC0, kDebug = 0x2280;

void Tuner()
{
    Data("tune.foot", {
        { "GcPlayerGlobals", "GroundRunSpeed", 0, false, 1, nullptr, 0x12D4, kPlayerG },
        { "GcPlayerGlobals", "GroundWalkSpeed", 0, false, 1, nullptr, 0x12EC, kPlayerG } }, 1);
    Data("tune.swim", {
        { "GcPlayerGlobals", "UnderwaterSwimMaxSpeed", 0, false, 1, nullptr, 0x1AB0, kPlayerG },
        { "GcPlayerGlobals", "SurfaceSwimMaxSpeed", 0, false, 1, nullptr, 0x193C, kPlayerG } }, 1);
    Data("tune.stamina", {
        { "GcPlayerGlobals", "StaminaRate", 0, true, 1, nullptr, 0x18F0, kPlayerG },
        { "GcPlayerGlobals", "StaminaRecoveryRate", 0, false, 1, nullptr, 0x18F8, kPlayerG } }, 1);
    Data("tune.jet_refill", {
        { "GcPlayerGlobals", "JetpackFillRate", 0, false, 1, nullptr, 0x1490, kPlayerG },
        { "GcPlayerGlobals", "JetpackFillRateMidair", 0, false, 1, nullptr, 0x1498, kPlayerG } }, 1);
    Data("tune.shield", {
        { "GcPlayerGlobals", "ShieldRechargeRate", 0, false, 1, nullptr, 0x1818, kPlayerG },
        { "GcPlayerGlobals", "ShieldRechargeMinTimeSinceDamage", 0, true, 1, nullptr, 0x1814, kPlayerG } }, 1);
    Data("tune.terrain", {
        { "GcTerrainGlobals", "TerrainEditing.EditSizes", 0, false, 1, nullptr, 0, 0, 0 },
        { "GcTerrainGlobals", "TerrainEditing.SubtractSizes", 0, false, 1, nullptr, 0, 0, 0 },
        { "GcTerrainGlobals", "TerrainEditing.FlatteningSizes", 0, false, 1, nullptr, 0, 0, 0 },
        { "GcTerrainGlobals", "TerrainEditing.TerrainEditBeamMaxRange", 1 } }, 2);
    Data("tune.mining", {
        { "GcPlayerGlobals", "LaserMiningDamageMultiplier", 0, false, 1, nullptr, 0x1514, kPlayerG } }, 1);

    Data("tune.pulse", {
        { "GcSpaceshipGlobals", "MiniWarpChargeTime", 0, true, 1, nullptr, 0x16DC, kShipG },
        { "GcSpaceshipGlobals", "MiniWarpCooldownTime", 0, true, 1, nullptr, 0x16E0, kShipG } }, 1);
    {
        // Space flight: every ship class has its own control block.
        static const char* blocks[] = { "Control", "ControlCorvette", "ControlHeavy", "ControlHeavyHover", "ControlHover", "ControlLight" };
        static const uint32_t base[] = { 0x310, 0x528, 0x740, 0x958, 0xB70, 0xD88 };
        static std::vector<std::string> paths;
        std::vector<Field> fields;
        std::string why;
        for (int b = 0; b < 6; ++b) {
            for (auto [name, off, slider] : { std::tuple{ "MaxSpeed", 0x44u, 0 }, { "ThrustForce", 0x64u, 0 }, { "BoostMaxSpeed", 0x10u, 1 } }) {
                paths.push_back(std::string(blocks[b]) + ".SpaceEngine." + name);
                G g{ "GcSpaceshipGlobals", paths.back().c_str(), slider, false, 1, nullptr, base[b] + 0x15C + off, kShipG };
                std::string w;
                if (!Add(g, fields, w) && why.empty()) why = w;
            }
        }
        if (fields.empty()) Fail("tune.space", why);
        else BindData("tune.space", std::move(fields), 2);
    }
    Data("tune.warp", {
        { "GcSpaceshipGlobals", "Warp.TravelTunnelTime", 0, true, 1, nullptr, 0x1160, kShipG },
        { "GcSpaceshipGlobals", "Warp.EntryTime", 0, true, 1, nullptr, 0x1158, kShipG } }, 1);
    Data("tune.ship_recharge", {
        { "GcSpaceshipGlobals", "BoostChargeRate", 0, false, 1, nullptr, 0x12CC, kShipG },
        { "GcSpaceshipGlobals", "LaunchThrustersRegenTimePeriod", 0, true, 1, nullptr, 0x1680, kShipG } }, 1);

    Data("tune.vehicle_fuel", {
        { "GcVehicleGlobals", "VehicleFuelRate", 0, true },
        { "GcVehicleGlobals", "VehicleBoostFuelRate", 0, true } }, 1);
    Data("tune.vehicle_boost", {
        { "GcVehicleGlobals", "VehicleBoostSpeedMultiplierPercent", 0 },
        { "GcVehicleGlobals", "VehicleJumpCooldown", 0, true } }, 1);
    Data("tune.vehicle_laser", {
        { "GcVehicleGlobals", "MiningLaserSpeed", 0 },
        { "GcVehicleGlobals", "MiningLaserDrainSpeed", 0, true },
        { "GcVehicleGlobals", "MiningLaserRadius", 1 } }, 2);

    Data("tune.day", { { "GcSkyGlobals", "DayLength", 0 } }, 1);
    Data("tune.storm_freq", {
        { "GcSkyGlobals", "MinTimeBetweenStormsLow", 0, true }, { "GcSkyGlobals", "MaxTimeBetweenStormsLow", 0, true },
        { "GcSkyGlobals", "MinTimeBetweenStormsHigh", 0, true }, { "GcSkyGlobals", "MaxTimeBetweenStormsHigh", 0, true },
        { "GcSkyGlobals", "MinStormLengthLow", 1 }, { "GcSkyGlobals", "MaxStormLengthLow", 1 },
        { "GcSkyGlobals", "MinStormLengthHigh", 1 }, { "GcSkyGlobals", "MaxStormLengthHigh", 1 } }, 2);
    Data("tune.space_hazard", {
        { "GcEnvironmentGlobals", "SpaceRadiation", 0 },
        { "GcEnvironmentGlobals", "SpaceToxicity", 0 } }, 1);
}

// ---- hooks ----

std::atomic<float> g_speed{ 1.f };

void ClockCb(Context& c)
{
    float s = g_speed;
    if (s == 1.f) return;
    c.xmm1.f32[0] *= s;
    Wr<float>(c.rbx + 0x18, c.xmm1.f32[0]);
}

void StormCb(Context& c)
{
    uint32_t s = (uint32_t)c.rdx;
    if (s >= 1 && s <= 3) c.rdx = (c.rdx & ~0xFFFFFFFFull);
}

// Boot: the intro movies follow SkipLogos, the logo screens fade at BootLogoFadeRate. Held every
// frame, since the globals load can land after we start.
uintptr_t g_skipLogos = 0, g_logoFade = 0;
void LogoTick()
{
    if (!Settings::Get().Data().skipLogos) return;
    uint8_t v = 1;
    if (Rd(g_skipLogos, v) && !v) Wr<uint8_t>(g_skipLogos, 1);
    float f = 0;
    if (g_logoFade && Rd(g_logoFade, f) && f < 100.f) Wr<float>(g_logoFade, 100.f);
}

}

void InstallLogoSkip()
{
    uintptr_t inst = Loc().GlobalsInstance("cGcDebugOptions");
    if (!inst) return;
    auto m = meta::Path("cGcDebugOptions", "SkipLogos");
    auto f = meta::Path("cGcDebugOptions", "BootLogoFadeRate");
    g_skipLogos = inst + (m ? m->offset : 0x223E);
    g_logoFade = inst + (f ? f->offset : 0x708);
    LogoTick();
    game::EveryFrame(LogoTick);
}

namespace {

void World()
{
    Data("world.portal", {
        { "GcGameplayGlobals", "Portal.KnowAllRunes", -1, false, 1, One, 0x141C, kPlayGame, 3 },
        { "GcGameplayGlobals", "Portal.SkipRuneEntry", -1, false, 1, One, 0x141D, kPlayGame, 3 } }, 0);
    Data("world.time", {
        { "GcDebugOptions", "EnableDayNightCycle", -1, false, 1, Zero, 0x219E, kDebug, 3 },
        { "GcDebugOptions", "ForceSunAngle", 0, false, 1, SunAngle, 0x774, kDebug, 0 } }, 1);

    if (auto* t = Need("Sky.SetStormState", { "world.storms" })) BindMid("world.storms", t->site, StormCb);

    if (auto* t = Need("Clock.Step", { "world.speed" })) {
        auto* h = Mid(t->site, ClockCb);
        if (!h) Fail("world.speed", "hook could not be placed");
        else {
            Bound b;
            b.id = "world.speed";
            b.toggle = HookToggle(h);
            b.slider = [](int i, float v) { if (i == 0) g_speed = std::clamp(v, 0.1f, 3.f); };
            Ok(std::move(b));
        }
    }
}

void Building()
{
    Data("build.settlement", {
        { "GcSettlementGlobals", "BuildingUpgradeTimeInSeconds", -1, false, 1, One },
        { "GcSettlementGlobals", "BuildingFreeUpgradeTimeInSeconds", -1, false, 1, One },
        { "GcSettlementGlobals", "SettlementBuildingTimes", -1, false, 1, One } }, 0);
    Data("build.corvette", {
        { "GcBuildableShipGlobals", "ComplexityLimitWarning", 0 },
        { "GcBuildableShipGlobals", "ComplexityLimitWarningNX", 0 },
        { "GcBuildableShipGlobals", "PartFXLimits", 0 },
        { "GcBuildingGlobals", "MaximumComplexityDensity", 0 },
        { "GcBuildingGlobals", "BuildingPlacementMaxDistance", 1 } }, 2);
}

// Exosuit slots through the game's own slot purchase, up to the cap the game allows.
using AddSlotFn = void (*)(uintptr_t store, int32_t choice, int32_t shipClass);
using SlotCapFn = int16_t (*)(uintptr_t store, int32_t choice, int32_t shipClass);
AddSlotFn g_addSlot = nullptr;
SlotCapFn g_slotCap = nullptr;
void (*g_notify)(uintptr_t, const void*) = nullptr; // Inventory.Notify(PS+0x900, {0, choice, index})
constexpr int32_t kNoShip = 0xC; // the class the game passes for exosuit and Multi-Tool stores

void Notify(uintptr_t ps, int32_t choice, int32_t index)
{
    struct { uint64_t zero; int32_t choice, index; } msg{ 0, choice, index };
    if (g_notify) g_notify(ps + 0x900, &msg);
}

// Store layout: one u64 bit mask of usable cells per row at +0, width +0x80, height +0x82, target count +0x84.
int UsableSlots(uintptr_t store)
{
    int16_t w = 0, h = 0;
    if (!Rd(store + 0x80, w) || !Rd(store + 0x82, h) || w <= 0 || h <= 0 || w > 64 || h > 16) return -1;
    int n = 0;
    for (int r = 0; r < h; ++r) {
        uint64_t m = 0;
        if (Rd(store + r * 8, m)) n += std::min<int>((int)__popcnt64(m), w);
    }
    return n;
}

// Unlocks the first locked cell inside the grid, the way the slot purchase does.
bool UnlockNext(uintptr_t store)
{
    int16_t w = 0, h = 0;
    if (!Rd(store + 0x80, w) || !Rd(store + 0x82, h) || w <= 0 || h <= 0 || w > 64 || h > 16) return false;
    for (int r = 0; r < h; ++r) {
        uint64_t m = 0;
        if (!Rd(store + r * 8, m)) return false;
        for (int c = 0; c < w; ++c)
            if (!(m & (1ull << c))) return Wr<uint64_t>(store + r * 8, m | (1ull << c));
    }
    return false;
}

// Adds slots to one store until the game's cap. Returns slots added.
int FillSlots(uintptr_t store, int32_t choice, int32_t shipClass)
{
    int added = 0;
    for (int i = 0; i < 200; ++i) {
        int have = UsableSlots(store);
        if (have < 0 || have >= g_slotCap(store, choice, shipClass)) break;
        g_addSlot(store, choice, shipClass); // raises the target and grows the grid when needed
        if (!UnlockNext(store)) break;
        ++added;
    }
    return added;
}

// Store choices from the game's store lookup: exosuit 0-2, Multi-Tool 3, starship 4-6, freighter 7-9.
enum class SlotTarget { Suit, Tool, Ship, Freighter };

void MaxSlots(SlotTarget t)
{
    uintptr_t ps = game::PlayerState(), d = game::Data();
    if (!ps || !d || !g_addSlot || !g_slotCap) return;
    auto store = [&](int32_t choice) { return ps + 0x910 + choice * 0x248; };
    int added = 0;
    switch (t) {
    case SlotTarget::Suit:
        for (int32_t c = 0; c < 3; ++c) { added += FillSlots(store(c), c, kNoShip); Notify(ps, c, -1); }
        break;
    case SlotTarget::Tool:
        added = FillSlots(store(3), 3, kNoShip);
        Notify(ps, 3, -1);
        break;
    case SlotTarget::Ship: {
        int32_t primary = -1, shipClass = -1;
        if (Rd(ps + 0x182A0, primary) && primary >= 0 && primary < 12 && Rd(d + 0xD5F60 + 0x60 + primary * 0x30 + 0x1C, shipClass) && shipClass >= 0 && shipClass < 0xC) {
            static const uint32_t bases[3] = { 0x7458, 0xAB28, 0x8FC8 }; // ship, tech, cargo
            for (int32_t k = 0; k < 3; ++k) { added += FillSlots(ps + bases[k] + primary * 0x248, 4 + k, shipClass); Notify(ps, 4 + k, primary); }
        }
        break;
    }
    case SlotTarget::Freighter: {
        // The freighter row is the larger of the Freighter and PlayerFreighter classes.
        int32_t fc = g_slotCap(store(7), 7, 5) >= g_slotCap(store(7), 7, 0) ? 5 : 0;
        for (int32_t c = 7; c < 10; ++c) { added += FillSlots(store(c), c, fc); Notify(ps, c, -1); }
        break;
    }
    }
    static const char* names[] = { "exosuit", "multitool", "starship", "freighter" };
    log::Info("Inventory", "Added {} {} slots", added, names[(int)t]);
}

void Slots()
{
    auto* t = Need("Inventory.AddSlot", { "inv.slots_suit", "inv.slots_tool", "inv.slots_ship", "inv.slots_freighter" });
    if (!t) return;
    auto at = ScanFunc(t->func, "E8 ?? ?? ?? ?? 66 44 3B D0");
    auto call = at ? Dec(*at) : std::nullopt;
    if (!call || call->mnemonic != "call" || !call->branchTarget) { for (auto* id : { "inv.slots_suit", "inv.slots_tool", "inv.slots_ship", "inv.slots_freighter" }) Fail(id, "slot cap call not found"); return; }
    g_addSlot = (AddSlotFn)Abs(t->func);
    if (const Target& n = Loc().Resolve("Inventory.Notify"); n.ok) g_notify = (decltype(g_notify))Abs(n.func);
    g_slotCap = (SlotCapFn)Abs(*call->branchTarget);
    for (auto [id, target] : { std::pair{ "inv.slots_suit", SlotTarget::Suit }, { "inv.slots_tool", SlotTarget::Tool },
                               { "inv.slots_ship", SlotTarget::Ship }, { "inv.slots_freighter", SlotTarget::Freighter } }) {
        Bound b;
        b.id = id;
        b.action = [target] {
            if (!game::FrameHookReady()) return;
            game::BackupSaveOnce();
            game::Post([target] { MaxSlots(target); });
        };
        Ok(std::move(b));
    }
}

}

namespace {

// Force Weather / Force Storms. The game looks up a planet's weather
// properties from its WeatherType every frame, and its storm timer follows StormFrequency, so
// both are steered through the planets' data. Originals come back when the toggles go off.
struct
{
    uint32_t sys = 0x72AFB0, planets = 0x2E30, stride = 0xD9170;
    uint32_t data = 0x60, index = 0x353C, effect = 0xD9104; // cGcPlanet: planet data, effect slot
    uint32_t type = 0x1CE0 + 0x174, freq = 0x1CE0 + 0x168, intensity = 0x1CE0 + 0x170; // in cGcPlanetData
} W;

std::atomic<bool> g_weatherOn{ false }, g_stormOn{ false };
std::atomic<int> g_weatherType{ 0 }, g_weatherIntensity{ 0 };

struct Saved
{
    int32_t type = 0, freq = 0, intensity = 0;
};
Saved g_saved[6];
bool g_haveSaved[6] = {};
uint64_t g_savedSystem = ~0ull;

void WeatherTick()
{
    static uint64_t last = 0;
    uint64_t now = game::Frame();
    if (now - last < 20) return;
    last = now;
    bool wOn = g_weatherOn, sOn = g_stormOn;
    bool any = std::any_of(std::begin(g_haveSaved), std::end(g_haveSaved), [](bool b) { return b; });
    if (!wOn && !sOn && !any) return;
    uintptr_t d = game::Data(), sys = 0;
    uint64_t ua = 0;
    if (!d || !Rd(d + W.sys, sys) || !sys || !CurrentAddress(ua) || !ua) return;
    uint64_t here = ua & 0x000FFFFFFFFFFFFFull;
    if (here != g_savedSystem) { // a new system starts with its own values
        g_savedSystem = here;
        std::fill(std::begin(g_haveSaved), std::end(g_haveSaved), false);
    }
    for (int i = 0; i < 6; ++i) {
        uintptr_t planet = sys + W.planets + (uintptr_t)i * W.stride, pd = planet + W.data;
        int32_t idx = -1;
        if (!Rd(pd + W.index, idx) || idx != i) break;
        Saved cur;
        if (!Rd(pd + W.type, cur.type) || !Rd(pd + W.freq, cur.freq) || !Rd(pd + W.intensity, cur.intensity)) break;
        if (!g_haveSaved[i]) {
            if (!wOn && !sOn) continue;
            g_saved[i] = cur;
            g_haveSaved[i] = true;
        }
        const Saved& o = g_saved[i];
        Saved want{ wOn ? std::clamp(g_weatherType.load(), 0, 16) : o.type, sOn ? 3 : o.freq, wOn ? std::clamp(g_weatherIntensity.load(), 0, 1) : o.intensity };
        if (want.type != cur.type) {
            Wr<int32_t>(pd + W.type, want.type);
            Wr<int32_t>(planet + W.effect, 0); // effect slots differ per weather
        }
        if (want.freq != cur.freq) Wr<int32_t>(pd + W.freq, want.freq);
        if (want.intensity != cur.intensity) Wr<int32_t>(pd + W.intensity, want.intensity);
        if (!wOn && !sOn) g_haveSaved[i] = false; // restored
    }
}

void Weather()
{
    if (const Target& t = Loc().Resolve("Data.SolarSystem"); t.ok && t.offset) W.sys = (uint32_t)*t.offset;
    if (const Target& t = Loc().Resolve("SolarSystem.Planets"); t.ok && t.offset) W.planets = (uint32_t)*t.offset;
    if (const Target& t = Loc().Resolve("SolarSystem.PlanetStride"); t.ok && t.offset) W.stride = (uint32_t)*t.offset;
    auto off = [](const char* path, uint32_t& v) { if (auto m = meta::Path("cGcPlanetData", path)) v = m->offset; };
    off("PlanetIndex", W.index);
    off("Weather.WeatherType", W.type);
    off("Weather.StormFrequency", W.freq);
    off("Weather.WeatherIntensity", W.intensity);
    log::Info("World", "Weather: system +0x{:X}, planets +0x{:X} x 0x{:X}, type +0x{:X}, storms +0x{:X}", W.sys, W.planets, W.stride, W.type, W.freq);
    game::EveryFrame(WeatherTick);

    Bound w;
    w.id = "world.weather";
    w.toggle = [](bool on) { g_weatherOn = on; return true; };
    w.slider = [](int i, float v) { (i == 0 ? g_weatherType : g_weatherIntensity) = (int)std::lround(v); };
    Ok(std::move(w));
    Bound s;
    s.id = "world.storm_force";
    s.toggle = [](bool on) { g_stormOn = on; return true; };
    Ok(std::move(s));
}

}

void InstallWorldFx()
{
    Tuner();
    World();
    Weather();
    Building();
    Slots();
}

}
