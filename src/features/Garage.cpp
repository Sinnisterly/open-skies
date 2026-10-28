#include "features/Garage.h"
#include "assets/PakIndex.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "core/Settings.h"
#include "features/Bind.h"
#include "game/Catalog.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "render/ModelPreview.h"
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>

// Garage tab: live ship, Multi-Tool, freighter, suit and exocraft editor.
namespace os::features {

namespace {

namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

// Object graph offsets, read from code where possible. The first three are refreshed from targets.
uint32_t kShipOwnership = 0xD5FC0, kToolOwnership = 0x2B3030, kPlayerWeapon = 0x72D1C0;
constexpr uint32_t kWeaponRes = 0x498;
constexpr uint32_t kShipResources = 0x17EE0, kShipInv = 0x7458, kShipCargo = 0x8FC8, kShipTech = 0xAB28, kStride = 0x248;
constexpr uint32_t kToolSlots = 0x10, kToolStride = 0x320, kWeaponInv = 0x910 + 3 * 0x248, kToolCustSlot = 0x2C8;
constexpr uint32_t kShipNames = 0x1ADF3, kNameLen = 0x20, kToolName = 0x284, kShipList = 0x60, kShipEntry = 0x30;
constexpr uint32_t kFreighterOwn = 0x2B91B0, kFreighterName = 0x34, kFreighterRes = 0x2D8, kFreighterStores = 0x910 + 7 * 0x248;
constexpr uint32_t kPrimaryVehicle = 0x17EA0;

enum Kind { ShipKind = 1, ToolKind = 2, FreighterKind = 3, SuitKind = 4, VehicleKind = 5 };
constexpr int kModelChip = 6;

struct Model { const char* path; const char* key; Kind kind; int modType; };
const Model kModels[] = {
    { "MODELS/COMMON/SPACECRAFT/FIGHTERS/FIGHTER_PROC.SCENE.MBIN", "gar.fighter", ShipKind, 1 },
    { "MODELS/COMMON/SPACECRAFT/DROPSHIPS/DROPSHIP_PROC.SCENE.MBIN", "gar.dropship", ShipKind, 2 },
    { "MODELS/COMMON/SPACECRAFT/SCIENTIFIC/SCIENTIFIC_PROC.SCENE.MBIN", "gar.scientific", ShipKind, 3 },
    { "MODELS/COMMON/SPACECRAFT/SHUTTLE/SHUTTLE_PROC.SCENE.MBIN", "gar.shuttle", ShipKind, -1 },
    { "MODELS/COMMON/SPACECRAFT/S-CLASS/S-CLASS_PROC.SCENE.MBIN", "gar.exotic", ShipKind, -1 },
    { "MODELS/COMMON/SPACECRAFT/S-CLASS/BIOPARTS/BIOSHIP_PROC.SCENE.MBIN", "gar.living", ShipKind, -1 },
    { "MODELS/COMMON/SPACECRAFT/SAILSHIP/SAILSHIP_PROC.SCENE.MBIN", "gar.solar", ShipKind, 5 },
    { "MODELS/COMMON/SPACECRAFT/SENTINELSHIP/SENTINELSHIP_PROC.SCENE.MBIN", "gar.sentinel", ShipKind, -1 },
    { "MODELS/COMMON/SPACECRAFT/INDUSTRIAL/CAPITALFREIGHTER_PROC.SCENE.MBIN", "gar.fr.capital", FreighterKind, -1 },
    { "MODELS/COMMON/SPACECRAFT/INDUSTRIAL/FREIGHTER_PROC.SCENE.MBIN", "gar.fr.standard", FreighterKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/MULTITOOL.SCENE.MBIN", "gar.mt.standard", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/ATLASMULTITOOL.SCENE.MBIN", "gar.mt.atlas", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/RETROMULTITOOL.SCENE.MBIN", "gar.mt.retro", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/ROYALMULTITOOL.SCENE.MBIN", "gar.mt.royal", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/SENTINELMULTITOOL.SCENE.MBIN", "gar.mt.sentinel", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/SENTINELMULTITOOLB.SCENE.MBIN", "gar.mt.sentinelb", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/STAFFMULTITOOL.SCENE.MBIN", "gar.mt.staff", ToolKind, 0 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/STAFFMULTITOOLATLAS.SCENE.MBIN", "gar.mt.staffatlas", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/STAFFMULTITOOLBONE.SCENE.MBIN", "gar.mt.staffbone", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/STAFFMULTITOOLRUIN.SCENE.MBIN", "gar.mt.staffruin", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/SWARMMULTITOOL.SCENE.MBIN", "gar.mt.swarm", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/SWITCHMULTITOOL.SCENE.MBIN", "gar.mt.switch", ToolKind, -1 },
    { "MODELS/COMMON/WEAPONS/MULTITOOL/RODMULTITOOL.SCENE.MBIN", "gar.mt.rod", ToolKind, -1 },
};

const char* kVehicleKeys[7] = {
    "gar.veh.roamer", "gar.veh.nomad", "gar.veh.colossus", "gar.veh.pilgrim", "gar.veh.hover", "gar.veh.nautilon", "gar.veh.minotaur"
};
// Exocraft scenes, in the same order (scene paths from the exe's strings)
const char* kVehicleFiles[7] = {
    "MODELS/COMMON/VEHICLES/BUGGY/BUGGY.SCENE.MBIN", "MODELS/COMMON/VEHICLES/BIKE/BIKE.SCENE.MBIN",
    "MODELS/COMMON/VEHICLES/ROVER/ROVER.SCENE.MBIN", "MODELS/COMMON/VEHICLES/WHEELEDBIKE/WHEELEDBIKE.SCENE.MBIN",
    "MODELS/COMMON/VEHICLES/HOVER/HOVERCRAFT.SCENE.MBIN", "MODELS/COMMON/VEHICLES/SUBMARINE/SUBMARINE.SCENE.MBIN",
    "MODELS/COMMON/VEHICLES/MECH_SUIT/MECH_SUIT.SCENE.MBIN"
};
// Player resource default in build 180836: string RVA 0x34E5020, assigned at 0x28A9DD9.
constexpr const char* kPlayerFile = "MODELS/COMMON/PLAYER/PLAYERCHARACTER/PLAYERCHARACTER.SCENE.MBIN";

struct Fns
{
    void (*setString)(void*, const char*) = nullptr;
    void (*toolGet)(void*, int, void*) = nullptr;
    void (*toolSet)(void*, int, const void*) = nullptr;
    void (*toolSwitch)(void*, int, bool) = nullptr;
    void (*custApply)(void*, const void*, int, bool, bool, const void*) = nullptr;
    void (*custReset)(void*) = nullptr;
    void (*custExport)(void*, void*) = nullptr;
    const float* (*getPalette)(void*, const void*, int) = nullptr;
    void (*genBaseStats)(void*, int, const void*, int, int, int, int, bool) = nullptr;
    void (*genSpecialSlots)(void*, int, const void*) = nullptr;
    void (*invNotify)(void*, const void*) = nullptr;
    int (*weaponGetClass)(uint32_t) = nullptr;
    void (*genDesc)(void*, const char*, const void*, const void*, const void*, void*) = nullptr; // Descriptor.Generate
    uintptr_t (*modelList)(const char*) = nullptr; // GetModelDescriptorList(path)
    void (*invCopy)(void* dst, const void* src) = nullptr;
    void (*setPrimary)(void* ps, int32_t index) = nullptr;
    uintptr_t (*getTech)(void* rm, const void* id, bool) = nullptr; // cGcRealityManager::GetTechnology, rolled upgrades too

    int64_t primary = -1, refresh = -1, active = -1, cls = 0x100, frRefresh = 0x4B0, baseStats = 0xD0;
    int64_t slotBase = 0x1CA10, slotStride = 0x3AE0, slotEdited = 0x71, slotEditCol = 0x90, slotFinalCol = 0x1D70, slotDescs = 0x3A70;
    int64_t descGroups = 0xCA500, playerModel = 0x72C838, playerRefresh = 0x15C, vehicleRefresh = 0x72F0F0;

