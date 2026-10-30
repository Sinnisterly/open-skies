#include "features/Building.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "hooks/Meta.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>

// Base building: freestyle placement, all parts, and a higher base upload limit.
namespace os::features {

namespace {

// Every placement check reports its error through one call (state, clear, error, text); the ghost
// goes red and placing stops for any error from 15 up. Freestyle drops all but missing resources.
safetyhook::InlineHook g_report;
std::atomic<bool> g_dropErrors{ false };
constexpr int32_t kNoResources = 14;

void ReportHk(void* state, bool clear, int32_t err, void* text)
{
    if (g_dropErrors && !clear && err != kNoResources) return;
    g_report.call<void>(state, clear, err, text);
}

// A few checks store their error directly instead of reporting it: those stores are skipped.
// One callback per store, since a mid hook's context does not say which store it is.
constexpr int kMaxStores = 12;
uintptr_t g_storeNext[kMaxStores] = {}; // address after each store
template <int N> void SkipStore(Context& c)
{
    if (g_dropErrors) c.rip = g_storeNext[N];
}
template <int... N> constexpr safetyhook::MidHookFn kSkips[] = { &SkipStore<N>... };
const auto& kStoreFns = kSkips<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11>;

// The 3000 part check: the immediate of cmp reg, 3000.
struct Limit { uintptr_t at = 0; uint32_t orig = 0; };

// Part table edits, modelled on the Beyond Base Building mod. Each part's flags are saved once,
// then every change restores them and applies the rules of whatever is on.
safetyhook::InlineHook g_avail;
std::atomic<bool> g_allParts{ false }, g_free{ false };
uintptr_t g_tableSlot = 0;

enum Fl { Show, Placeable, Floating, Decoration, Removes, Unsnapped, Scale, Colour, Material, PlayerCol, HolderCol,
          AboveWater, Underwater, Freighter, PlanetBase, Station, StationBack, ShipStruct, ShipDeco, Planet, FlCount };
enum Lim { PlanetLim, RegionLim, BaseLim, FreighterLim, CorvetteLim, LimCount };
struct
{
    uint32_t objects = 0x1C8, stride = 0x278, id = 0x1C8, groups = 0x1A8, groupSize = 0x24, group = 0, subName = 0x10, deco = 0x218;
    uint32_t fl[FlCount] = { 623, 616, 617, 613, 620, 621, 604, 600, 601, 607, 606, 588, 599, 591, 593, 597, 596, 590, 589, 592 };
    uint32_t lim[LimCount] = { 564, 568, 560, 540, 532 };
    uint32_t lo = 0, hi = 0; // saved byte range
} E;
const char* kFlNames[FlCount] = { "ShowInBuildMenu", "IsPlaceable", "IsPlaceableFloatingInSpace", "IsDecoration", "RemovesAttachedDecoration",
                                  "RemovesWhenUnsnapped", "CanScale", "CanChangeColour", "CanChangeMaterial", "CheckPlayerCollision",
                                  "CheckPlaceholderCollision", "BuildableAboveWater", "BuildableUnderwater", "BuildableOnFreighter",
                                  "BuildableOnPlanetBase", "BuildableOnSpaceStationBase", "BuildableOnSpaceStationBackSection",
                                  "BuildableInShipStructural", "BuildableInShipDecorative", "BuildableOnPlanet" };
const char* kLimNames[LimCount] = { "PlanetLimit", "RegionLimit", "PlanetBaseLimit", "FreighterBaseLimit", "CorvetteBaseLimit" };
constexpr int32_t kTerrain = 3, kNormal = 0; // GcBaseBuildingObjectDecorationTypes

struct { uintptr_t data = 0; int32_t n = 0; std::vector<uint8_t> bytes; } g_saved; // game thread
struct GroupCopy { uintptr_t at; std::vector<uint8_t> bytes; };
std::vector<GroupCopy> g_savedGroups; // group records lent a planet tab, restored on every sync
struct { bool all = false, free = false; uintptr_t data = 0; int32_t n = 0; } g_applied;

bool AvailHk(void* menu, uintptr_t entry, bool flag)
{
    int32_t groups = 0;
    if (g_allParts && Rd(entry + E.groups + 8, groups) && groups > 0) return true;
    return g_avail.call<bool>(menu, entry, flag);
}

std::string Id16(uintptr_t a)
{
    char b[17] = {};
    for (int i = 0; i < 16 && Rd(a + i, b[i]) && b[i]; ++i) {}
    return b;
}

bool In(const std::string& id, std::initializer_list<const char*> l)
{
    for (const char* x : l) if (id == x) return true;
    return false;
}

bool Numbered(const std::string& id, const char* prefix) // CONTAINER0..9, S_CONTAINER0..9
{
    size_t n = strlen(prefix);
    return id.size() > n && id.compare(0, n, prefix) == 0 && std::all_of(id.begin() + n, id.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

// Build-count limits the mod keeps: parts that break or flood the region when repeated.
struct Cap { const char* id; int32_t v[LimCount]; };
const Cap kCaps[] = {
    { "POWERLINE_HIDER", { 0, 0, 1, 1, 1 } }, { "NPCFRIGTERM", { 0, 0, 10, 10, 10 } }, { "NPCBUILDERTERM", { 0, 0, 1, 1, 1 } },
    { "NPCVEHICLETERM", { 0, 0, 1, 1, 1 } }, { "NPCWEAPONTERM", { 0, 0, 1, 1, 1 } }, { "NPCSCIENCETERM", { 0, 0, 1, 1, 1 } },
    { "NPCFARMTERM", { 0, 0, 1, 1, 1 } }, { "BASECAPSULE", { 0, 1, 0, 0, 0 } }, { "SUMMON_GARAGE", { 0, 9, 0, 0, 0 } },
    { "GARAGE_B", { 0, 9, 0, 0, 0 } }, { "GARAGE_S", { 0, 9, 0, 0, 0 } }, { "GARAGE_M", { 0, 9, 0, 0, 0 } },
    { "GARAGE_L", { 0, 9, 0, 0, 0 } }, { "GARAGE_SUB", { 0, 9, 0, 0, 0 } }, { "GARAGE_MECH", { 0, 9, 0, 0, 0 } },
    { "BUILDSIGNAL", { 0, 6, 0, 0, 0 } }, { "BUILDBEACON", { 0, 6, 0, 0, 0 } }, { "BUILDSAVE", { 0, 6, 0, 0, 0 } },
    { "MESSAGE", { 0, 2, 0, 0, 0 } }, { "MESSAGEMODULE", { 0, 1, 0, 0, 0 } }, { "BLD_FIREPIT", { 0, 6, 0, 0, 0 } },
    { "BUILDLANDINGPAD", { 0, 0, 12, 0, 0 } }, { "S_LANDINGZONE", { 0, 0, 12, 0, 0 } },
};

void Set(uintptr_t e, Fl f, bool v) { Wr<uint8_t>(e + E.fl[f], v ? 1 : 0); }

void AllPartsRules(uintptr_t e, const std::string& id, const std::vector<std::string>& subs)
{
    auto sub = [&](const char* s) {
        for (auto& x : subs) if (x.find(s) != std::string::npos) return true;
        return false;
    };
    if (!subs.empty()) Set(e, Show, true);
    if (!In(id, { "FRE_FACE_DOOR_A", "FRE_FACE_WALL" })) Set(e, Placeable, true); // these two break freighter interiors
    Set(e, Floating, true);
    Set(e, Colour, true);
    Set(e, Material, true);
    Set(e, Removes, false);
    Set(e, Decoration, sub("WALLDECALS") || sub("WALLPOSTERS")); // decals still go when their wall goes
    bool vehicle = sub("PLANETEXOCRAFT"), prefab = sub("ROOMS") || sub("FREIGHTER");
    if (In(id, { "BUILDLADDER", "FRE_ROOM_LADDER", "WALLFLOORLADDER" })) Set(e, Scale, true);
    else if (!vehicle && !prefab && !Numbered(id, "S_CONTAINER") && !In(id, { "BASE_FLAG", "SB_BEACON", "U_EXTRACTOR_S", "U_GASEXTRACTOR" }))
        Set(e, Scale, true);
    if (!In(id, { "GARAGE_SUB", "WATERBUBBLE" })) Set(e, AboveWater, true);
    if (id != "SB_BEACON") Set(e, Underwater, true);
    if (!vehicle && !Numbered(id, "CONTAINER") && !id.starts_with("GARAGE") && id != "SUMMON_GARAGE" && !id.starts_with("NPC") &&
        !In(id, { "BASE_FLAG", "SB_BEACON", "BASECAPSULE", "MESSAGE", "BUILDLANDINGPAD", "S_LANDINGZONE", "BUILDSIGNAL", "BUILDBEACON",
                  "U_EXTRACTOR_S", "U_GASEXTRACTOR", "U_SILO_S", "U_GENERATOR_S", "U_PIPELINE", "CREATURE_FEED", "CREATURE_FARM",
                  "BUILDHARVESTER", "BUILDGASHARVEST", "TELEPORTER", "O2_HARVESTER" }))
        Set(e, Freighter, true);
    if (!In(id, { "GARAGE_FREIGHT", "TELEPORTER_F", "SB_BEACON" })) Set(e, PlanetBase, true);
    if (id != "SB_BEACON") {
        Set(e, Station, true);
        Set(e, StationBack, true);
    }
    uint8_t st = 0, de = 0;
    Rd(e + E.fl[ShipStruct], st);
    Rd(e + E.fl[ShipDeco], de);
    if (!st && !de && !In(id, { "U_MINIPORTAL", "SB_BEACON" })) Set(e, ShipDeco, true); // corvettes
    const Cap* cap = nullptr;
    for (auto& c : kCaps) if (id == c.id) cap = &c;
    for (int k = 0; k < LimCount; ++k) Wr<int32_t>(e + E.lim[k], cap ? cap->v[k] : 0);
}

void FreestyleRules(uintptr_t e, const std::string& id)
{
    Set(e, Placeable, !In(id, { "FRE_FACE_DOOR_A", "FRE_FACE_WALL" }));
    Set(e, Floating, true);
    Set(e, PlayerCol, false);
    Set(e, HolderCol, false);
    Set(e, Unsnapped, false);
    Set(e, Removes, false);
    // Outside a base too, except the power and resource parts that stop working there.
    if (!In(id, { "U_SOLAR_S", "U_BIOGENERATOR", "U_BATTERY_S", "U_GENERATOR_S", "U_EXTRACTOR_S", "U_GASEXTRACTOR", "U_SILO_S" }))
        Set(e, Planet, true);
    int32_t d = 0;
    if (Rd(e + E.deco, d) && d == kTerrain && !In(id, { "O2_HARVESTER", "BUILDGASHARVEST" })) Wr<int32_t>(e + E.deco, kNormal); // not bare ground only
}

// Game thread. Brings the table in line with the toggles; cheap when nothing changed.
void SyncParts()
{
    bool all = g_allParts, free = g_free;
    uintptr_t table = 0, data = 0;
    int32_t n = 0;
    if (!g_tableSlot || !Rd(g_tableSlot, table) || !table || !Rd(table + E.objects, data) || !Rd(table + E.objects + 8, n) || !data || n <= 0 ||
        n > 20000)
        return;
    if (g_applied.all == all && g_applied.free == free && g_applied.data == data && g_applied.n == n) return;
    size_t span = E.hi - E.lo;
    if (g_saved.data != data || g_saved.n != n) { // first use, or the game reloaded the table
        g_saved.data = data;
        g_saved.n = n;
        g_saved.bytes.assign(span * n, 0);
        g_savedGroups.clear(); // the old table is gone, nothing to restore
        for (int32_t i = 0; i < n; ++i)
            for (size_t b = 0; b < span; ++b) Rd(data + (uintptr_t)i * E.stride + E.lo + b, g_saved.bytes[i * span + b]);
    }
    for (auto& c : g_savedGroups)
        for (size_t b = 0; b < c.bytes.size(); ++b) Wr<uint8_t>(c.at + b, c.bytes[b]);
    g_savedGroups.clear();
    // Station decor only sits in the station exterior tab; with all parts on it borrows the station sofa's furniture group.
    std::vector<uint8_t> furniture;
    if (all)
        for (int32_t i = 0; i < n && furniture.empty(); ++i) {
            uintptr_t e = data + (uintptr_t)i * E.stride, g = 0;
            if (Id16(e + E.id) != "STATION_SOFA" || !Rd(e + E.groups, g) || !g) continue;
            furniture.resize(E.groupSize);
            for (uint32_t b = 0; b < E.groupSize; ++b) Rd(g + b, furniture[b]);
        }
    int changed = 0, lent = 0;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t e = data + (uintptr_t)i * E.stride;
        for (size_t b = 0; b < span; ++b) Wr<uint8_t>(e + E.lo + b, g_saved.bytes[i * span + b]);
        if (!all && !free) continue;
        std::string id = Id16(e + E.id);
        std::vector<std::string> subs;
        uintptr_t g = 0;
        int32_t gn = 0;
        bool stationOnly = false;
        if (Rd(e + E.groups, g) && Rd(e + E.groups + 8, gn) && g && gn > 0 && gn < 64) {
            stationOnly = true;
            for (int32_t k = 0; k < gn; ++k) {
                subs.push_back(Id16(g + (uintptr_t)k * E.groupSize + E.subName));
                std::string grp = Id16(g + (uintptr_t)k * E.groupSize + E.group);
                stationOnly = stationOnly && grp.starts_with("STATION_EXT");
            }
        }
        if (all && stationOnly && !furniture.empty()) {
            GroupCopy c{ g, std::vector<uint8_t>(E.groupSize) };
            for (uint32_t b = 0; b < E.groupSize; ++b) Rd(g + b, c.bytes[b]);
            g_savedGroups.push_back(std::move(c));
            for (uint32_t b = 0; b < E.groupSize; ++b) Wr<uint8_t>(g + b, furniture[b]);
            ++lent;
        }
        if (all) AllPartsRules(e, id, subs);
        if (free) FreestyleRules(e, id);
        ++changed;
    }
    g_applied = { all, free, data, n };
    log::Info("Building", "Part table: all parts {}, freestyle {}, {} of {} parts edited, {} station parts in furniture", all, free, changed, n, lent);
}

void Tick()
{
    if (game::Frame() % 30 == 0) SyncParts();
}

}

void InstallBuilding()
{
    // Error reporter, and the direct error stores in the functions that report.
    std::vector<safetyhook::MidHook*> stores;
    if (const Target& rt = Loc().Resolve("Building.ReportError"); rt.ok) {
        int64_t errOff = -1;
        auto in = Dec(rt.func);
        for (int i = 0; in && i < 12; ++i, in = Next(*in))
            if (in->mnemonic == "mov" && in->ops.size() == 2 && in->ops[0].kind == hooks::Operand::Mem && in->ops[1].kind == hooks::Operand::Imm &&
                in->ops[1].imm == 0x1A) { errOff = in->ops[0].disp; break; }
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(rt.func)), reinterpret_cast<void*>(&ReportHk))) g_report = std::move(*h);
        for (uint32_t f : errOff > 0 ? Img().Callers(rt.func) : std::vector<uint32_t>{})
            for (auto& r : Img().FuncRanges(f))
                for (auto& x : Img().DecodeRange(r.first, r.second)) {
                    if (x.mnemonic != "mov" || x.ops.size() != 2 || x.ops[0].kind != hooks::Operand::Mem || x.ops[0].disp != errOff ||
                        x.ops[1].kind != hooks::Operand::Imm || x.ops[1].imm >= 0x1A || x.ops[1].imm == kNoResources)
                        continue;
                    size_t n = stores.size();
                    if (n >= kMaxStores) continue;
                    g_storeNext[n] = Abs(x.rva) + x.len;
                    if (auto* h = Mid(x.rva, kStoreFns[n]); h && h->enable()) {
                        stores.push_back(h);
                    }
                }
        log::Info("Building", "Error reporter at +0x{:X}, error +0x{:X}, {} direct stores", rt.func, errOff, stores.size());
    }
    if (!g_report) Fail("build.freestyle", "placement error reporter not found");
    else {
        Bound b;
        b.id = "build.freestyle";
        b.toggle = [](bool on) {
            g_free = on; // the part flags follow on the next sync
            g_dropErrors = on;
            return true;
        };
        Ok(std::move(b));
    }

    std::vector<Limit> limits;
    for (const char* name : { "Base.UploadLimitA", "Base.UploadLimitB", "Base.UploadLimitC" }) {
        const Target& t = Loc().Resolve(name);
        auto in = t.ok ? Dec(t.site) : std::nullopt;
        if (!in || in->ops.size() != 2 || in->ops[1].kind != hooks::Operand::Imm || in->ops[1].imm != 3000) continue;
        uintptr_t at = Abs(t.site) + in->len - 4; // imm32 is the last field
        uint32_t v = 0;
        if (Rd(at, v) && v == 3000) limits.push_back({ at, v });
    }
    if (limits.empty()) Fail("build.upload_limit", "limit checks not found");
    else {
        Bound b;
        b.id = "build.upload_limit";
        b.toggle = [limits](bool on) {
            bool ok = true;
            for (auto& l : limits) {
                uint32_t v = on ? 10000 : l.orig;
                ok = WriteCode(l.at, reinterpret_cast<const uint8_t*>(&v), 4) && ok;
            }
            return ok;
        };
        Ok(std::move(b));
    }
    const Target& tt = Loc().Resolve("Building.Table");
    const Target& at = Loc().Resolve("Building.PartAvailable");
    if (!tt.ok || !tt.global || !at.ok) Fail("build.all_parts", !tt.ok ? tt.reason : at.reason);
    else {
        g_tableSlot = tt.global;
        namespace meta = hooks::meta;
        auto off = [](const char* cls, const char* f, uint32_t& out) { if (auto m = meta::Path(cls, f)) out = m->offset; };
        off("cGcBaseBuildingTable", "Objects", E.objects);
        if (auto* ci = meta::Get("cGcBaseBuildingEntry")) E.stride = ci->size;
        if (auto* ci = meta::Get("cGcBaseBuildingEntryGroup")) E.groupSize = ci->size;
        off("cGcBaseBuildingEntryGroup", "SubGroupName", E.subName);
        off("cGcBaseBuildingEntryGroup", "Group", E.group);
        off("cGcBaseBuildingEntry", "ID", E.id);
        off("cGcBaseBuildingEntry", "Groups", E.groups);
        off("cGcBaseBuildingEntry", "DecorationType", E.deco);
        for (int k = 0; k < FlCount; ++k) off("cGcBaseBuildingEntry", kFlNames[k], E.fl[k]);
        for (int k = 0; k < LimCount; ++k) off("cGcBaseBuildingEntry", kLimNames[k], E.lim[k]);
        E.lo = E.deco;
        E.hi = E.deco + 4;
        for (uint32_t o : E.fl) { E.lo = std::min(E.lo, o); E.hi = std::max(E.hi, o + 1); }
        for (uint32_t o : E.lim) { E.lo = std::min(E.lo, o); E.hi = std::max(E.hi, o + 4); }
        game::EveryFrame(Tick);
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(at.func)), reinterpret_cast<void*>(&AvailHk));
        if (!h) Fail("build.all_parts", "hook could not be placed");
        else {
            g_avail = std::move(*h);
            Bound b;
            b.id = "build.all_parts";
            b.toggle = [](bool on) {
                if (!game::FrameHookReady()) return false;
                g_allParts = on; // applied by the next sync
                return true;
            };
            Ok(std::move(b));
        }
    }
    log::Info("Building", "Ready: {} upload checks, parts +0x{:X}..0x{:X}", limits.size(), E.lo, E.hi);
}

}