    uint32_t resSize = 0x48, resFile = 0x10, resSeed = 0x30;
    uint32_t toolSize = 0x290, toolRes = 0x1E8, toolSeed = 0x248, toolName = 0x260, toolCust = 0x190, saveCustData = 0x10, toolLegacy = 0x281;
    // cGcCustomisationDescriptorGroups: sets, then each set's groups.
    uint32_t dgSets = 0x30, setSize = 0x38, setGroups = 0, setId = 0x10, setRequires = 0x20, setExclusive = 0x30;
    uint32_t groupSize = 0x108, groupTitle = 0x20, groupId = 0x50, groupHidden = 0x100;
    bool custOk = true; // cGcCharacterCustomisationData still has the layout below
} F;

// cGcTechnology and cGcInventoryElement fields, refreshed from the reflection data.
struct
{
    uint32_t size = 0x2E0, id = 0x108, cat = 0x194, stat = 0x18C, charge = 0x198;
    uint32_t core = 0x2C7, upg = 0x2CF, proc = 0x2CC, rep = 0x2CD, broken = 0x2C4, tmpl = 0x2C9;
    uint32_t elSize = 0x30, elId = 0x00, elAmount = 0x18, elMax = 0x20, elType = 0x24;
} TF;

struct StatEntry
{
    std::string id;
    float value = 0.f;
};

// cGcCharacterCustomisationData, copied into our own arrays.
struct Id16 { char s[16]{}; };
struct ColourEntry { float rgba[4]{}; int32_t alt = 0, index = -1, palette = 0, pad = 0; };
struct TexOption { char name[32]{}; char group[16]{}; };
struct BoneScale { char name[16]{}; float scale = 1.f, pad = 0.f; };
static_assert(sizeof(ColourEntry) == 0x20 && sizeof(TexOption) == 0x30 && sizeof(BoneScale) == 0x18);

struct CustData
{
    std::vector<BoneScale> bones;
    std::vector<ColourEntry> colours;
    std::vector<Id16> descs;
    std::vector<TexOption> tex;
    Id16 palette;
    float scale = 1.f;
    uint8_t seed[16]{}; // the slot's cTkSeed; the mesh job builds from it
    uint8_t legacy = 0; // old seed palettes; while set the item ignores edited colours
};

uint64_t PreviewVersion(const CustData& c)
{
    uint64_t hash = 14695981039346656037ull;
    auto bytes = [&](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    };
    auto value = [&](const auto& v) { bytes(&v, sizeof v); };
    // Include vector lengths and actual fields, excluding game struct padding.
    bytes(c.seed, 9); // cTkSeed is a uint64 plus its valid flag.
    value(c.bones.size());
    for (const auto& b : c.bones) { value(b.name); value(b.scale); }
    value(c.colours.size());
    for (const auto& colour : c.colours) {
        value(colour.rgba); value(colour.alt); value(colour.index); value(colour.palette);
    }
    value(c.descs.size());
    for (const auto& id : c.descs) value(id.s);
    value(c.tex.size());
    for (const auto& texture : c.tex) { value(texture.name); value(texture.group); }
    value(c.palette.s);
    value(c.scale);
    value(c.legacy);
    return hash;
}

// The customisation of the item in the inspector, read on the game thread.
struct Detail
{
    std::string key;
    CustData cust;
    std::vector<std::array<float, 4>> swatches; // the slot's palette
};

struct DescGroup { std::string id, label; };
struct DescSet { std::string id, label, needs; std::vector<DescGroup> groups; };

// One owned ship, Multi-Tool, freighter, suit or exocraft, read on the game thread.
struct Owned
{
    Kind kind = ShipKind;
    int index = 0;
    std::string name, file;
    uint64_t seed = 0;
    bool useSeed = false, current = false;
    int cls = -1, rules = 0;
    int slots[3]{}; // general, tech, cargo
    std::vector<StatEntry> stats;
    std::string Key() const
    {
        const char* p = kind == ShipKind ? "S:" : kind == ToolKind ? "T:" : kind == FreighterKind ? "F:" : kind == SuitKind ? "U:" : "V:";
        return std::string(p) + std::to_string(index);
    }
};

// Everything needed to undo edits to one item.
struct Snap
{
    std::string file, name;
    uint64_t seed = 0;
    uint8_t use = 0;
    int32_t cls[3]{};
    std::vector<uint8_t> tool; // cGcMultitoolData from GetData
    std::vector<StatEntry> stats;
    std::optional<CustData> cust;
};

std::mutex g_mx;
std::vector<Owned> g_owned;
std::map<std::string, Snap> g_snaps; // game thread
std::set<std::string> g_snapKeys;    // copy for the UI
std::optional<Detail> g_detail;   // under g_mx
std::atomic<bool> g_ourApply{ false }, g_keepExact{ false }; // skip palette snapping in Customise.Apply
struct CacheGroup { uint32_t base, size, flag, stride; };
std::vector<CacheGroup> g_statCache; // stat cache blocks in cGcPlayerState, read from its reset
std::vector<DescSet> g_sets;       // under g_mx, filled once
std::atomic<bool> g_setsLoaded{ false };

struct UiState
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    std::string sig, editKey;
    char seed[40] = {}, name[kNameLen] = {};
    int typeIdx = -1, clsIdx = -1;
    int subTab = 0; // 0 = General, 1 = Parts & Paint, 2 = Stats
    int colGroup = 0;
    bool moreGroups = false;
    bool previewEnabled = true;
    float prevZoom = 1.f;
    float prevYaw = 0.f, prevPitch = 0.f; // degrees, our own render only
    std::string prevMissing; // item already logged as having no model file
    float pick[3] = { 1.f, 1.f, 1.f };
    float glow = 1.f;
    std::string pickFor; // group the picker was filled from
    std::vector<StatEntry> stats; // pending stat edits
    // Preview seed: a seed shown on an owned item without applying it
    std::string seedPrevKey;
    uint64_t seedPrev = 0;
    // Add to Garage: a draft that only exists in the preview until it is added
    struct
    {
        Kind kind = ShipKind;
        std::string file;
        uint64_t seed = 0;
        int cls = 3;
        char seedText[40] = {}, name[kNameLen] = {};
        int addedSlot = -1;
    } draft;
} g;

constexpr const char* kAddKey = "add:new";

std::string ReadVarString(uintptr_t at)
{
    uintptr_t p = 0;
    std::string s;
    if (!Rd(at, p) || !p) return s;
    for (char c; s.size() < 260 && Rd(p + s.size(), c) && c;) s += c;
    return s;
}

std::string ReadFixed(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return s;
}

void WriteFixed(uintptr_t at, const std::string& s, size_t max)
{
    for (size_t i = 0; i < max; ++i) Wr<char>(at + i, i < s.size() && i + 1 < max ? s[i] : 0);
}

uint64_t RandomSeed()
{
    static std::mt19937_64 rng{ std::random_device{}() };
    return rng();
}

int16_t Slots(uintptr_t store)
{
    int16_t n = 0;
    Rd(store + 0x84, n);
    return n;
}

int CustSlotFor(Kind kind, int index, const std::string& file = {})
{
    if (kind == ShipKind) return index < 6 ? 3 + index : 11 + index;
    if (kind == ToolKind) return 2;
    if (kind == FreighterKind) return file.find("PIRATE") != std::string::npos ? 23 : 15;
    if (kind == SuitKind) return 0;
    if (kind == VehicleKind) return index == 0 ? 1 : 8 + index;
    return -1;
}

uintptr_t CustSlotAddr(uintptr_t ps, int slot)
{
    if (!ps || slot < 0 || slot > 25) return 0;
    return ps + (uintptr_t)F.slotBase + (uintptr_t)slot * (uintptr_t)F.slotStride;
}

int ModTypeForFile(const std::string& file)
{
    for (auto& m : kModels)
        if (_stricmp(m.path, file.c_str()) == 0) return m.modType;
    return -1;
}

// cGcSpaceshipClasses, the type GenerateBaseStats rolls for: Dropship 1, Fighter 2, Scientific 3,
// Shuttle 4, Royal 6, Alien 7, Sail 8, Robot 9.
constexpr int kFighterType = 2, kAlienType = 7, kRobotType = 9;

int ShipClassEnumFromFile(const std::string& file)
{
    if (file.find("DROPSHIP") != std::string::npos) return 1;
    if (file.find("FIGHTER") != std::string::npos) return kFighterType;
    if (file.find("SCIENTIFIC") != std::string::npos) return 3;
    if (file.find("SHUTTLE") != std::string::npos) return 4;
    if (file.find("BIOPARTS") != std::string::npos || file.find("BIOSHIP") != std::string::npos) return kAlienType;
    if (file.find("S-CLASS") != std::string::npos) return 6;
    if (file.find("SAILSHIP") != std::string::npos) return 8;
    if (file.find("SENTINEL") != std::string::npos) return kRobotType;
    return kFighterType;
}

// ---- tech rules ----
// On load the game picks a ship's allowed tech categories from the ALIEN_SHIP / ROBOT_SHIP base stat
// of its main inventory and drops any tech outside them. The model plays no part.
enum Rules { RulesNormal, RulesLiving, RulesSentinel };

// cGcTechnologyCategory: Ship 0, AlienShip 13, AllShips 14, RobotShip 15, AllShipsExceptAlien 16
bool RulesAllow(int rules, int cat)
{
    if (rules == RulesSentinel) return cat == 14 || cat == 15 || cat == 16;
    if (rules == RulesLiving) return cat == 13 || cat == 14;
    return cat == 0 || cat == 14 || cat == 16;
}

int RulesOwnCategory(int rules) { return rules == RulesSentinel ? 15 : rules == RulesLiving ? 13 : 0; }

int RulesFromStats(const std::vector<StatEntry>& stats)
{
    for (auto& s : stats) {
        if (s.value > 0 && s.id == "ALIEN_SHIP") return RulesLiving;
        if (s.value > 0 && s.id == "ROBOT_SHIP") return RulesSentinel;
    }
    return RulesNormal;
}

int RulesForFile(const std::string& file)
{
    int t = ShipClassEnumFromFile(file);
    return t == kRobotType ? RulesSentinel : t == kAlienType ? RulesLiving : RulesNormal;
}

// The ship type to roll base stats for, so the stats carry the marker for these rules.
int TypeForRules(int rules, const std::string& file)
{
    if (rules == RulesSentinel) return kRobotType;
    if (rules == RulesLiving) return kAlienType;
    int t = ShipClassEnumFromFile(file);
    return t == kRobotType || t == kAlienType ? kFighterType : t;
}

int WeaponClassEnumFromFile(const std::string& file, uintptr_t slotAddr)
{
    if (F.weaponGetClass && slotAddr) {
        uint32_t h = 0;
        if (Rd(slotAddr + 0x280, h) && h) {
            int c = F.weaponGetClass(h);
            if (c >= 0 && c <= 9) return c;
        }
    }
    if (file.find("STAFF") != std::string::npos) return 9;
    if (file.find("ATLAS") != std::string::npos) return 6;
    if (file.find("SENTINEL") != std::string::npos) return 5;
    if (file.find("ROYAL") != std::string::npos) return 4;
    if (file.find("SWARM") != std::string::npos) return 3;
    return 1;
}

std::string CleanIdLabel(const std::string& id)
{
    size_t u = id.find('_');
    std::string s = u != std::string::npos ? id.substr(u + 1) : id;
    for (char& c : s) if (c == '_') c = ' ';
    return s.empty() ? id : s;
}

// ---- game thread ----

int32_t Primary()
{
    uintptr_t d = game::Data();
    int32_t p = -1;
    if (!d || F.primary < 0 || !Rd(d + F.primary, p) || p < 0 || p > 11) return -1;
    return p;
}

uintptr_t ShipRes(uintptr_t ps, int i) { return ps + kShipResources + (uintptr_t)i * F.resSize; }

uintptr_t ShipStore(uintptr_t ps, int i, int which)
{
    uint32_t base = which == 0 ? kShipInv : which == 1 ? kShipTech : kShipCargo;
    return ps + base + (uintptr_t)i * kStride;
}

uintptr_t ToolOwn() { uintptr_t d = game::Data(); return d ? d + kToolOwnership : 0; }
uintptr_t ToolSlot(int i) { uintptr_t o = ToolOwn(); return o ? o + kToolSlots + (uintptr_t)i * kToolStride : 0; }
uintptr_t FreighterOwn() { uintptr_t d = game::Data(); return d ? d + kFreighterOwn : 0; }

int ToolActive()
{
    int32_t a = -1;
    uintptr_t own = ToolOwn();
    if (!own || F.active < 0 || !Rd(own + F.active, a) || a < 0 || a > 5) return -1;
    return a;
}

std::vector<uint8_t> ToolData(int i)
{
    std::vector<uint8_t> buf(F.toolSize, 0);
    if (uintptr_t own = ToolOwn(); own && F.toolGet) F.toolGet((void*)own, i, buf.data());
    return buf;
}

std::vector<StatEntry> ReadStoreStats(uintptr_t store)
{
    std::vector<StatEntry> out;
    if (!store || F.baseStats < 0) return out;
    uint32_t cnt = 0;
    uintptr_t data = 0;
    if (!Rd(store + F.baseStats + 4, cnt) || !Rd(store + F.baseStats + 8, data) || !data || cnt == 0 || cnt > 16)
        return out;
    for (uint32_t i = 0; i < cnt; ++i) {
        uintptr_t e = data + (uintptr_t)i * 0x18;
        std::string id = ReadFixed(e, 16);
        float val = 0.f;
        if (!id.empty() && Rd(e + 0x10, val)) out.push_back({ std::move(id), val });
    }
    return out;
}

int WriteStoreStats(uintptr_t store, const std::vector<StatEntry>& stats)
{
    int n = 0;
    if (!store || F.baseStats < 0) return n;
    uint32_t cnt = 0;
    uintptr_t data = 0;
    if (!Rd(store + F.baseStats + 4, cnt) || !Rd(store + F.baseStats + 8, data) || !data || cnt == 0 || cnt > 16)
        return n;
    for (uint32_t i = 0; i < cnt; ++i) {
        uintptr_t e = data + (uintptr_t)i * 0x18;
        std::string id = ReadFixed(e, 16);
        for (auto& s : stats) {
            if (s.id == id) {
                n += Wr<float>(e + 0x10, s.value);
                break;
            }
        }
    }
    return n;
}

void NotifyStore(uintptr_t ps, int32_t choice, int32_t index)
{
    if (!ps || !F.invNotify) return;
    struct { uint64_t zero; int32_t choice; int32_t index; } notif{ 0, choice, index };
    F.invNotify((void*)(ps + 0x900), &notif);
}

bool RdBytes(uintptr_t at, void* out, size_t n)
{
    __try { memcpy(out, reinterpret_cast<const void*>(at), n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// cTkDynamicArray { T* data; int32 count; bool notOwned; }
template <class T> std::vector<T> ReadArray(uintptr_t at)
{
    std::vector<T> out;
    uintptr_t p = 0;
    int32_t n = 0;
    if (!Rd(at, p) || !Rd(at + 8, n) || !p || n <= 0 || n > 4096) return out;
    out.resize(n);
    if (!RdBytes(p, out.data(), n * sizeof(T))) out.clear();
    return out;
}

template <class T> void PutArray(uint8_t* at, std::vector<T>& v)
{
    uintptr_t p = v.empty() ? 0 : (uintptr_t)v.data();
    int32_t n = (int32_t)v.size();
    memcpy(at, &p, 8);
    memcpy(at + 8, &n, 4);
    at[12] = 1; // not owned by the game
}

CustData FromRaw(uintptr_t d)
{
    CustData c;
    c.bones = ReadArray<BoneScale>(d);
    c.colours = ReadArray<ColourEntry>(d + 0x10);
    c.descs = ReadArray<Id16>(d + 0x20);
    RdBytes(d + 0x30, &c.palette, 16);
    c.tex = ReadArray<TexOption>(d + 0x40);
    Rd(d + 0x50, c.scale);
    return c;
}

void ToRaw(CustData& c, uint8_t* d)
{
    PutArray(d, c.bones);
    PutArray(d + 0x10, c.colours);
    PutArray(d + 0x20, c.descs);
    memcpy(d + 0x30, &c.palette, 16);
    PutArray(d + 0x40, c.tex);
    memcpy(d + 0x50, &c.scale, 4);
}

std::string FileOf(uintptr_t ps, Kind kind, int index)
{
    if (kind == SuitKind) return kPlayerFile;
    if (kind == ShipKind) return ReadVarString(ShipRes(ps, index) + F.resFile);
    if (kind == FreighterKind) if (uintptr_t fo = FreighterOwn()) return ReadVarString(fo + kFreighterRes + F.resFile);
    return {};
}

uintptr_t ToolCust(std::vector<uint8_t>& buf) { return (uintptr_t)buf.data() + F.toolCust + F.saveCustData; }

bool RefreshShip();
bool RefreshFreighter();

// Game thread. A ship still building its last mesh can't take a refresh, so try again each frame for a while.
void RetryRefreshShip(int left = 180)
{
    if (RefreshShip()) return;
    if (left > 0) game::Post([left] { RetryRefreshShip(left - 1); });
    else log::Warn("Garage", "Ship kept rebuilding, change not shown yet");
}
void RefreshPlayerModel();
void RefreshVehicleModel();
void ToolApply(int idx, const std::vector<uint8_t>& data);
int32_t Primary();

// Game thread. The Multi-Tool keeps its look in its own data, everything else in a customisation slot.
std::optional<CustData> ReadCust(uintptr_t ps, Kind kind, int index)
{
    if (!F.custOk) return std::nullopt;
    if (kind == ToolKind) {
        auto buf = ToolData(index);
        if (buf.size() < F.toolCust + F.saveCustData + 0x58) return std::nullopt;
        CustData c = FromRaw(ToolCust(buf));
        if (buf.size() > F.toolLegacy) c.legacy = buf[F.toolLegacy];
        return c;
    }
    uintptr_t at = CustSlotAddr(ps, CustSlotFor(kind, index, FileOf(ps, kind, index)));
    if (!at || !F.custExport) return std::nullopt;
    alignas(16) static uint8_t buf[0x58] = {}; // Export frees what it wrote last time
    F.custExport((void*)at, buf);
    CustData c = FromRaw((uintptr_t)buf);
    RdBytes(at, c.seed, 16);
    Rd(at + 0x70, c.legacy);
    return c;
}

// Game thread. Applies the whole look the way the game's appearance modifier does.
void WriteCust(uintptr_t ps, Kind kind, int index, CustData c, bool refresh = true)
{
    alignas(16) uint8_t raw[0x58] = {};
    ToRaw(c, raw);
    if (kind == ToolKind) {
        auto buf = ToolData(index);
        if (buf.size() < F.toolCust + F.saveCustData + 0x58) return;
        memset(buf.data() + F.toolCust, 0, 16); // SelectedPreset
        memcpy((void*)ToolCust(buf), raw, sizeof raw);
        if (buf.size() > F.toolLegacy) buf[F.toolLegacy] = c.legacy;
        g_ourApply = true;
        ToolApply(index, buf);
        g_ourApply = false;
        return;
    }
    int slot = CustSlotFor(kind, index, FileOf(ps, kind, index));
    uintptr_t at = CustSlotAddr(ps, slot);
    if (!at || !F.custApply) return;
    if (F.custReset) F.custReset((void*)at); // also clears the preset, so a reload keeps this look
    g_ourApply = true; // colours stay exactly as picked
    // Apply stores the legacy flag in the slot, and saving copies it from there
    F.custApply((void*)at, raw, slot, false, c.legacy != 0, c.seed);
    g_ourApply = false;
    if (!refresh) return;
    if (kind == ShipKind) { if (index == Primary()) RetryRefreshShip(); }
    else if (kind == FreighterKind) RefreshFreighter();
    else if (kind == SuitKind) RefreshPlayerModel();
    else if (kind == VehicleKind) RefreshVehicleModel();
}

// The mesh job seeds from the customisation slot, so a new seed goes there too.
// Ships drop their parts, or the parts would hide the seeded look.
void SeedSlot(uintptr_t ps, Kind kind, int index, uint64_t seed)
{
    auto c = ReadCust(ps, kind, index);
    if (!c) return;
    memset(c->seed, 0, sizeof c->seed);
    memcpy(c->seed, &seed, 8);
    c->seed[8] = 1;
    if (kind == ShipKind) c->descs.clear();
    WriteCust(ps, kind, index, std::move(*c), false);
}

std::string Pretty(std::string id)
{
    bool up = true;
    for (char& ch : id) {
        ch = ch == '_' ? ' ' : (char)tolower((unsigned char)ch);
        if (up && isalpha((unsigned char)ch)) ch = (char)toupper((unsigned char)ch);
        up = ch == ' ';
    }
    return id;
}

// Game thread, once: the descriptor group sets that every customisable thing picks its parts from.
void LoadSets(uintptr_t ps)
{
    uintptr_t tbl = 0, sets = 0;
    int32_t n = 0;
    if (g_setsLoaded || !ps || F.descGroups < 0 || !Rd(ps + F.descGroups, tbl) || !tbl) return;
    if (!Rd(tbl + F.dgSets, sets) || !Rd(tbl + F.dgSets + 8, n) || !sets || n <= 0 || n > 512) return;
    std::vector<DescSet> out;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t st = sets + (uintptr_t)i * F.setSize;
        uint8_t excl = 0;
        Rd(st + F.setExclusive, excl);
        if (!excl) continue; // decal sets allow several at once
        DescSet ds;
        ds.id = ReadFixed(st + F.setId, 16);
        ds.needs = ReadFixed(st + F.setRequires, 16);
        std::string base = ds.id.starts_with("CHAR_") ? ds.id.substr(5) : ds.id;
        if (base.ends_with("_HEAD")) base = "HEAD";
        ds.label = Pretty(base);
        uintptr_t gp = 0;
        int32_t gn = 0;
        if (!Rd(st + F.setGroups, gp) || !Rd(st + F.setGroups + 8, gn) || !gp || gn <= 0 || gn > 512) continue;
        for (int32_t k = 0; k < gn; ++k) {
            uintptr_t gr = gp + (uintptr_t)k * F.groupSize;
            DescGroup dg;
            dg.id = ReadFixed(gr + F.groupId, 16);
            if (dg.id.empty()) continue;
            std::string title = ReadFixed(gr + F.groupTitle, 32);
            if (!title.empty()) dg.label = game::catalog::Translate(title);
            if (dg.label.empty() || dg.label == title) {
                std::string rest = dg.id;
                for (const std::string& pre : { ds.id + "_", base + "_" })
                    if (rest.starts_with(pre) && rest.size() > pre.size()) rest = rest.substr(pre.size());
                dg.label = Pretty(rest);
                if (!dg.label.empty() && isdigit((unsigned char)dg.label[0])) dg.label = L::Tf("gar.style", dg.label);
            }
            ds.groups.push_back(std::move(dg));
        }
        if (ds.groups.size() > 1) out.push_back(std::move(ds));
    }
    log::Info("Garage", "{} part sets loaded", out.size());
    std::lock_guard lk(g_mx);
    g_sets = std::move(out);
    g_setsLoaded = true;
}

// Game thread: what the inspector shows for one item.
void LoadDetail(Kind kind, int index)
{
    uintptr_t ps = game::PlayerState();
    if (!ps) return;
    LoadSets(ps);
    auto c = ReadCust(ps, kind, index);
    if (!c) return;
    Detail d;
    d.key = Owned{ kind, index }.Key();
    int slot = kind == ToolKind ? 2 : CustSlotFor(kind, index, FileOf(ps, kind, index));
    if (F.getPalette) {
        const float* pal = F.getPalette((void*)ps, &c->palette, slot);
        static const int kCounts[] = { 0, 1, 4, 8, 16, 64 }; // cTkPaletteTexture NumColours
        int32_t num = -1;
        if (pal && Rd(uintptr_t(pal) + 0x400, num) && num >= 0 && num < 6) {
            for (int k = 0; k < kCounts[num]; ++k) {
                std::array<float, 4> col{};
                if (RdBytes(uintptr_t(pal + k * 4), col.data(), 16)) d.swatches.push_back(col);
            }
        }
    }
    d.cust = std::move(*c);
    std::lock_guard lk(g_mx);
    g_detail = std::move(d);
}

bool RefreshShip()
{
    uintptr_t d = game::Data();
    if (!d || F.refresh < 0) return false;
    uintptr_t own = d + kShipOwnership;
    int32_t state = -1;
    if (!Rd(own + F.refresh + 4, state) || state != 0) return false; // busy building a mesh
    return Wr<uint8_t>(own + F.refresh, 1);
}

bool RefreshFreighter()
{
    uintptr_t own = FreighterOwn();
    int32_t state = -1;
    if (!own || !Rd(own + F.frRefresh + 4, state) || state != 0) return false;
    return Wr<uint8_t>(own + F.frRefresh, 1);
}

void RefreshPlayerModel()
{
    uintptr_t d = game::Data(), pm = 0;
    if (!d || F.playerModel < 0 || !Rd(d + F.playerModel, pm) || !pm) return;
    uint32_t s1 = 0, s2 = 0;
    if (Rd(pm + 0x160, s1) && Rd(pm + 0x168, s2) && s1 == 0 && s2 == 0) {
        Wr<uint64_t>(pm + 0x20, 0);
        Wr<uint8_t>(pm + F.playerRefresh, 1);
    }
}

void RefreshVehicleModel()
{
    uintptr_t d = game::Data();
    if (d && F.vehicleRefresh > 0) Wr<uint8_t>(d + F.vehicleRefresh, 1);
}

void ReadOwned()
{
    uintptr_t ps = game::PlayerState();
    if (!ps) return;

    std::vector<Owned> out;
    int32_t primary = Primary();
    for (int i = 0; i < 12; ++i) {
        Owned o;
        o.kind = ShipKind;
        o.index = i;
        uintptr_t res = ShipRes(ps, i);
        o.file = ReadVarString(res + F.resFile);
        if (o.file.empty()) continue;
        Rd(res + F.resSeed, o.seed);
        uint8_t use = 0;
        Rd(res + F.resSeed + 8, use);
        o.useSeed = use != 0;
        o.name = ReadFixed(ps + kShipNames + i * kNameLen, kNameLen);
        Rd(ShipStore(ps, i, 0) + F.cls, o.cls);
        for (int k = 0; k < 3; ++k) o.slots[k] = Slots(ShipStore(ps, i, k));
        o.stats = ReadStoreStats(ShipStore(ps, i, 0));
        o.rules = RulesFromStats(o.stats);
        o.current = i == primary;
        out.push_back(std::move(o));
    }
    int active = ToolActive();
    for (int i = 0; i < 6; ++i) {
        uintptr_t slot = ToolSlot(i);
        int32_t elems = 0;
        if (!slot || !Rd(slot + 0x8C, elems) || elems <= 0) continue;
        Owned o;
        o.kind = ToolKind;
        o.index = i;
        auto buf = ToolData(i);
        o.file = ReadVarString((uintptr_t)buf.data() + F.toolRes + F.resFile);
        memcpy(&o.seed, buf.data() + F.toolSeed, 8);
        o.useSeed = buf[F.toolSeed + 8] != 0;
        o.name = ReadFixed(slot + kToolName, kNameLen);
        Rd(slot + F.cls, o.cls);
        o.slots[0] = Slots(slot);
        o.stats = ReadStoreStats(i == active ? ps + kWeaponInv : slot);
        o.current = i == active;
        out.push_back(std::move(o));
    }
    if (uintptr_t fo = FreighterOwn()) {
        Owned o;
        o.kind = FreighterKind;
        o.file = ReadVarString(fo + kFreighterRes + F.resFile);
        if (!o.file.empty()) {
            Rd(fo + kFreighterRes + F.resSeed, o.seed);
            uint8_t use = 0;
            Rd(fo + kFreighterRes + F.resSeed + 8, use);
            o.useSeed = use != 0;
            o.name = ReadFixed(fo + kFreighterName, kNameLen);
            Rd(ps + kFreighterStores + F.cls, o.cls);
            o.slots[0] = Slots(ps + kFreighterStores);
            o.slots[1] = Slots(ps + kFreighterStores + kStride);
            o.slots[2] = Slots(ps + kFreighterStores + 2 * kStride);
            o.stats = ReadStoreStats(ps + kFreighterStores);
            o.current = true;
            out.push_back(std::move(o));
        }
    }
    {
        Owned suit;
        suit.kind = SuitKind;
        suit.index = 0;
        suit.file = kPlayerFile;
        suit.name = L::T("gar.chip.suit");
        suit.current = true;
        out.push_back(std::move(suit));
    }
    int32_t primVeh = 0;
    Rd(ps + kPrimaryVehicle, primVeh);
    for (int v = 0; v < 7; ++v) {
        Owned veh;
        veh.kind = VehicleKind;
        veh.index = v;
        veh.name = L::T(kVehicleKeys[v]);
        veh.current = (v == primVeh);
        out.push_back(std::move(veh));
    }
    std::set<std::string> keys;
    for (auto& [k, v] : g_snaps) keys.insert(k);
    std::lock_guard lk(g_mx);
    g_owned = std::move(out);
    g_snapKeys = std::move(keys);
}

// An edit to one item. Empty or missing fields are left alone.
struct Edit
{
    Kind kind = ShipKind;
    int index = 0;
    std::string file, name;
    std::optional<uint64_t> seed;
    int cls = -1, rules = -1; // rules: ships only, -1 follows the model when it changes
    bool rename = false;
};

void TakeSnap(const Edit& e, uintptr_t ps)
{
    std::string key = Owned{ e.kind, e.index }.Key();
    if (g_snaps.contains(key)) return;
    Snap s;
    if (e.kind == ShipKind) {
        uintptr_t res = ShipRes(ps, e.index);
        s.file = ReadVarString(res + F.resFile);
        Rd(res + F.resSeed, s.seed);
        Rd(res + F.resSeed + 8, s.use);
        for (int k = 0; k < 3; ++k) Rd(ShipStore(ps, e.index, k) + F.cls, s.cls[k]);
        s.name = ReadFixed(ps + kShipNames + e.index * kNameLen, kNameLen);
        s.stats = ReadStoreStats(ShipStore(ps, e.index, 0));
    }
    else if (e.kind == ToolKind) {
        s.tool = ToolData(e.index);
        Rd(ToolSlot(e.index) + F.cls, s.cls[0]);
        s.stats = ReadStoreStats(e.index == ToolActive() ? ps + kWeaponInv : ToolSlot(e.index));
    }
    else if (e.kind == FreighterKind) {
        if (uintptr_t fo = FreighterOwn()) {
            s.file = ReadVarString(fo + kFreighterRes + F.resFile);
            Rd(fo + kFreighterRes + F.resSeed, s.seed);
            Rd(fo + kFreighterRes + F.resSeed + 8, s.use);
            for (int k = 0; k < 3; ++k) Rd(ps + kFreighterStores + k * kStride + F.cls, s.cls[k]);
            s.name = ReadFixed(fo + kFreighterName, kNameLen);
            s.stats = ReadStoreStats(ps + kFreighterStores);
        }
    }
    if (e.kind != ToolKind) s.cust = ReadCust(ps, e.kind, e.index);
    g_snaps[key] = std::move(s);
}

// Rebuilds the held model even when only the seed changed.
void ToolApply(int idx, const std::vector<uint8_t>& data)
{
    uintptr_t own = ToolOwn();
    if (!own || !F.toolSet) return;
    F.toolSet((void*)own, idx, data.data());
    if (idx != ToolActive() || !F.toolSwitch) return;
    if (uintptr_t d = game::Data()) Wr<uint32_t>(d + kPlayerWeapon + kWeaponRes, 0);
    F.toolSwitch((void*)own, idx, true);
}

void MarkStatsDirty(uintptr_t ps);

struct TechRec
{
    std::string id;
    int cat = -1, stat = -1, charge = 0;
    bool core = false, upg = false, usable = false;
};

TechRec ReadTech(uintptr_t r)
{
    TechRec t;
    uint8_t core = 0, upg = 0, proc = 0, rep = 0, broken = 0, tmpl = 0;
    t.id = ReadFixed(r + TF.id, 16);
    Rd(r + TF.cat, t.cat);
    Rd(r + TF.stat, t.stat);
    Rd(r + TF.charge, t.charge);
    Rd(r + TF.core, core); Rd(r + TF.upg, upg); Rd(r + TF.proc, proc); Rd(r + TF.rep, rep); Rd(r + TF.broken, broken); Rd(r + TF.tmpl, tmpl);
    t.core = core != 0;
    t.upg = upg != 0;
    t.usable = !t.id.empty() && !proc && !rep && !broken && !tmpl;
    return t;
}

// The base technology table (RealityManager+0x10, as in Unlocks).
std::vector<TechRec> BaseTechs()
{
    std::vector<TechRec> out;
    uintptr_t rm = game::Reality(), obj = 0, data = 0;
    int32_t n = 0;
    if (!rm || !Rd(rm + 0x10, obj) || !obj || !Rd(obj, data) || !Rd(obj + 8, n) || !data || n <= 0 || n > 20000) return out;
    for (int32_t i = 0; i < n; ++i) out.push_back(ReadTech(data + (uintptr_t)i * TF.size));
    return out;
}

uintptr_t CallGetTech(uintptr_t rm, const Id16* key)
{
    __try { return F.getTech((void*)rm, key, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

std::optional<TechRec> LookupTech(const std::string& id, const std::vector<TechRec>& base)
{
    if (uintptr_t rm = game::Reality(); rm && F.getTech) {
        Id16 key{};
        memcpy(key.s, id.data(), std::min<size_t>(id.size(), 15));
        if (uintptr_t r = CallGetTech(rm, &key)) return ReadTech(r);
    }
    for (auto& t : base) if (t.id == id) return t;
    return std::nullopt;
}

// The same job under other rules: same base stat and kind, preferring the rules' own version.
const TechRec* Counterpart(const TechRec& src, int rules, const std::vector<TechRec>& base, const std::set<std::string>& have)
{
    if (src.upg) return nullptr; // upgrades of the same stat are different items, so they never swap
    const TechRec* best = nullptr;
    int bestScore = -1;
    for (auto& t : base) {
        if (!t.usable || t.upg || t.stat != src.stat || !RulesAllow(rules, t.cat) || have.count(t.id)) continue;
        int score = (t.core == src.core) * 4 + (t.cat == RulesOwnCategory(rules)) * 2 + !t.id.ends_with("_SPEC");
        if (score > bestScore) { best = &t; bestScore = score; }
    }
    return best;
}

std::string TechName(const std::string& id)
{
    const game::Item* it = game::catalog::Find(id);
    return it && !it->name.empty() ? it->name : id;
}

// Game thread. Sets the rules marker in a ship's base stats and swaps installed tech that the rules
// would drop on the next load. Stat values the player has are kept.
void SetRules(uintptr_t ps, int index, int rules, bool tell)
{
    uintptr_t res = ShipRes(ps, index);
    std::string file = ReadVarString(res + F.resFile);
    uint64_t sVal = 0;
    Rd(res + F.resSeed, sVal);
    struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } s{ sVal, 1, {} };
    const int choices[3] = { 4, 5, 6 };
    if (RulesFromStats(ReadStoreStats(ShipStore(ps, index, 0))) != rules && F.genBaseStats) {
        int32_t cls = 0;
        Rd(ShipStore(ps, index, 0) + F.cls, cls);
        for (int k = 0; k < 3; ++k) {
            uintptr_t st = ShipStore(ps, index, k);
            auto old = ReadStoreStats(st);
            // living ships only roll as S class, other classes would come back without the marker
            F.genBaseStats((void*)st, choices[k], &s, rules == RulesLiving ? 3 : cls, TypeForRules(rules, file), 0xA, 0, false);
            WriteStoreStats(st, old);
        }
    }
    bool marked = RulesFromStats(ReadStoreStats(ShipStore(ps, index, 0))) == rules;

    auto base = BaseTechs();
    std::vector<std::string> swapped, lost;
    for (int k = 0; k < 3; ++k) {
        uintptr_t st = ShipStore(ps, index, k), data = 0;
        uint32_t n = 0;
        if (!Rd(st + 0x8C, n) || n > 4096 || !Rd(st + 0x90, data) || !data) continue;
        std::set<std::string> have;
        for (uint32_t i = 0; i < n; ++i) have.insert(ReadFixed(data + (uintptr_t)i * TF.elSize + TF.elId, 16));
        for (uint32_t i = 0; i < n; ++i) {
            uintptr_t el = data + (uintptr_t)i * TF.elSize;
            int32_t type = -1;
            if (!Rd(el + TF.elType, type) || type != 1) continue; // technology
            std::string id = ReadFixed(el + TF.elId, 16);
            auto t = LookupTech(id, base);
            if (!t || t->cat < 0 || RulesAllow(rules, t->cat) || t->id.starts_with("SHIPSLOT_") || t->id.starts_with("SHIPEASY_")) continue;
            const TechRec* to = id.find('#') == std::string::npos ? Counterpart(*t, rules, base, have) : nullptr;
            if (!to) { lost.push_back(TechName(id)); continue; }
            char key[16]{};
            memcpy(key, to->id.data(), std::min<size_t>(to->id.size(), 15));
            for (int b = 0; b < 16; ++b) Wr<char>(el + TF.elId + b, key[b]);
            int32_t max = 0;
            if (to->charge > 0 && Rd(el + TF.elMax, max) && max > 0) { Wr<int32_t>(el + TF.elAmount, to->charge); Wr<int32_t>(el + TF.elMax, to->charge); }
            have.insert(to->id);
            swapped.push_back(TechName(id) + " > " + TechName(to->id));
        }
        NotifyStore(ps, choices[k], index);
    }
    MarkStatsDirty(ps);
    log::Info("Garage", "Ship {} tech rules {}: marker {}, {} swapped, {} won't stay", index, rules, marked, swapped.size(), lost.size());
    if (!tell) return;
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 6; ++i) s += (i ? ", " : "") + v[i];
        if (v.size() > 6) s += ", ...";
        return s;
    };
    std::string body = !marked ? L::T("gar.rules.fail")
        : swapped.empty() && lost.empty() ? L::T("gar.rules.fits")
        : (swapped.empty() ? "" : L::Tf("gar.rules.swapped", join(swapped))) + (swapped.empty() || lost.empty() ? "" : "\n")
            + (lost.empty() ? "" : L::Tf("gar.rules.lost", join(lost)));
    std::string title = L::Tf("gar.rules.done", L::T(("gar.rules." + std::to_string(rules)).c_str()));
    unsigned glyph = marked && lost.empty() ? icon::Check : icon::Warning;
    Registry::Get().Post([title, body, glyph] { ui::Toast(title, body, glyph); });
}

void Apply(Edit e)
{
    game::BackupSaveOnce();
    game::Post([e] {
        uintptr_t ps = game::PlayerState();
        if (!ps) return;
        TakeSnap(e, ps);
        if (e.kind == ShipKind) {
            uintptr_t res = ShipRes(ps, e.index);
            bool mesh = false;
            // a new model brings its own tech rules unless the player picked rules
            int was = RulesFromStats(ReadStoreStats(ShipStore(ps, e.index, 0)));
            int want = e.rules >= 0 ? e.rules : !e.file.empty() ? RulesForFile(e.file) : was;
            if (!e.file.empty() && F.setString) { F.setString((void*)(res + F.resFile), e.file.c_str()); mesh = true; }
            if (e.seed) {
                Wr<uint64_t>(res + F.resSeed, *e.seed);
                Wr<uint8_t>(res + F.resSeed + 8, 1);
                SeedSlot(ps, ShipKind, e.index, *e.seed);
                mesh = true;
            }
            if (e.cls >= 0) {
                std::string curFile = ReadVarString(res + F.resFile);
                int shipType = TypeForRules(want, curFile);
                uint64_t sVal = 0;
                Rd(res + F.resSeed, sVal);
                struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } s{ sVal, 1, {} };
                int choices[3] = { 4, 5, 6 };
                for (int k = 0; k < 3; ++k) {
                    uintptr_t st = ShipStore(ps, e.index, k);
                    Wr<int32_t>(st + F.cls, e.cls);
                    if (F.genBaseStats) F.genBaseStats((void*)st, choices[k], &s, e.cls, shipType, 0xA, 0, false);
                    if (k == 1 && F.genSpecialSlots) F.genSpecialSlots((void*)st, 5, &s);
                    NotifyStore(ps, choices[k], e.index);
                }
            }
            bool held = RulesFromStats(ReadStoreStats(ShipStore(ps, e.index, 0))) == want;
            if (e.rules >= 0 || want != was || !held) SetRules(ps, e.index, want, e.rules >= 0 || want != was);
            if (e.rename) WriteFixed(ps + kShipNames + e.index * kNameLen, e.name, kNameLen);
            if (mesh && e.index == Primary() && !RefreshShip()) log::Warn("Garage", "Ship is still rebuilding, try again in a moment");
        }
        else if (e.kind == ToolKind) {
            auto buf = ToolData(e.index);
            uintptr_t b = (uintptr_t)buf.data();
            bool rebuild = false;
            if (!e.file.empty() && F.setString) { F.setString((void*)(b + F.toolRes + F.resFile), e.file.c_str()); rebuild = true; }
            if (e.seed) { memcpy(buf.data() + F.toolSeed, &*e.seed, 8); buf[F.toolSeed + 8] = 1; rebuild = true; }
            if (e.rename) { WriteFixed(b + F.toolName, e.name, kNameLen); rebuild = true; }
            if (rebuild) ToolApply(e.index, buf);
            if (e.cls >= 0) {
                uintptr_t slot = ToolSlot(e.index);
                std::string curFile = ReadVarString(b + F.toolRes + F.resFile);
                int weapClass = WeaponClassEnumFromFile(curFile, slot);
                uint64_t sVal = 0;
                memcpy(&sVal, buf.data() + F.toolSeed, 8);
                struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } s{ sVal, 1, {} };
                Wr<int32_t>(slot + F.cls, e.cls);
                if (F.genBaseStats) F.genBaseStats((void*)slot, 3, &s, e.cls, 0xC, weapClass, 0, false);
                if (F.genSpecialSlots) F.genSpecialSlots((void*)slot, 3, &s);
                if (e.index == ToolActive()) {
                    uintptr_t wst = ps + kWeaponInv;
                    Wr<int32_t>(wst + F.cls, e.cls);
                    if (F.genBaseStats) F.genBaseStats((void*)wst, 3, &s, e.cls, 0xC, weapClass, 0, false);
                    if (F.genSpecialSlots) F.genSpecialSlots((void*)wst, 3, &s);
                    NotifyStore(ps, 3, -1);
                }
            }
        }
        else if (e.kind == FreighterKind) {
            if (uintptr_t fo = FreighterOwn()) {
                bool mesh = false;
                if (!e.file.empty() && F.setString) { F.setString((void*)(fo + kFreighterRes + F.resFile), e.file.c_str()); mesh = true; }
                if (e.seed) {
                    Wr<uint64_t>(fo + kFreighterRes + F.resSeed, *e.seed);
                    Wr<uint8_t>(fo + kFreighterRes + F.resSeed + 8, 1);
                    std::string file = ReadVarString(fo + kFreighterRes + F.resFile);
                    SeedSlot(ps, FreighterKind, 0, *e.seed);
                    mesh = true;
                }
                if (e.cls >= 0) {
                    uint64_t sVal = 0;
                    Rd(fo + kFreighterRes + F.resSeed, sVal);
                    struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } s{ sVal, 1, {} };
                    for (int k = 0; k < 3; ++k) {
                        uintptr_t st = ps + kFreighterStores + k * kStride;
                        Wr<int32_t>(st + F.cls, e.cls);
                        if (k == 0 && F.genBaseStats) F.genBaseStats((void*)st, 7, &s, e.cls, 8, 0xA, 0, false);
                        if (k == 1 && F.genSpecialSlots) F.genSpecialSlots((void*)st, 8, &s);
                    }
                    NotifyStore(ps, 7, 0);
                }
                if (e.rename) WriteFixed(fo + kFreighterName, e.name, kNameLen);
                if (mesh && !RefreshFreighter()) log::Warn("Garage", "Freighter is still rebuilding, try again in a moment");
            }
        }
        ReadOwned();
    });
}

// Applies a changed look to one item, keeping a snapshot for Revert.
void PostCust(Kind kind, int index, CustData c)
{
    game::BackupSaveOnce();
    game::Post([kind, index, c = std::move(c)]() mutable {
        uintptr_t ps = game::PlayerState();
        if (!ps) return;
        TakeSnap(Edit{ kind, index }, ps);
        WriteCust(ps, kind, index, std::move(c));
        LoadDetail(kind, index);
        ReadOwned();
    });
}

// The game caches each inventory's computed stats and only rebuilds a block marked dirty.
void MarkStatsDirty(uintptr_t ps)
{
    for (auto& g : g_statCache)
        for (uint32_t o = g.base; o < g.base + g.size; o += g.stride) Wr<uint8_t>(ps + o + g.flag, 1);
}

void ApplyStatChange(Kind kind, int index, std::vector<StatEntry> stats)
{
    game::BackupSaveOnce();
    game::Post([kind, index, stats = std::move(stats)] {
        uintptr_t ps = game::PlayerState();
        if (!ps) return;
        TakeSnap(Edit{ kind, index }, ps);
        int n = 0;
        if (kind == ShipKind) {
            int choices[3] = { 4, 5, 6 };
            for (int k = 0; k < 3; ++k) {
                n += WriteStoreStats(ShipStore(ps, index, k), stats);
                NotifyStore(ps, choices[k], index);
            }
        }
        else if (kind == ToolKind) {
            n += WriteStoreStats(ToolSlot(index), stats);
            if (index == ToolActive()) {
                n += WriteStoreStats(ps + kWeaponInv, stats);
                NotifyStore(ps, 3, -1);
            }
        }
        else if (kind == FreighterKind) {
            for (int k = 0; k < 3; ++k) n += WriteStoreStats(ps + kFreighterStores + k * kStride, stats);
            NotifyStore(ps, 7, 0);
        }
        MarkStatsDirty(ps);
        log::Info("Garage", "Wrote {} base stat values", n);
        ReadOwned();
    });
}

void Revert(Kind kind, int index, bool keep)
{
    game::Post([kind, index, keep] {
        std::string key = Owned{ kind, index }.Key();
        auto it = g_snaps.find(key);
        uintptr_t ps = game::PlayerState();
        if (it == g_snaps.end() || !ps) return;
        Snap s = std::move(it->second);
        g_snaps.erase(it);
        if (!keep) {
            if (s.cust) WriteCust(ps, kind, index, std::move(*s.cust), kind == SuitKind || kind == VehicleKind);
            if (kind == ShipKind) {
                uintptr_t res = ShipRes(ps, index);
                if (F.setString && !s.file.empty()) F.setString((void*)(res + F.resFile), s.file.c_str());
                Wr<uint64_t>(res + F.resSeed, s.seed);
                Wr<uint8_t>(res + F.resSeed + 8, s.use);
                for (int k = 0; k < 3; ++k) {
                    Wr<int32_t>(ShipStore(ps, index, k) + F.cls, s.cls[k]);
                    WriteStoreStats(ShipStore(ps, index, k), s.stats);
                }
                WriteFixed(ps + kShipNames + index * kNameLen, s.name, kNameLen);
                if (index == Primary()) RefreshShip();
            }
            else if (kind == ToolKind) {
                if (!s.tool.empty()) ToolApply(index, s.tool);
                Wr<int32_t>(ToolSlot(index) + F.cls, s.cls[0]);
                WriteStoreStats(ToolSlot(index), s.stats);
                if (index == ToolActive()) {
                    Wr<int32_t>(ps + kWeaponInv + F.cls, s.cls[0]);
                    WriteStoreStats(ps + kWeaponInv, s.stats);
                }
            }
            else if (kind == FreighterKind) {
                if (uintptr_t fo = FreighterOwn()) {
                    if (F.setString && !s.file.empty()) F.setString((void*)(fo + kFreighterRes + F.resFile), s.file.c_str());
                    Wr<uint64_t>(fo + kFreighterRes + F.resSeed, s.seed);
                    Wr<uint8_t>(fo + kFreighterRes + F.resSeed + 8, s.use);
                    for (int k = 0; k < 3; ++k) {
                        Wr<int32_t>(ps + kFreighterStores + k * kStride + F.cls, s.cls[k]);
                        WriteStoreStats(ps + kFreighterStores + k * kStride, s.stats);
                    }
                    WriteFixed(fo + kFreighterName, s.name, kNameLen);
                    RefreshFreighter();
                }
            }
        }
        LoadDetail(kind, index);
        ReadOwned();
    });
}

// ---- UI ----

const char* KindKey(Kind k)
{
    switch (k) {
    case ShipKind: return "gar.chip.ship";
    case ToolKind: return "gar.chip.tool";
    case FreighterKind: return "gar.chip.freighter";
    case SuitKind: return "gar.chip.suit";
    case VehicleKind: return "gar.chip.vehicle";
    }
    return "gar.chip.ship";
}

unsigned KindGlyph(Kind k)
{
    switch (k) {
    case ShipKind: return icon::Rocket;
    case ToolKind: return icon::Tool;
    case FreighterKind: return icon::Garage;
    case SuitKind: return icon::Astronaut;
    case VehicleKind: return icon::Rover;
    }
    return icon::Rocket;
}

std::string ModelName(const std::string& file)
{
    for (auto& m : kModels) if (_stricmp(m.path, file.c_str()) == 0) return L::T(m.key);
    size_t s = file.find_last_of('/');
    std::string base = s == std::string::npos ? file : file.substr(s + 1);
    if (size_t dot = base.find('.'); dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

std::string ClassName(int c) { return c >= 0 && c <= 3 ? std::string(1, "CBAS"[c]) : "?"; }

std::string SeedText(uint64_t s)
{
    char b[24];
    snprintf(b, sizeof b, "0x%016llX", (unsigned long long)s);
    return b;
}

std::optional<uint64_t> ParseSeed(std::string t)
{
    std::erase_if(t, [](char c) { return c == ' ' || c == '_'; });
    if (t.empty()) return std::nullopt;
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { t = t.substr(2); base = 16; }
    else if (t.find_first_of("abcdefABCDEF") != std::string::npos) base = 16;
    char* end = nullptr;
    uint64_t v = strtoull(t.c_str(), &end, base);
    if (!end || *end) return std::nullopt;
    return v;
}

const char* StatLabel(const std::string& id)
{
    if (id == "SHIP_DAMAGE" || id == "WEAPON_DAMAGE") return L::T("gar.stat.damage");
    if (id == "SHIP_SHIELD") return L::T("gar.stat.shield");
    if (id == "SHIP_HYPERDRIVE" || id == "FREI_HYPERDRIVE") return L::T("gar.stat.hyperdrive");
    if (id == "SHIP_AGILE") return L::T("gar.stat.agile");
    if (id == "WEAPON_MINING") return L::T("gar.stat.mining");
    if (id == "WEAPON_SCAN") return L::T("gar.stat.scan");
    if (id == "FREI_FLEET") return L::T("gar.stat.fleet");
    return id.c_str();
}

void DrawList(const char* query)
{
    if (!game::PlayerState()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T("items.ingame"));
        return;
    }
    static int tick = 0;
    if (++tick % 30 == 1) game::Post(ReadOwned);
    std::vector<Owned> owned;
    {
        std::lock_guard lk(g_mx);
        owned = g_owned;
    }
    // what this save holds, against its slot limits
    {
        int ships = 0, tools = 0;
        for (auto& o : owned) ships += o.kind == ShipKind, tools += o.kind == ToolKind;
        std::string line = L::Tf("gar.count", std::to_string(ships), std::to_string(tools));
        ImGui::GetWindowDrawList()->AddText(ui::theme::Body(), S(15), ImGui::GetCursorScreenPos(), P().textDim, line.c_str());
        ImGui::Dummy(ImVec2(0, S(24)));
    }
    std::string sig;
    for (auto& o : owned) sig += o.Key() + o.file + o.name + SeedText(o.seed) + ClassName(o.cls) + (o.current ? "*" : "") + "|";
    if (sig != g.sig || g.view.empty()) {
        g.sig = sig;
        g.view.clear();
        {
            ui::BrowserEntry add;
            add.key = kAddKey;
            add.title = L::T("gar.add.title");
            add.subtitle = L::T("gar.add.sub");
            add.glyph = icon::Plus;
            add.chip = 0;
            g.view.push_back(std::move(add));
        }
        for (auto& o : owned) {
            ui::BrowserEntry e;
            e.key = o.Key();
            if (o.kind == SuitKind || o.kind == VehicleKind) {
                e.title = o.name;
                e.subtitle = L::T(KindKey(o.kind));
            }
            else {
                std::string type = ModelName(o.file);
                e.title = o.name.empty() ? type : o.name;
                e.subtitle = type + "   /   " + L::Tf("gar.classof", ClassName(o.cls));
            }
            e.glyph = KindGlyph(o.kind);
            e.chip = o.kind;
            e.marked = o.current;
            g.view.push_back(std::move(e));
        }
        for (auto& m : kModels) {
            if (assets::paks::Ready() && !assets::paks::Exists(m.path)) continue;
            ui::BrowserEntry e;
            e.key = m.path;
            e.title = L::T(m.key);
            e.subtitle = L::Tf("gar.model", L::T(KindKey(m.kind)));
            e.glyph = KindGlyph(m.kind);
            e.chip = kModelChip;
            g.view.push_back(std::move(e));
        }
        g.browser.Invalidate();
    }
    g.browser.Draw(g.view, query);
}

float Label(ImDrawList* dl, float x, float y, const char* key)
{
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T(key));
    return y + S(21);
}

enum SubTab { TabGeneral, TabParts, TabPaint, TabStats };

float DrawSubTabs(ImDrawList* dl, float x, float y, int* tab, const bool show[4])
{
    const char* keys[4] = { "gar.tab.general", "gar.tab.parts", "gar.tab.paint", "gar.tab.stats" };
    float cx = x, h = S(26);
    for (int i = 0; i < 4; ++i) {
        if (!show[i]) continue;
        std::string up = ui::Upper(L::T(keys[i]));
        float w = ui::SpacedWidth(ui::theme::Title(), S(14), up.c_str(), S(1.1f)) + S(24);
        ImGui::SetCursorScreenPos(ImVec2(cx, y));
        ImGui::PushID(900 + i);
        bool click = ImGui::InvisibleButton("##subtab", ImVec2(w, h));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = (*tab == i);
        dl->AddRectFilled(ImVec2(cx, y), ImVec2(cx + w, y + h), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 110), S(13));
        ui::SpacedText(dl, ui::theme::Title(), S(14), ImVec2(cx + S(12), y + (h - S(14)) * 0.5f - S(1)),
                       on || hov ? P().text : P().textDim, up.c_str(), S(1.1f));
        if (click) *tab = i;
        cx += w + S(6);
    }
    return y + h + S(12);
}

bool Has(const std::vector<Id16>& v, const std::string& id)
{
    for (auto& d : v) if (id == d.s) return true;
    return false;
}

// Sets that apply to this item: one of their groups is on, or the group they need is.
std::vector<const DescSet*> SetsFor(const std::vector<DescSet>& sets, const std::vector<Id16>& descs)
{
    std::vector<const DescSet*> out;
    for (auto& st : sets) {
        bool on = !st.needs.empty() && Has(descs, st.needs);
        for (auto& gr : st.groups) on = on || Has(descs, gr.id);
        if (on) out.push_back(&st);
    }
    return out;
}

// Picks one group of a set, then keeps the "needs" rules: a head set only stays while its race is on.
std::vector<Id16> PickGroup(const std::vector<DescSet>& sets, std::vector<Id16> descs, const DescSet& set, const std::string& id)
{
    auto drop = [&](const DescSet& st) {
        std::erase_if(descs, [&](const Id16& d) { for (auto& gr : st.groups) if (gr.id == d.s) return true; return false; });
    };
    auto add = [&](const std::string& g) {
        Id16 d;
        strncpy(d.s, g.c_str(), sizeof d.s - 1);
        descs.push_back(d);
    };
    drop(set);
    add(id);
    for (auto& st : sets) {
        if (st.needs.empty()) continue;
        bool any = false;
        for (auto& gr : st.groups) any = any || Has(descs, gr.id);
        if (!Has(descs, st.needs)) drop(st);
        else if (!any) add(st.groups.front().id);
    }
    return descs;
}

struct Finish { const char* group; std::vector<const char*> ids; std::vector<const char*> keys; };
const Finish* FinishFor(const std::string& file)
{
    static const Finish fight{ "SHIP_FIGHT", { "PANELS", "COATING", "STEALTH", "METALBOLT", "SEVENTEEN" },
                               { "gar.fin.panels", "gar.fin.coating", "gar.fin.stealth", "gar.fin.metal", "gar.fin.titanium" } };
    static const Finish sail{ "SHIP_SAIL", { "PANELS", "COATING", "STEALTH", "METALBOLT" },
                              { "gar.fin.panels", "gar.fin.coating", "gar.fin.stealth", "gar.fin.metal" } };
    static const Finish sci{ "SHIP_SCI", { "DEFAULT", "SHINY" }, { "gar.fin.default", "gar.fin.metal" } };
    switch (ModTypeForFile(file)) {
    case 1: case 2: return &fight;
    case 3: return &sci;
    case 5: return &sail;
    default: return nullptr;
    }
}

// ---- parts by seed ----
// Every procedural model rolls its parts from its seed. The roll is the game's own (Descriptor.Generate),
// so a seed search finds a seed that grows the parts the player asks for. Living, exotic and sentinel
// ships have no customiser, so this is their only way to pick parts.

struct LivGroup { std::string type, label; int depth = 0; std::vector<std::string> opts, labels; };
struct LivSearch { std::string file; std::vector<std::string> want; uint64_t found = 0; int tries = 0; bool running = false, done = false; };
std::vector<LivGroup> g_liv;       // under g_mx
std::string g_livFile;             // file g_liv was read from, under g_mx
std::atomic<bool> g_livLoading{ false };
std::vector<std::string> g_livCur; // parts of the inspected seed, under g_mx
std::string g_livCurKey;           // item and seed g_livCur is for, UI thread
LivSearch g_livSearch;             // under g_mx
constexpr int kLivMaxTries = 300000, kLivPerFrame = 400;

struct LivDesc { uint32_t cap = 0, count = 0; uintptr_t data = 0; uint64_t seed = 0; uint8_t valid = 0, pad[7]{}; uint64_t seed2 = ~0ull; uint8_t valid2 = 0, pad2[7]{}; };
struct LivList { uint32_t cap = 0, count = 0; uintptr_t data = 0; };
static_assert(sizeof(LivDesc) == 0x30 && sizeof(LivList) == 0x10);

bool SeedOnly(const std::string& file)
{
    for (const char* k : { "BIOSHIP", "SENTINELSHIP", "S-CLASS_PROC" }) if (file.find(k) != std::string::npos) return true;
    return false;
}

// "_FEET_ALOD0" -> "_FEET_A": the LOD entries are one visual option.
std::string LivBase(std::string id)
{
    size_t p = id.rfind("LOD");
    if (p != std::string::npos && p + 3 < id.size() && std::all_of(id.begin() + p + 3, id.end(), [](char c) { return isdigit((unsigned char)c) != 0; }))
        id.resize(p);
    return id;
}

// Game thread. The parts a seed grows, as base ids.
bool LivRoll(const std::string& file, uint64_t seed, std::vector<std::string>& out)
{
    static LivDesc d;   // reused: the roll clears the count and keeps the buffer
    static LivList extra;
    static const char empty[32] = {};
    if (!F.genDesc) return false;
    struct { uint64_t v; uint8_t valid, pad[7]; } sd{ seed, 1, {} };
    extra.count = 0;
    F.genDesc(&d, file.c_str(), &sd, empty, empty, &extra);
    out.clear();
    for (uint32_t i = 0; i < d.count && i < 64 && d.data; ++i) out.push_back(LivBase(ReadFixed(d.data + (uintptr_t)i * 0x20, 32)));
    return true;
}

// Game thread: every part group and option in the model's descriptor list.
void LivLoad(std::string file)
{
    namespace meta = hooks::meta;
    uint32_t listOff = 0, descsOff = 0, typeOff = 16, idOff = 0, kidsOff = 32, listSize = 32, dataSize = 200;
    if (auto m = meta::Path("cTkModelDescriptorList", "List")) listOff = m->offset;
    if (auto m = meta::Path("cTkResourceDescriptorList", "Descriptors")) descsOff = m->offset;
    if (auto m = meta::Path("cTkResourceDescriptorList", "TypeId")) typeOff = m->offset;
    if (auto m = meta::Path("cTkResourceDescriptorData", "Id")) idOff = m->offset;
    if (auto m = meta::Path("cTkResourceDescriptorData", "Children")) kidsOff = m->offset;
    if (auto* c = meta::Get("cTkResourceDescriptorList")) listSize = c->size;
    if (auto* c = meta::Get("cTkResourceDescriptorData")) dataSize = c->size;
    std::vector<LivGroup> out;
    std::function<void(uintptr_t, int)> walk = [&](uintptr_t arr, int depth) {
        uintptr_t p = 0;
        int32_t n = 0;
        if (depth > 6 || !Rd(arr, p) || !Rd(arr + 8, n) || !p || n <= 0 || n > 64) return;
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t l = p + (uintptr_t)i * listSize;
            std::string type = ReadFixed(l + typeOff, 16);
            size_t gi = 0;
            while (gi < out.size() && out[gi].type != type) ++gi;
            if (gi == out.size()) {
                LivGroup g;
                g.type = type;
                g.depth = depth;
                std::string t = type;
                while (!t.empty() && t.front() == '_') t.erase(t.begin());
                while (!t.empty() && t.back() == '_') t.pop_back();
                g.label = Pretty(t);
                out.push_back(std::move(g));
            }
            uintptr_t dp = 0;
            int32_t dn = 0;
            if (!Rd(l + descsOff, dp) || !Rd(l + descsOff + 8, dn) || !dp || dn <= 0 || dn > 256) continue;
            for (int32_t k = 0; k < dn; ++k) {
                uintptr_t e = dp + (uintptr_t)k * dataSize;
                std::string id = LivBase(ReadFixed(e + idOff, 32));
                auto& gr = out[gi];
                if (!id.empty() && std::find(gr.opts.begin(), gr.opts.end(), id) == gr.opts.end()) {
                    std::string rest = id.size() > type.size() && id.starts_with(type) ? id.substr(type.size()) : id;
                    std::string label = rest.starts_with("NULL") ? std::string(L::T("gar.liv.nopart")) : Pretty(rest);
                    gr.opts.push_back(id);
                    gr.labels.push_back(label.empty() ? id : label);
                }
                walk(e + kidsOff, depth + 1);
            }
        }
    };
    uintptr_t list = F.modelList ? F.modelList(file.c_str()) : 0;
    if (list) walk(list + listOff, 0);
    std::erase_if(out, [](const LivGroup& g) { return g.opts.size() < 2; }); // nothing to choose
    log::Info("Garage", "Living ship: {} part groups", out.size());
    std::lock_guard lk(g_mx);
    g_liv = std::move(out);
    g_livFile = file;
    g_livLoading = false;
}

// Game thread, one batch per frame until a seed has every wanted part.
void LivStep()
{
    LivSearch s;
    {
        std::lock_guard lk(g_mx);
        if (!g_livSearch.running) return;
        s = g_livSearch;
    }
    std::vector<std::string> parts;
    for (int i = 0; i < kLivPerFrame && s.tries < kLivMaxTries; ++i, ++s.tries) {
        uint64_t seed = RandomSeed();
        if (!LivRoll(s.file, seed, parts)) { s.tries = kLivMaxTries; break; }
        bool all = true;
        for (auto& w : s.want) all = all && std::find(parts.begin(), parts.end(), w) != parts.end();
        if (all) { s.found = seed; break; }
    }
    bool more = !s.found && s.tries < kLivMaxTries;
    {
        std::lock_guard lk(g_mx);
        if (!g_livSearch.running) return; // stopped meanwhile
        g_livSearch.tries = s.tries;
        g_livSearch.found = s.found;
        g_livSearch.running = more;
        g_livSearch.done = !more;
    }
    if (more) game::Post(LivStep);
}

float DrawSeedParts(ImDrawList* dl, float x, float y, float bw, const Owned& o, bool live)
{
    static std::map<std::string, std::string> want; // group type -> wanted base id
    static std::string wantFor;
    if (wantFor != o.Key()) { want.clear(); wantFor = o.Key(); }
    std::vector<LivGroup> groups;
    std::vector<std::string> cur;
    LivSearch srch;
    bool loaded;
    {
        std::lock_guard lk(g_mx);
        loaded = g_livFile == o.file;
        groups = g_liv;
        cur = g_livCur;
        srch = g_livSearch;
    }
    if (!F.genDesc || o.file.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.noparts"));
        return y + S(28);
    }
    if (!SeedOnly(o.file)) { // a second way next to the customiser, folded away by default
        static bool open = false;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##seedparts", L::T(open ? "gar.liv.hide" : "gar.liv.show"), bw, ui::Tone::Teal, live)) open = !open;
        y += S(44);
        if (!open) return y;
    }
    if (!loaded) {
        if (live && !g_livLoading.exchange(true)) game::Post([f = o.file] { LivLoad(f); });
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.liv.loading"));
        return y + S(28);
    }
    if (std::string key = o.Key() + SeedText(o.seed); live && g_livCurKey != key) { // the parts it has now
        g_livCurKey = key;
        game::Post([f = o.file, sd = o.seed] {
            std::vector<std::string> parts;
            LivRoll(f, sd, parts);
            std::lock_guard lk(g_mx);
            g_livCur = std::move(parts);
        });
    }
    const char* note = L::T(SeedOnly(o.file) ? "gar.liv.note" : "gar.liv.note2");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + S(10);

    float half = (bw - S(8)) * 0.5f;
    int shown = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        const LivGroup& gr = groups[i];
        float cx = x + (shown % 2) * (half + S(8));
        dl->AddText(ui::theme::Body(), S(14), ImVec2(cx, y), P().textDim, gr.label.c_str());
        std::vector<std::string> labels{ L::T("gar.liv.any") };
        labels.insert(labels.end(), gr.labels.begin(), gr.labels.end());
        auto w = want.find(gr.type);
        int sel = 0;
        for (size_t k = 0; k < gr.opts.size(); ++k) {
            if (w != want.end() && w->second == gr.opts[k]) sel = (int)k + 1;
            if (std::find(cur.begin(), cur.end(), gr.opts[k]) != cur.end()) labels[k + 1] += L::T("gar.liv.now");
        }
        ImGui::SetCursorScreenPos(ImVec2(cx, y + S(19)));
        ImGui::PushID((int)i + 9000);
        int before = sel;
        if (ui::Dropdown("##liv", labels, &sel, half, live && !srch.running) && sel != before) {
            if (sel == 0) want.erase(gr.type);
            else want[gr.type] = gr.opts[sel - 1];
        }
        ImGui::PopID();
        if (++shown % 2 == 0) y += S(58);
    }
    if (shown % 2) y += S(58);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (!srch.running) {
        if (ui::Button("##livfind", L::T("gar.liv.find"), half, ui::Tone::Green, live && !want.empty())) {
            LivSearch s;
            s.file = o.file;
            for (auto& [t, id] : want) s.want.push_back(id);
            s.running = true;
            {
                std::lock_guard lk(g_mx);
                g_livSearch = s;
            }
            game::Post(LivStep);
        }
    }
    else if (ui::Button("##livstop", L::T("gar.liv.stop"), half, ui::Tone::Teal, true)) {
        std::lock_guard lk(g_mx);
        g_livSearch.running = false;
    }
    ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
    if (ui::Button("##livclear", L::T("gar.liv.clear"), half, ui::Tone::Teal, live && !want.empty() && !srch.running)) want.clear();
    y += S(44);

    std::string status;
    if (srch.running) status = L::Tf("gar.liv.searching", std::to_string(srch.tries));
    else if (srch.done && srch.found) status = L::Tf("gar.liv.found", SeedText(srch.found));
    else if (srch.done) status = L::Tf("gar.liv.notfound", std::to_string(srch.tries));
    if (!status.empty()) {
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, status.c_str());
        y += S(22);
    }
    if (srch.done && srch.found && srch.file == o.file) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##livuse", L::T("gar.liv.use"), bw, ui::Tone::Green, live)) {
            if (o.index < 0) { // a draft: the seed goes to the draft, nothing in the save changes
                g.draft.seed = srch.found;
                snprintf(g.draft.seedText, sizeof g.draft.seedText, "%s", SeedText(srch.found).c_str());
            }
            else {
                Edit e{ o.kind, o.index };
                e.seed = srch.found;
                snprintf(g.seed, sizeof g.seed, "%s", SeedText(srch.found).c_str());
                Apply(e);
            }
            want.clear();
            std::lock_guard lk(g_mx);
            g_livSearch = {};
        }
        y += S(44);
    }
    return y;
}

float DrawParts(ImDrawList* dl, float x, float y, float bw, const Owned& o, const Detail& d, bool live)
{
    if (o.kind == ShipKind && SeedOnly(o.file)) return DrawSeedParts(dl, x, y, bw, o, live);
    std::vector<DescSet> sets;
    {
        std::lock_guard lk(g_mx);
        sets = g_sets;
    }
    float half = (bw - S(8)) * 0.5f;
    auto sel = SetsFor(sets, d.cust.descs);
    int shown = 0;
    for (size_t i = 0; i < sel.size(); ++i) {
        const DescSet& st = *sel[i];
        float cx = x + (shown % 2) * (half + S(8));
        dl->AddText(ui::theme::Body(), S(14), ImVec2(cx, y), P().textDim, st.label.c_str());
        std::vector<std::string> labels;
        int cur = -1;
        for (size_t k = 0; k < st.groups.size(); ++k) {
            labels.push_back(st.groups[k].label);
            if (Has(d.cust.descs, st.groups[k].id)) cur = (int)k;
        }
        ImGui::SetCursorScreenPos(ImVec2(cx, y + S(19)));
        ImGui::PushID((int)i);
        int pick = cur;
        if (ui::Dropdown("##set", labels, &pick, half, live) && pick >= 0 && pick != cur) {
            CustData c = d.cust;
            c.descs = PickGroup(sets, c.descs, st, st.groups[pick].id);
            PostCust(o.kind, o.index, std::move(c));
        }
        ImGui::PopID();
        if (++shown % 2 == 0) y += S(58);
    }
    if (shown % 2) y += S(58);

    if (const Finish* fin = o.kind == ShipKind ? FinishFor(o.file) : nullptr) {
        std::vector<std::string> labels;
        int cur = -1;
        for (size_t k = 0; k < fin->ids.size(); ++k) {
            labels.push_back(L::T(fin->keys[k]));
            for (auto& t : d.cust.tex) if (!strcmp(t.group, fin->group) && !strcmp(t.name, fin->ids[k])) cur = (int)k;
        }
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, L::T("gar.finish"));
        ImGui::SetCursorScreenPos(ImVec2(x, y + S(19)));
        int pick = cur;
        if (ui::Dropdown("##finish", labels, &pick, half, live) && pick >= 0 && pick != cur) {
            CustData c = d.cust;
            std::erase_if(c.tex, [&](const TexOption& t) { return !strcmp(t.group, fin->group); });
            TexOption t;
            strncpy(t.name, fin->ids[pick], sizeof t.name - 1);
            strncpy(t.group, fin->group, sizeof t.group - 1);
            c.tex.push_back(t);
            PostCust(o.kind, o.index, std::move(c));
        }
        y += S(58);
    }
    if (o.kind == ShipKind || o.kind == ToolKind || o.kind == FreighterKind) return DrawSeedParts(dl, x, y + S(6), bw, o, live);
    if (!shown) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.noparts"));
        y += S(28);
    }
    return y;
}

// Colour groups the item uses now, plus the usual ones for its kind so a fresh item has something to paint.
std::vector<std::pair<int, int>> PaintGroups(const Owned& o, const CustData& c)
{
    std::vector<std::pair<int, int>> out;
    auto add = [&](int pal, int alt) {
        if (std::find(out.begin(), out.end(), std::pair{ pal, alt }) == out.end()) out.push_back({ pal, alt });
    };
    // only ColourAlt 0..4 are saved, so higher ones would be lost on reload
    for (auto& e : c.colours) if (e.palette >= 0 && e.palette < 66 && e.alt >= 0 && e.alt < 5) add(e.palette, e.alt);
    if (o.kind == FreighterKind) { add(56, 0); add(56, 2); add(10, 2); }
    else if (o.kind == ShipKind) {
        add(20, 0); add(10, 0); add(10, 3);
        if (ModTypeForFile(o.file) == 5) add(55, 0);
    }
    else if (out.empty()) { add(20, 0); add(10, 0); add(10, 2); }
    // Ship types differ in which groups their materials read, so offer the common ones when asked.
    if (g.moreGroups)
        for (int pal : { 20, 10, 9 })
            for (int alt = 0; alt < 5; ++alt) add(pal, alt);
    return out;
}

std::string PaintLabel(int pal, int alt)
{
    static auto pals = meta::EnumNames("cTkPaletteTexture", "Palette");
    static auto alts = meta::EnumNames("cTkPaletteTexture", "ColourAlt");
    auto name = [](const auto& list, int v) -> std::string {
        for (auto& [k, n] : list) if (k == v) return n;
        return std::to_string(v);
    };
    std::string a = alt == 0 ? L::T("gar.col.primary") : L::Tf("gar.col.alt", std::to_string(alt));
    if (!alts.empty() && alt != 0) a = Pretty(name(alts, alt));
    return Pretty(name(pals, pal)) + "  /  " + a;
}

ImU32 Col(const float* c) { return IM_COL32((int)(std::clamp(c[0], 0.f, 1.f) * 255), (int)(std::clamp(c[1], 0.f, 1.f) * 255), (int)(std::clamp(c[2], 0.f, 1.f) * 255), 255); }

float DrawPaint(ImDrawList* dl, float x, float y, float bw, const Owned& o, const Detail& d, bool live)
{
    auto groups = PaintGroups(o, d.cust);
    g.colGroup = std::clamp(g.colGroup, 0, (int)groups.size() - 1);

    // One row per colour group: its current colour and name.
    for (int i = 0; i < (int)groups.size(); ++i) {
        auto [pal, alt] = groups[i];
        const float* cur = nullptr;
        for (auto& e : d.cust.colours) if (e.palette == pal && e.alt == alt) cur = e.rgba;
        float h = S(28);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(1300 + i);
        bool click = ImGui::InvisibleButton("##grp", ImVec2(bw, h));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = g.colGroup == i;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + h), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 90));
        ImVec2 sa(x + S(6), y + S(5)), sb(x + S(34), y + h - S(5));
        if (cur) dl->AddRectFilled(sa, sb, Col(cur));
        dl->AddRect(sa, sb, P().lineStrong, 0.f, S(1));
        std::string lbl = PaintLabel(pal, alt) + (cur ? "" : "   " + std::string(L::T("gar.col.default")));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(44), y + (h - S(15)) * 0.5f), on || hov ? P().text : P().textBody, lbl.c_str());
        if (click) g.colGroup = i;
        y += h + S(4);
    }
    y += S(4);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::Switch("##moregrp", &g.moreGroups);
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(66), y + S(3)), P().textDim, L::T("gar.col.more"));
    y += S(34);
    if (d.cust.legacy) {
        const char* lg = L::T("gar.col.legacy");
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, lg, nullptr, bw);
        y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, lg).y + S(8);
    }
    if (d.swatches.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.nopalette"));
        return y + S(28);
    }

    // The slot's own palette, 16 to a row, the same colours the game's appearance modifier offers.
    int cols = 16;
    float gap = S(3), sw = (bw - gap * (cols - 1)) / cols;
    auto [pal, alt] = groups[g.colGroup];
    for (int i = 0; i < (int)d.swatches.size(); ++i) {
        ImVec2 sa(x + (i % cols) * (sw + gap), y + (i / cols) * (sw + gap));
        ImVec2 sb(sa.x + sw, sa.y + sw);
        ImGui::SetCursorScreenPos(sa);
        ImGui::PushID(1400 + i);
        bool click = ImGui::InvisibleButton("##sw", ImVec2(sw, sw));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        dl->AddRectFilled(sa, sb, Col(d.swatches[i].data()));
        if (hov) dl->AddRect(sa, sb, P().text, 0.f, S(2));
        if (click && live) {
            CustData c = d.cust;
            std::erase_if(c.colours, [&](const ColourEntry& e) { return e.palette == pal && e.alt == alt; });
            ColourEntry e;
            memcpy(e.rgba, d.swatches[i].data(), sizeof e.rgba);
            e.rgba[3] = 1.f;
            e.palette = pal;
            e.alt = alt;
            c.colours.push_back(e);
            c.legacy = 0;
            PostCust(o.kind, o.index, std::move(c));
        }
    }
    y += ((int)d.swatches.size() + cols - 1) / cols * (sw + gap) + S(12);

    // Any colour, not just the palette's. Glow above 1 brightens past the normal range.
    std::string key = o.Key() + "#" + std::to_string(pal) + ":" + std::to_string(alt);
    if (g.pickFor != key) {
        g.pickFor = key;
        g.glow = 1.f;
        for (auto& e : d.cust.colours) if (e.palette == pal && e.alt == alt) {
            float m = std::max({ e.rgba[0], e.rgba[1], e.rgba[2], 1.f });
            for (int k = 0; k < 3; ++k) g.pick[k] = e.rgba[k] / m;
            g.glow = m;
        }
    }
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, L::T("gar.col.custom"));
    y += S(20);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::PushItemWidth(bw);
    ImGui::ColorPicker3("##picker", g.pick, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayRGB |
                                                ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha);
    ImGui::PopItemWidth();
    y = ImGui::GetItemRectMax().y + S(8);
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, L::T("gar.col.glow"));
    ImGui::SetCursorScreenPos(ImVec2(x, y + S(18)));
    ui::EnergyBar("##glow", &g.glow, 0.f, 4.f, "%.1fx", bw, live);
    y += S(50);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##applypick", L::T("gar.col.apply"), bw, ui::Tone::Green, live)) {
        CustData c = d.cust;
        std::erase_if(c.colours, [&](const ColourEntry& e) { return e.palette == pal && e.alt == alt; });
        ColourEntry e;
        for (int k = 0; k < 3; ++k) e.rgba[k] = g.pick[k] * g.glow;
        e.rgba[3] = 1.f;
        e.palette = pal;
        e.alt = alt;
        c.colours.push_back(e);
        c.legacy = 0;
        PostCust(o.kind, o.index, std::move(c));
    }
    return y + S(44);
}

float DrawStatsTab(ImDrawList* dl, float x, float y, float bw, const Owned& o, bool live)
{
    if (g.stats.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.nostats"));
        return y + S(32);
    }
    // Edits stay local while dragging and go to the game when the bar is let go.
    bool commit = false;
    for (size_t i = 0; i < g.stats.size() && i < 6; ++i) {
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, StatLabel(g.stats[i].id));
        ImGui::SetCursorScreenPos(ImVec2(x, y + S(18)));
        char id[24];
        snprintf(id, sizeof id, "##bstat_%zu", i);
        ui::EnergyBar(id, &g.stats[i].value, 0.f, 250.f, "%.1f", bw, live);
        commit = commit || ImGui::IsItemDeactivated();
        y += S(44);
    }
    if (commit && live) ApplyStatChange(o.kind, o.index, g.stats);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##maxbstats", L::T("gar.maxstats"), bw, ui::Tone::Green, live)) {
        for (auto& s : g.stats) s.value = std::max(s.value, 150.f);
        ApplyStatChange(o.kind, o.index, g.stats);
    }
    return y + S(44);
}

// Item preview: our own render of the selected item, above the sub-tabs so it stays
// in view on every tab. Mouse wheel zooms, dragging turns it.
float DrawPreview(ImDrawList* dl, float x, float y, float bw, const Owned& o, const std::optional<Detail>& detail)
{
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::Switch("##preview", &g.previewEnabled);
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(66), y + S(3)), P().textDim, L::T("gar.preview.label"));
    if (g.previewEnabled) {
        bool& spin = Settings::Get().Data().previewSpin;
        const char* sl = L::T("gar.preview.spin");
        float sw = ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, 0, sl).x;
        ImGui::SetCursorScreenPos(ImVec2(x + bw - sw - S(66), y));
        if (ui::Switch("##prevspin", &spin)) Settings::Get().MarkDirty();
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + bw - sw, y + S(3)), P().textDim, sl);
    }
    y += S(32);
    if (!g.previewEnabled) return y;
    // ask for the selected item, rebuilt whenever its look changes
    std::string file = o.file;
    if (file.empty()) file = FileOf(game::PlayerState(), o.kind, o.index);
    if (file.empty() && o.kind == VehicleKind && o.index >= 0 && o.index < 7) file = kVehicleFiles[o.index];
    if (file.empty() && g.prevMissing != o.Key()) {
        g.prevMissing = o.Key();
        log::Info("Preview", "No model file for {}", o.Key());
    }
    // the job seeds from the scratch slot; a Multi-Tool keeps its seed in its own data, not in the look
    uint8_t seed[16] = {};
    // a previewed seed replaces the look: the seed alone, as applying it would give
    bool seedPreview = g.seedPrevKey == o.Key();
    uint64_t s64 = seedPreview ? g.seedPrev : o.seed;
    memcpy(seed, &s64, 8);
    seed[8] = seedPreview || o.useSeed;
    {
        render::preview::Request r;
        r.key = o.Key();
        r.file = file;
        if (detail && !seedPreview) {
            CustData c = detail->cust;
            if (o.kind == ToolKind) memcpy(c.seed, seed, sizeof seed);
            r.version = PreviewVersion(c);
            int slot = CustSlotFor(o.kind, o.index, file);
            r.fill = [c, slot](void* scratch) mutable {
                if (!F.custApply) return;
                alignas(16) uint8_t raw[0x58] = {};
                ToRaw(c, raw);
                g_ourApply = true;
                F.custApply(scratch, raw, slot, false, c.legacy != 0, c.seed);
                g_ourApply = false;
            };
        }
        else {
            // no look read yet: the seed alone
            r.version = std::hash<std::string>{}(std::string((const char*)seed, sizeof seed));
            r.fill = [s = std::to_array(seed)](void* scratch) { memcpy(scratch, s.data(), s.size()); };
        }
        render::preview::Want(r);
    }
    render::preview::SetScale(g.prevZoom);
    render::preview::SetView(g.prevYaw, g.prevPitch);
    // as tall as the panel allows while leaving room for the tabs and a few editor rows below
    float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - S(62);
    float h = std::clamp(bottom - y - S(260), S(140), bw * 0.62f);
    render::preview::SetAspect(bw / h);
    render::preview::Info info;
    ImTextureID img = render::preview::Image(&info);
    ImVec2 a(x, y), b(x + bw, y + h);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 90));
    if (img) {
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##prevarea", ImVec2(bw, h));
        if (ImGui::IsItemHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0) g.prevZoom = std::clamp(g.prevZoom * (wheel > 0 ? 1.15f : 1 / 1.15f), 0.4f, 8.f);
        }
        ImVec2 dlt = ImGui::GetIO().MouseDelta;
        bool drag = ImGui::IsItemActive();
        if (info.own) {
            if (drag) {
                g.prevYaw += dlt.x * 0.5f;
                g.prevPitch = std::clamp(g.prevPitch + dlt.y * 0.3f, -60.f, 60.f);
            }
            else if (Settings::Get().Data().previewSpin) g.prevYaw += ImGui::GetIO().DeltaTime * 20.f;
            g.prevYaw = std::fmod(g.prevYaw, 360.f);
        }
        // Show the whole target; cropping cut off tall models and turning freighters.
        dl->AddImage(ImTextureRef(img), a, b);
    }
    dl->AddRect(a, b, P().line, 0.f, S(1));
    y += h + S(4);
    std::string line = img ? L::T("gar.preview.note") : info.step;
    if (seedPreview) line = L::Tf("gar.seedprev.on", SeedText(s64)) + "   " + line;
    dl->AddText(ui::theme::Body(), S(13), ImVec2(x, y), P().textFaint, line.c_str(), nullptr);
    return y + S(22);
}

float DrawOwned(ImDrawList* dl, ImVec2 a, float w, const Owned& o, bool snapped)
{
    ui::BrowserEntry head;
    bool isSuitOrVeh = (o.kind == SuitKind || o.kind == VehicleKind);
    std::string type = isSuitOrVeh ? L::T(KindKey(o.kind)) : ModelName(o.file);
    head.title = o.name.empty() ? type : o.name;
    head.subtitle = std::string(L::T(KindKey(o.kind))) + (isSuitOrVeh ? "" : "   /   " + type) + (o.current ? "   /   " + std::string(L::T("gar.current")) : "");
    if (!isSuitOrVeh) {
        head.body.clear(); // the seed is in the Seed field below, so only the slots go here, saving a line
        if (o.kind == ToolKind) head.body += L::Tf("gar.toolslots", std::to_string(o.slots[0]));
        else head.body += L::Tf("gar.slots", std::to_string(o.slots[0]), std::to_string(o.slots[1])) + ", " + L::Tf("gar.cargo", std::to_string(o.slots[2]));
    }
    else {
        head.body = L::T("gar.custom.body");
    }
    head.glyph = KindGlyph(o.kind);
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    bool live = game::FrameHookReady() && F.setString;

    // Reset the fields when the selection changes.
    if (g.editKey != o.Key()) {
        g.editKey = o.Key();
        g.seedPrevKey.clear();
        snprintf(g.seed, sizeof g.seed, "%s", SeedText(o.seed).c_str());
        snprintf(g.name, sizeof g.name, "%s", o.name.c_str());
        g.typeIdx = -1;
        g.clsIdx = o.cls;
        g.colGroup = 0;
        g.stats = o.stats;
        if (isSuitOrVeh && g.subTab == TabGeneral) g.subTab = TabParts;
        if (g.subTab == TabStats && o.stats.empty()) g.subTab = TabGeneral;
        game::Post([k = o.kind, i = o.index] { LoadDetail(k, i); });
    }
    if (!ImGui::IsAnyItemActive() && g.stats.size() != o.stats.size()) g.stats = o.stats;
    std::optional<Detail> detail;
    {
        std::lock_guard lk(g_mx);
        if (g_detail && g_detail->key == o.Key()) detail = g_detail;
    }

    y = DrawPreview(dl, x, y, bw, o, detail);
    const bool show[4] = { !isSuitOrVeh, true, true, !o.stats.empty() };
    y = DrawSubTabs(dl, x, y, &g.subTab, show);

    // The editor scrolls inside the inspector, which ends above the menu's bottom bar.
    float top = y, limit = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - S(62);
    ImGui::SetCursorScreenPos(ImVec2(a.x, top));
    ImGui::BeginChild("##garedit", ImVec2(w, std::max(limit - top, S(120))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    y = ImGui::GetCursorScreenPos().y;
    float y0 = y;

    float half = (bw - S(8)) * 0.5f;
    if ((g.subTab == TabParts || g.subTab == TabPaint) && !detail) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T(F.custOk ? "gar.reading" : "gar.nocustom"));
        y += S(32);
    }
    else if (g.subTab == TabParts) y = DrawParts(dl, x, y, bw, o, *detail, live);
    else if (g.subTab == TabPaint) y = DrawPaint(dl, x, y, bw, o, *detail, live);
    else if (g.subTab == TabStats) y = DrawStatsTab(dl, x, y, bw, o, live);
    else {
        Edit e{ o.kind, o.index };

        // Model and Class side by side
        std::vector<std::string> types;
        std::vector<const char*> paths;
        for (auto& m : kModels) {
            if (m.kind != o.kind) continue;
            if (_stricmp(m.path, o.file.c_str()) == 0) g.typeIdx = (int)types.size();
            types.push_back(L::T(m.key));
            paths.push_back(m.path);
        }
        std::vector<std::string> classes = { "C", "B", "A", "S" };
        g.clsIdx = o.cls;

        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.type"));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + half + S(8), y), P().textDim, L::T("gar.class"));
        y += S(21);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Dropdown("##type", types, &g.typeIdx, half, live) && g.typeIdx >= 0) {
            e.file = paths[g.typeIdx];
            Apply(e);
            e = { o.kind, o.index };
        }
        ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
        if (ui::Dropdown("##cls", classes, &g.clsIdx, half, live)) {
            e.cls = g.clsIdx;
            Apply(e);
            e = { o.kind, o.index };
        }
        y += S(40);

        // Tech rules: which tech the save keeps on load, set from the model unless picked here
        if (o.kind == ShipKind) {
            std::vector<std::string> rules = { L::T("gar.rules.0"), L::T("gar.rules.1"), L::T("gar.rules.2") };
            int ri = o.rules, own = RulesForFile(o.file);
            dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.rules"));
            y += S(21);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (ui::Dropdown("##rules", rules, &ri, half, live) && ri != o.rules) {
                e.rules = ri;
                Apply(e);
                e = { o.kind, o.index };
            }
            if (o.rules != own)
                dl->AddText(ui::theme::Body(), S(13), ImVec2(x + half + S(8), y + S(9)), P().textFaint, L::Tf("gar.rules.model", rules[own]).c_str());
            y += S(40);
        }

        // Seed
        y = Label(dl, x, y, "gar.seedlabel");
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool enter = ui::TextField("##seed", g.seed, sizeof g.seed, bw, "0x0123456789ABCDEF");
        y += S(38);
        auto parsed = ParseSeed(g.seed);
        float third = (bw - S(16)) / 3;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if ((ui::Button("##applyseed", L::T("gar.useseed"), third, ui::Tone::Green, live && parsed.has_value()) || (enter && parsed)) && live) {
            e.seed = *parsed;
            Apply(e);
            e = { o.kind, o.index };
            g.seedPrevKey.clear();
        }
        // look at a pasted seed first, without touching the item
        bool previewing = g.seedPrevKey == o.Key();
        ImGui::SetCursorScreenPos(ImVec2(x + third + S(8), y));
        if (ui::Button("##prevseed", L::T(previewing ? "gar.seedprev.stop" : "gar.seedprev"), third, ui::Tone::Teal, previewing || parsed.has_value())) {
            if (previewing) g.seedPrevKey.clear();
            else { g.seedPrevKey = o.Key(); g.seedPrev = *parsed; g.previewEnabled = true; }
        }
        if (previewing && parsed && *parsed != g.seedPrev) g.seedPrev = *parsed; // follow edits to the field
        ImGui::SetCursorScreenPos(ImVec2(x + 2 * (third + S(8)), y));
        if (ui::Button("##reroll", L::T("gar.reroll"), third, ui::Tone::Teal, live)) {
            uint64_t s = RandomSeed();
            snprintf(g.seed, sizeof g.seed, "%s", SeedText(s).c_str());
            if (previewing) g.seedPrev = s; // reroll the preview, not the item
            else {
                e.seed = s;
                Apply(e);
                e = { o.kind, o.index };
            }
        }
        y += S(42);

        // Name
        y = Label(dl, x, y, "gar.name");
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool nameEnter = ui::TextField("##name", g.name, sizeof g.name, bw - S(108), L::T("gar.noname"));
        ImGui::SetCursorScreenPos(ImVec2(x + bw - S(100), y + S(2)));
        if ((ui::Button("##rename", L::T("gar.rename"), S(100), ui::Tone::Teal, live) || nameEnter) && live) {
            e.rename = true;
            e.name = g.name;
            Apply(e);
        }
        y += S(42);
    }

    if (snapped) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##revert", L::T("gar.revert"), half, ui::Tone::Red, true)) { Revert(o.kind, o.index, false); g.editKey.clear(); }
        ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
        if (ui::Button("##keep", L::T("gar.keep"), half, ui::Tone::Neutral, true)) Revert(o.kind, o.index, true);
        y += S(42);
    }
    const char* note = L::T(o.kind == ShipKind ? (o.current ? "gar.note.ship" : "gar.note.stored") : o.kind == ToolKind ? "gar.note.tool"
                            : o.kind == FreighterKind ? "gar.note.freighter" : o.kind == SuitKind ? "gar.note.suit" : "gar.note.vehicle");
    dl->AddText(ui::theme::Body(), S(13), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(13), FLT_MAX, bw, note).y + pad;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Dummy(ImVec2(1, 1));
    float used = y - y0;
    ImGui::EndChild();
    return std::min(top + used, limit);
}

void Notify(std::string title, std::string body, unsigned glyph)
{
    Registry::Get().Post([title, body, glyph] { ui::Toast(title, body, glyph); });
}

// Game thread. Puts a new ship or Multi-Tool into the first free slot of the loaded save.
// The new item starts from a copy of the current one's inventories, then gets the draft's model, seed, class and name.
void AddDraft(Kind kind, std::string file, uint64_t seed, int cls, std::string name)
{
    uintptr_t ps = game::PlayerState();
    if (!ps || !F.setString) return;
    int slot = -1;
    if (kind == ShipKind) {
        for (int i = 0; i < 12 && slot < 0; ++i) if (ReadVarString(ShipRes(ps, i) + F.resFile).empty()) slot = i;
        int32_t from = Primary();
        if (slot < 0 || from < 0 || !F.invCopy) { Notify(L::T("gar.add.full"), L::T("gar.chip.ship"), icon::Warning); return; }
        for (int k = 0; k < 3; ++k) F.invCopy((void*)ShipStore(ps, slot, k), (void*)ShipStore(ps, from, k));
    }
    else {
        for (int i = 0; i < 6 && slot < 0; ++i) {
            int32_t elems = 0;
            if (uintptr_t s = ToolSlot(i); s && Rd(s + 0x8C, elems) && elems <= 0) slot = i;
        }
        int from = ToolActive();
        if (slot < 0 || from < 0 || !F.toolSet) { Notify(L::T("gar.add.full"), L::T("gar.chip.tool"), icon::Warning); return; }
        auto buf = ToolData(from);
        if (buf.size() < F.toolSize) return;
        F.toolSet((void*)ToolOwn(), slot, buf.data()); // a copy of the held tool first, so the slot has a store
    }
    log::Info("Garage", "Adding {} to slot {}: {} seed {}", kind == ShipKind ? "ship" : "Multi-Tool", slot, file, SeedText(seed));
    Edit e{ kind, slot };
    e.file = file;
    e.seed = seed;
    e.cls = cls;
    e.rename = true;
    e.name = name;
    Apply(e);
    // a ship is only built while it is the current one; make it current so the game builds it
    if (kind == ShipKind && F.setPrimary)
        game::Post([slot] {
            if (uintptr_t p = game::PlayerState()) F.setPrimary((void*)p, slot);
            RetryRefreshShip();
        });
    g.draft.addedSlot = slot;
    Notify(L::T("gar.add.done"), name.empty() ? ModelName(file) : name, icon::Check);
}

// The Add to Garage page: a draft that only exists in the preview until it is added.
float DrawDraft(ImDrawList* dl, ImVec2 a, float w, const std::vector<Owned>& owned)
{
    ui::BrowserEntry head;
    head.title = L::T("gar.add.title");
    head.subtitle = L::T("gar.add.sub");
    head.glyph = icon::Plus;
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad, half = (bw - S(8)) * 0.5f;
    bool live = game::FrameHookReady() && F.setString;
    auto& d = g.draft;

    // kind and model
    std::vector<std::string> kinds = { L::T("gar.chip.ship"), L::T("gar.chip.tool") };
    int ki = d.kind == ToolKind ? 1 : 0;
    std::vector<std::string> types;
    std::vector<const char*> paths;
    int ti = -1;
    for (auto& m : kModels) {
        if (m.kind != d.kind) continue;
        if (assets::paks::Ready() && !assets::paks::Exists(m.path)) continue;
        if (d.file == m.path) ti = (int)types.size();
        types.push_back(L::T(m.key));
        paths.push_back(m.path);
    }
    if (ti < 0 && !paths.empty()) { ti = 0; d.file = paths[0]; }
    if (d.seedText[0] == 0) { d.seed = RandomSeed(); snprintf(d.seedText, sizeof d.seedText, "%s", SeedText(d.seed).c_str()); }

    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("gar.add.kind"));
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x + half + S(8), y), P().textDim, L::T("gar.type"));
    y += S(21);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Dropdown("##dkind", kinds, &ki, half)) { d.kind = ki ? ToolKind : ShipKind; d.file.clear(); }
    ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
    if (ui::Dropdown("##dtype", types, &ti, half) && ti >= 0) d.file = paths[ti];
    y += S(42);

    // the draft as a pretend item, so the preview and the part finder treat it like an owned one
    Owned o;
    o.kind = d.kind;
    o.index = -1;
    o.file = d.file;
    o.seed = d.seed;
    o.useSeed = true;
    o.cls = d.cls;
    y = DrawPreview(dl, x, y, bw, o, std::nullopt);

    // the rest scrolls inside the inspector, as on owned items, so the panel never runs past the menu
    float top = y, limit = ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - S(62);
    ImGui::SetCursorScreenPos(ImVec2(a.x, top));
    ImGui::BeginChild("##gardraft", ImVec2(w, std::max(limit - top, S(120))), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    dl = ImGui::GetWindowDrawList();
    y = ImGui::GetCursorScreenPos().y;
    float y0 = y;

    // seed: paste one from the community, or roll one
    y = Label(dl, x, y, "gar.seedlabel");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::TextField("##dseed", d.seedText, sizeof d.seedText, bw, "0x0123456789ABCDEF");
    if (auto p = ParseSeed(d.seedText)) d.seed = *p;
    y += S(38);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##dreroll", L::T("gar.reroll"), half, ui::Tone::Teal, true)) {
        d.seed = RandomSeed();
        snprintf(d.seedText, sizeof d.seedText, "%s", SeedText(d.seed).c_str());
    }
    std::vector<std::string> classes = { "C", "B", "A", "S" };
    ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
    ui::Dropdown("##dcls", classes, &d.cls, half);
    y += S(42);
    y = Label(dl, x, y, "gar.name");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::TextField("##dname", d.name, sizeof d.name, bw, L::T("gar.noname"));
    y += S(42);

    // find a seed with the parts you want (the same finder as for owned ships)
    if (d.kind == ShipKind) y = DrawSeedParts(dl, x, y + S(4), bw, o, live);

    int used = 0;
    for (auto& it : owned) used += it.kind == d.kind;
    int cap = d.kind == ShipKind ? 12 : 6;
    std::string room = L::Tf("gar.add.room", std::to_string(used), std::to_string(cap));
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, room.c_str());
    y += S(24);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (live && used < cap && !d.file.empty() && ui::HoldButton("##dadd", L::T("gar.add.go"), bw)) {
        game::BackupSaveOnce();
        game::Post([k = d.kind, f = d.file, s = d.seed, c = d.cls, n = std::string(d.name)] { AddDraft(k, f, s, c, n); });
    }
    y += S(46);
    const char* note = L::T(d.kind == ShipKind ? "gar.add.note.ship" : "gar.add.note.tool");
    dl->AddText(ui::theme::Body(), S(13), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(13), FLT_MAX, bw, note).y + pad;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ImGui::Dummy(ImVec2(1, 1));
    float used2 = y - y0;
    ImGui::EndChild();
    return std::min(top + used2, limit);
}

float DrawInspector(ImVec2 a, float w)
{
    if (!game::PlayerState()) return 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = g.browser.Focused(g.view);
    std::vector<Owned> owned;
    std::set<std::string> snaps;
    {
        std::lock_guard lk(g_mx);
        owned = g_owned;
        snaps = g_snapKeys;
    }
    if (f && f->key == kAddKey) return DrawDraft(dl, a, w, owned);
    if (f && f->chip != kModelChip) {
        for (auto& o : owned) if (o.Key() == f->key) return DrawOwned(dl, a, w, o, snaps.contains(o.Key()));
    }
    // A model, or nothing selected: apply the model to the current item of that kind.
    ui::BrowserEntry head;
    Kind kind = ShipKind;
    if (f) {
        head = *f;
        for (auto& m : kModels) if (f->key == m.path) kind = m.kind;
    }
    else {
        head.title = L::T("tab.garage");
        head.subtitle = L::T("gar.subtitle");
        head.body = L::T("gar.help");
        head.glyph = icon::Garage;
    }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2;
    if (f) {
        const Owned* cur = nullptr;
        for (auto& o : owned) if (o.kind == kind && o.current) cur = &o;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        std::string lbl = L::Tf(kind == ShipKind ? "gar.apply.ship" : kind == ToolKind ? "gar.apply.tool" : "gar.apply.freighter", f->title);
        if (ui::Button("##apply", lbl.c_str(), bw, ui::Tone::Green, cur && game::FrameHookReady() && F.setString)) {
            Edit e{ kind, cur->index };
            e.file = f->key;
            e.seed = RandomSeed();
            Apply(e);
        }
        y += S(46);
    }
    return y;
}

template <class Fn> void Bind(const char* name, Fn& fn)
{
    const Target& t = Loc().Resolve(name);
    if (t.ok) fn = (Fn)Abs(t.func);
    else log::Warn("Garage", "{} missing: {}", name, t.reason);
}

int64_t Off(const char* name, int64_t fb = -1)
{
    const Target& t = Loc().Resolve(name);
    return t.ok && t.offset ? *t.offset : fb;
}

}

void InstallGarage()
{
    Bind("String.Set", F.setString);
    Bind("Multitool.GetData", F.toolGet);
    Bind("Multitool.SetFromData", F.toolSet);
    Bind("Multitool.Switch", F.toolSwitch);
    Bind("Customise.Apply", F.custApply);
    Bind("Customise.Reset", F.custReset);
    Bind("Customise.Export", F.custExport);
    Bind("Customise.GetPalette", F.getPalette);
    Bind("Inventory.GenerateBaseStats", F.genBaseStats);
    Bind("Inventory.GenerateSpecialSlots", F.genSpecialSlots);
    Bind("Inventory.Notify", F.invNotify);
    Bind("Weapon.GetClass", F.weaponGetClass);
    Bind("Inventory.Copy", F.invCopy);
    Bind("PlayerState.SetPrimaryShip", F.setPrimary);
    Bind("Reality.GetTechnology", F.getTech);
    {
        auto get = [](const char* cls, const char* f, uint32_t& out) { if (auto m = meta::Path(cls, f)) out = m->offset; };
        if (auto* ci = meta::Get("cGcTechnology"); ci && ci->size) TF.size = ci->size;
        get("cGcTechnology", "ID", TF.id); get("cGcTechnology", "Category", TF.cat); get("cGcTechnology", "BaseStat", TF.stat);
        get("cGcTechnology", "ChargeAmount", TF.charge); get("cGcTechnology", "Core", TF.core); get("cGcTechnology", "Upgrade", TF.upg);
        get("cGcTechnology", "Procedural", TF.proc); get("cGcTechnology", "RepairTech", TF.rep);
        get("cGcTechnology", "BrokenSlotTech", TF.broken); get("cGcTechnology", "IsTemplate", TF.tmpl);
        if (auto* ci = meta::Get("cGcInventoryElement"); ci && ci->size >= 0x28 && ci->size <= 0x100) TF.elSize = ci->size;
        get("cGcInventoryElement", "Id", TF.elId); get("cGcInventoryElement", "Amount", TF.elAmount);
        get("cGcInventoryElement", "MaxAmount", TF.elMax); get("cGcInventoryElement", "Type", TF.elType);
    }
    if (const Target& t = Loc().Resolve("Descriptor.Generate"); t.ok) {
        // Generate -> GenerateInstanceFromModel (its call) -> GetModelDescriptorList (that one's first call)
        std::optional<uint32_t> fromModel;
        for (auto in = Dec(t.func); in && !fromModel; in = Next(*in)) {
            if (in->isCall && in->branchTarget) fromModel = in->branchTarget;
            if (in->mnemonic == "ret") break;
        }
        for (auto in = fromModel ? Dec(*fromModel) : std::nullopt; in; in = Next(*in))
            if (in->isCall && in->branchTarget) { F.modelList = reinterpret_cast<decltype(F.modelList)>(Abs(*in->branchTarget)); break; }
        if (F.modelList) F.genDesc = reinterpret_cast<decltype(F.genDesc)>(Abs(t.func));
    }
    log::Info("Garage", "Living ship rolls: {}", F.genDesc != nullptr);

    kShipOwnership = (uint32_t)Off("Ship.Ownership", kShipOwnership);
    kToolOwnership = (uint32_t)Off("HUD.ToolThumbnailBase", kToolOwnership);
    if (int64_t p = Off("Player.InstanceOffset"); p > 0) kPlayerWeapon = (uint32_t)p + 0x3E0; // cGcPlayerWeapon
    F.primary = Off("Ship.PrimaryIndex");
    F.refresh = Off("Ship.RefreshFlag");
    F.active = Off("Multitool.ActiveIndex");
    F.frRefresh = Off("Freighter.RefreshFlag", 0x4B0);
    F.cls = Off("Inventory.ClassField", 0x100);
    F.baseStats = Off("Inventory.BaseStats", 0xD0);
    F.slotBase = Off("Customise.SlotBase", 0x1CA70);
    F.slotStride = Off("Customise.SlotStride", 0x3AE0);
    F.slotEdited = Off("Customise.SlotEditedFlag", 0x71);
    F.slotEditCol = Off("Customise.SlotEditColours", 0x90);
    F.slotFinalCol = Off("Customise.SlotFinalColours", 0x1D70);
    F.slotDescs = Off("Customise.SlotDescriptors", 0x3A70);
    F.descGroups = Off("Customise.DescriptorGroups", 0xCA560);
    F.playerModel = Off("Customise.PlayerModelPtr", 0x72CF28);
    F.playerRefresh = Off("Customise.PlayerRefreshFlag", 0x15C);
    F.vehicleRefresh = Off("Customise.VehicleRefreshFlag", 0x72F810);

    if (auto* ci = meta::Get("cGcResourceElement")) F.resSize = ci->size;
    if (auto m = meta::Path("cGcResourceElement", "Filename")) F.resFile = m->offset;
    if (auto m = meta::Path("cGcResourceElement", "Seed")) F.resSeed = m->offset;
    if (auto* ci = meta::Get("cGcMultitoolData")) F.toolSize = ci->size;
    if (auto m = meta::Path("cGcMultitoolData", "Resource")) F.toolRes = m->offset;
    if (auto m = meta::Path("cGcMultitoolData", "Seed")) F.toolSeed = m->offset;
    if (auto m = meta::Path("cGcMultitoolData", "Name")) F.toolName = m->offset;
    if (auto m = meta::Path("cGcMultitoolData", "CustomisationData")) F.toolCust = m->offset;
    if (auto m = meta::Path("cGcCharacterCustomisationSaveData", "CustomData")) F.saveCustData = m->offset;

    auto off = [](const char* cls, const char* field, uint32_t& out) { if (auto m = meta::Path(cls, field)) out = m->offset; };
    off("cGcCustomisationDescriptorGroups", "DescriptorGroupSets", F.dgSets);
    if (auto* ci = meta::Get("cGcCustomisationDescriptorGroupSet")) F.setSize = ci->size;
    off("cGcCustomisationDescriptorGroupSet", "DescriptorGroups", F.setGroups);
    off("cGcCustomisationDescriptorGroupSet", "Id", F.setId);
    off("cGcCustomisationDescriptorGroupSet", "RequiresGroup", F.setRequires);
    off("cGcCustomisationDescriptorGroupSet", "GroupsAreMutuallyExclusive", F.setExclusive);
    if (auto* ci = meta::Get("cGcCustomisationDescriptorGroup")) F.groupSize = ci->size;
    off("cGcCustomisationDescriptorGroup", "Title", F.groupTitle);
    off("cGcCustomisationDescriptorGroup", "GroupID", F.groupId);
    off("cGcCustomisationDescriptorGroup", "HiddenInCustomiser", F.groupHidden);
    // The look is copied with fixed offsets; stop if the game changed them.
    if (auto* ci = meta::Get("cGcCharacterCustomisationData")) {
        auto at = [&](const char* n) { auto* m = ci->Find(n); return m ? (int)m->offset : -1; };
        F.custOk = ci->size == 0x58 && at("BoneScales") == 0 && at("Colours") == 0x10 && at("DescriptorGroups") == 0x20 &&
                   at("PaletteID") == 0x30 && at("TextureOptions") == 0x40 && at("Scale") == 0x50;
        if (!F.custOk) log::Warn("Garage", "Customisation layout changed, parts and paint are off");
    }

    if (const Target& t = Loc().Resolve("Customise.ApplySnap"); t.ok) {
        if (auto* h = Mid(t.site, [](Context& c) { if (g_ourApply || g_keepExact) c.r10 = 0; }); h && h->enable()) {
            Bound b;
            b.id = "cust.exact_colours";
            b.toggle = [](bool on) {
                g_keepExact = on;
                // the hook only acts when the game applies a look, so apply the current ship and tool once now
                // only once a save is in play; at startup the saved toggle state is restored before any ship exists
                if (on && game::FrameHookReady() && game::catalog::Ready())
                    game::Post([] {
                        uintptr_t ps = game::PlayerState();
                        if (!ps) return;
                        for (auto [kind, idx] : { std::pair{ ShipKind, (int)Primary() }, std::pair{ ToolKind, (int)ToolActive() } })
                            if (idx >= 0)
                                if (auto c = ReadCust(ps, kind, idx)) WriteCust(ps, kind, idx, std::move(*c));
                    });
                return true;
            };
            Ok(std::move(b));
        }
        else Fail("cust.exact_colours", "hook could not be placed");
    }
    else Fail("cust.exact_colours", t.reason);
    if (const Target& t = Loc().Resolve("PlayerState.ResetCaches"); t.ok) {
        // lea rax,[rdi+base]; lea rcx,[rax+size]; ...; mov byte [rax+flag],1; add rax,stride
        CacheGroup cur{};
        for (auto& r : Img().FuncRanges(t.func))
            for (auto& in : Img().DecodeRange(r.first, r.second)) {
                if (in.ops.size() != 2) continue;
                auto& a = in.ops[0];
                auto& b = in.ops[1];
                if (in.mnemonic == "lea" && b.kind == hooks::Operand::Mem && b.base == ZYDIS_REGISTER_RDI && a.reg == ZYDIS_REGISTER_RAX) cur = { (uint32_t)b.disp, 0, 0, 0 };
                else if (in.mnemonic == "lea" && b.kind == hooks::Operand::Mem && b.base == ZYDIS_REGISTER_RAX && a.reg == ZYDIS_REGISTER_RCX && cur.base) cur.size = (uint32_t)b.disp;
                else if (in.mnemonic == "mov" && a.kind == hooks::Operand::Mem && a.base == ZYDIS_REGISTER_RAX && b.kind == hooks::Operand::Imm && b.imm == 1 && cur.size) cur.flag = (uint32_t)a.disp;
                else if (in.mnemonic == "add" && a.reg == ZYDIS_REGISTER_RAX && b.kind == hooks::Operand::Imm && cur.flag) {
                    cur.stride = (uint32_t)b.imm;
                    if (cur.stride && cur.size / cur.stride <= 64) g_statCache.push_back(cur);
                    cur = {};
                }
            }
        log::Info("Garage", "{} stat cache groups", g_statCache.size());
    }
    log::Info("Garage", "Ready: primary +0x{:X}, refresh +0x{:X}, active +0x{:X}, freighter refresh +0x{:X}, seed +0x{:X}, parts +0x{:X}, export {}",
              F.primary, F.refresh, F.active, F.frRefresh, F.resSeed, F.descGroups, F.custExport != nullptr);

    Registry::Get().Post([] {
        g.browser.chips = {
            L::T("gar.chip.all"), L::T("gar.chip.ship"), L::T("gar.chip.tool"), L::T("gar.chip.freighter"),
            L::T("gar.chip.suit"), L::T("gar.chip.vehicle"), L::T("gar.chip.models")
        };
        g.browser.hideFromAll = kModelChip; // the model list is a catalogue, not something this save owns
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { std::lock_guard lk(g_mx); return std::to_string(g_owned.size()); };
        page.inspectorWidth = 620; // room for the preview and its editor
        ui::RegisterPage("garage", std::move(page));
    });
}

}
