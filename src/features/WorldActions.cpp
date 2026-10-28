#include "features/WorldActions.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/Catalog.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <random>

// Missions, signals (POI finder), fleet and companion edits, free shop and gifts.
namespace os::features {

namespace {

namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

struct Fns
{
    void* (*missionCreate)(const char*, void*, const void*, bool) = nullptr;
    void (*missionAdd)(void*, void*) = nullptr;
    void (*missionSetCurrent)(void*, void*, bool) = nullptr;
    void (*missionComplete)(void*) = nullptr;
    void* (*scanFind)(void*, const char*, int) = nullptr;
    void (*scanStart)(void*, void*, int, const void*, void*, bool, float, void*, int, bool, bool, bool, const void*, bool) = nullptr;
    void (*petRefresh)(void*) = nullptr;
    int64_t missionMgr = -1, seasonMgr = -1, scanSlot = -1, fleet = -1, currency = -1, craftFree = -1, startTime = 0xA0;
    int64_t stats = 0x104, damage = 0x130, counters = 0xFC;
    int64_t petTreats = 0x2420, petOverrides = 0x2408, petUseOverrides = 0x2404, petTraits = 0x23E0, petScale = 0x23F4, petRefreshFlag = 0xE10C0;
    int frigateMax = 0;
} F;

template <class Fn> void BindFn(const char* name, Fn& fn)
{
    const Target& t = Loc().Resolve(name);
    if (t.ok) fn = (Fn)Abs(t.func);
    else log::Warn("World", "{} missing: {}", name, t.reason);
}

int64_t Off(const char* name, int64_t fb = -1)
{
    const Target& t = Loc().Resolve(name);
    return t.ok && t.offset ? *t.offset : fb;
}

std::string Fixed(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return s;
}

std::string Var(uintptr_t at)
{
    uintptr_t p = 0;
    return Rd(at, p) && p ? Fixed(p, 256) : std::string();
}

std::string Tr(const std::string& key)
{
    if (key.empty()) return key;
    std::string k = key[0] == '^' ? key.substr(1) : key;
    return game::catalog::Translate(k);
}

uint64_t Rand() { static std::mt19937_64 r{ std::random_device{}() }; return r(); }

// ---- difficulty overrides: free shop, free building ----
// Restored around saves so the save keeps the player's real difficulty.

struct Pin { int64_t off = -1; int size = 4; int64_t want = 0, orig = 0; bool on = false; };
Pin g_shop{ -1, 4, 0 }, g_build{ -1, 1, 1 };
safetyhook::InlineHook g_save;

int64_t ReadPin(const Pin& p, uintptr_t d)
{
    if (p.size == 1) { uint8_t v = 0; Rd(d + p.off, v); return v; }
    int32_t v = 0; Rd(d + p.off, v); return v;
}

void WritePin(const Pin& p, uintptr_t d, int64_t v)
{
    if (p.size == 1) Wr<uint8_t>(d + p.off, (uint8_t)v);
    else Wr<int32_t>(d + p.off, (int32_t)v);
}

void PinTick(Pin& p)
{
    uintptr_t d = game::Data();
    if (!p.on || !d) return;
    int64_t cur = ReadPin(p, d);
    if (cur == p.want) return;
    p.orig = cur; // a save was loaded
    WritePin(p, d, p.want);
}

// The "Free" purchase option's row in the difficulty config (cGcDifficultyCurrencyCostOptionData).
// The game only zeroes a currency price when that currency's FreeCostTypes flag is set, and cost
// manager and interaction costs have flags of their own, so Free Shops sets all of them.
struct
{
    uintptr_t slot = 0; // global holding the config pointer
    int64_t index = 0;  // row index of option 0
    uint32_t multipliers = 0, freeTypes = 16, managerFree = 19, interactFree = 20;
    uint8_t saved[24]{};
    uintptr_t at = 0; // row we changed, 0 when untouched
} g_freeRow;

uintptr_t FreeRow()
{
    uintptr_t cfg = 0;
    return g_freeRow.slot && Rd(g_freeRow.slot, cfg) && cfg ? cfg + (uintptr_t)g_freeRow.index * 24 : 0;
}

void SetFreeRow(bool on)
{
    auto& r = g_freeRow;
    if (!on) {
        if (r.at) for (int i = 0; i < 24; ++i) Wr<uint8_t>(r.at + i, r.saved[i]);
        r.at = 0;
        return;
    }
    uintptr_t row = FreeRow();
    if (!row) return;
    if (r.at != row) { // first time, or the config was reloaded
        for (int i = 0; i < 24; ++i) Rd(row + i, r.saved[i]);
        if (!r.at) {
            uint8_t t[3] = {};
            for (int k = 0; k < 3; ++k) Rd(row + r.freeTypes + k, t[k]);
            uint8_t m = 0, n = 0;
            Rd(row + r.managerFree, m);
            Rd(row + r.interactFree, n);
            log::Info("World", "Free purchase option as shipped: units {}, nanites {}, quicksilver {}, cost manager {}, interactions {}", t[0], t[1], t[2], m, n);
        }
        r.at = row;
    }
    for (int k = 0; k < 3; ++k) {
        Wr<float>(row + r.multipliers + 4 * k, 0.f);
        Wr<uint8_t>(row + r.freeTypes + k, 1);
    }
    Wr<uint8_t>(row + r.managerFree, 1);
    Wr<uint8_t>(row + r.interactFree, 1);
}

uintptr_t SaveHk(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t e)
{
    uintptr_t d = game::Data();
    for (Pin* p : { &g_shop, &g_build }) if (p->on && d) WritePin(*p, d, p->orig);
    uintptr_t r = g_save.call<uintptr_t>(a, b, c, e);
    for (Pin* p : { &g_shop, &g_build }) if (p->on && d) WritePin(*p, d, p->want);
    return r;
}

void BindPin(const char* id, Pin& p)
{
    if (p.off < 0) return Fail(id, "setting not found");
    Bound b;
    b.id = id;
    b.toggle = [&p](bool on) {
        uintptr_t d = game::Data();
        if (!d) return false;
        if (on && !p.on) { p.orig = ReadPin(p, d); p.on = true; WritePin(p, d, p.want); }
        else if (!on && p.on) { p.on = false; WritePin(p, d, p.orig); }
        return true;
    };
    Ok(std::move(b));
}

// ---- gifts and requests ----

void GiftCb(Context& c)
{
    c.rax = 1;
    c.rip = RetGadget();
}

// ---- frigates ----

safetyhook::InlineHook g_stat;
std::atomic<bool> g_maxStats{ false };

int StatHk(uintptr_t frigate, int stat)
{
    int v = g_stat.call<int>(frigate, stat);
    if (!g_maxStats) return v;
    if (stat == 4) return 0;  // fuel burn rate
    if (stat == 5) return v;  // fuel capacity
    return std::max(v, F.frigateMax);
}

uintptr_t Fleet() { uintptr_t d = game::Data(); return d && F.fleet > 0 ? d + F.fleet : 0; }

std::vector<uintptr_t> Frigates()
{
    std::vector<uintptr_t> out;
    uintptr_t fl = Fleet(), arr = 0;
    int32_t n = 0;
    if (!fl || !Rd(fl + 0x3E24, n) || !Rd(fl + 0x3E28, arr) || !arr || n <= 0 || n > 30) return out;
    for (int32_t i = 0; i < n; ++i) { uintptr_t f = 0; if (Rd(arr + i * 8, f) && f) out.push_back(f); }
    return out;
}

void FinishExpeditions()
{
    uintptr_t fl = Fleet(), arr = 0;
    int32_t n = 0;
    if (!fl || !Rd(fl + 0x3E54, n) || !Rd(fl + 0x3E58, arr) || !arr || n <= 0 || n > 64) return;
    int done = 0;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t e = 0;
        uint64_t pause = 1;
        if (!Rd(arr + i * 8, e) || !e || !Rd(e + F.startTime + 8, pause) || pause) continue;
        Wr<uint64_t>(e + F.startTime, 1);
        ++done;
    }
    log::Info("Fleet", "Finished {} frigate expeditions", done);
}

void RepairFrigates()
{
    for (uintptr_t f : Frigates()) Wr<uint64_t>(f + F.damage, 0);
}

// ---- pets ----

constexpr uint32_t kPets = 0xE10D0, kPetStride = 0x24A0, kPetFilled = 0x2370, kPetName = 0x1D00, kPetId = 0x2310;

uintptr_t Pet(int i) { uintptr_t d = game::Data(); return d ? d + kPets + (uintptr_t)i * kPetStride : 0; }

// ---- missions and signals ----

struct Mission
{
    std::string key, id, title, subtitle, table;
    int tableIndex = 0;
    bool active = false;
    bool running = false;
    int activeIndex = -1;
    int stage = -1, stages = 0;
    uint64_t seed = 0;
    bool hasSeed = false;
};

struct Signal { int cls; const char* key; };
const Signal kSignals[] = {
    { 18, "sig.crashedship" }, { 24, "sig.crashedfreighter" }, { 29, "sig.watercrashedfreighter" }, { 17, "sig.ruin" },
    { 27, "sig.treasureruin" }, { 12, "sig.factory" }, { 16, "sig.portal" }, { 15, "sig.monolith" }, { 14, "sig.plaque" },
    { 20, "sig.distress" }, { 3, "sig.abandoned" }, { 2, "sig.shelter" }, { 13, "sig.harvester" }, { 19, "sig.machine" },
    { 36, "sig.settlement" }, { 51, "sig.drones" }, { 53, "sig.robotcamp" }, { 56, "sig.guardian" }, { 61, "sig.scrapyard" },
};

const char* kTables[] = { "Mission", "NPC", "Wiki", "Core", "Tutorial", "AtlasPath", "Recurring", "Fleet", "Water",
                          "Multiplayer", "Corvette", "BaseComputer", "Community", "PlanetProc", "DisablingConditions", "SpacePOI",
                          "SpaceOutpost", "Seasonal", "SeasonalBespoke", "SentinelSettlement", "Pirate", "StatStories", "StartedOnUse",
                          "NPCBuilders", "Swarm", "Deprecated", "Mod" };
// Not startable from the list: multiplayer and corvette need extra state (the game's debug menu skips them too),
// and wiki guides, disabling conditions, lore counters and deprecated entries are not real missions.
bool HiddenTable(int t) { return t == 2 || t == 9 || t == 10 || t == 14 || t == 21 || t == 25 || t == 26; }

std::mutex g_mx;
std::vector<Mission> g_missions;
std::atomic<bool> g_loading{ false };

struct Finish { uintptr_t mission = 0; int stage = -2, waits = 0; } g_finish; // game thread

uintptr_t MissionMgr() { uintptr_t d = game::Data(); return d && F.missionMgr > 0 ? d + F.missionMgr : 0; }

bool IsActiveMissionPtr(uintptr_t want)
{
    if (!want) return false;
    uintptr_t mgr = MissionMgr(), arr = 0;
    int32_t n = 0;
    if (!mgr || !Rd(mgr + 0x1C4, n) || !Rd(mgr + 0x1C8, arr) || !arr || n <= 0 || n > 2000) return false;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t m = 0;
        if (Rd(arr + i * 8, m) && m == want) return true;
    }
    return false;
}

uintptr_t FindActive(const std::string& id, int activeIndex = -1, bool hasSeed = false, uint64_t seed = 0)
{
    uintptr_t mgr = MissionMgr(), arr = 0;
    int32_t n = 0;
    if (!mgr || !Rd(mgr + 0x1C4, n) || !Rd(mgr + 0x1C8, arr) || !arr || n <= 0 || n > 2000) return 0;
    auto check = [&](uintptr_t m, bool matchSeed) -> bool {
        if (!m || Fixed(m + 0x08, 16) != id) return false;
        int32_t st = -1, cnt = 0;
        if (!Rd(m + 0x48, st) || !Rd(m + 0x1C, cnt) || st < 0 || st >= cnt) return false;
        if (matchSeed) {
            uint64_t s = 0;
            uint8_t u = 0;
            if (!Rd(m + 0x100, s) || !Rd(m + 0x108, u) || (u != 0) != hasSeed || (hasSeed && s != seed)) return false;
        }
        return true;
    };
    if (activeIndex >= 0 && activeIndex < n) {
        uintptr_t m = 0;
        if (Rd(arr + (uintptr_t)activeIndex * 8, m) && check(m, true)) return m;
    }
    if (hasSeed) {
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t m = 0;
            if (Rd(arr + (uintptr_t)i * 8, m) && check(m, true)) return m;
        }
    }
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t m = 0;
        if (Rd(arr + (uintptr_t)i * 8, m) && check(m, false)) return m;
    }
    return 0;
}

bool LooksRawKey(const std::string& s, const std::string& id)
{
    if (s.empty() || s == id) return true;
    return s.find('_') != std::string::npos || s.find('%') != std::string::npos;
}

// Looks up an expedition milestone's localized title and phase label from SeasonMgr + 0x66A8 (Stages).
bool FindMilestoneInfo(uintptr_t d, const std::string& id, bool hasSeed, uint64_t seed, std::string& outTitle, std::string& outPhase)
{
    if (!d || F.seasonMgr < 0) return false;
    uint32_t oStages = 0x66A8, stageStride = 0x50, oMilestones = 0x40, msStride = 0x650, oMission = 0x1F0, oTitle = 0x128, oTitleUpper = 0x148;
    if (auto m = meta::Path("cGcSeasonalGameModeData", "Stages")) oStages = m->offset;
    if (auto* ci = meta::Get("cGcSeasonalStage")) stageStride = ci->size;
    if (auto m = meta::Path("cGcSeasonalStage", "Milestones")) oMilestones = m->offset;
    if (auto* ci = meta::Get("cGcSeasonalMilestone")) msStride = ci->size;
    if (auto m = meta::Path("cGcSeasonalMilestone", "Mission")) oMission = m->offset;
    if (auto m = meta::Path("cGcSeasonalMilestone", "Title")) oTitle = m->offset;
    if (auto m = meta::Path("cGcSeasonalMilestone", "TitleUpper")) oTitleUpper = m->offset;

    uintptr_t smgr = d + F.seasonMgr, stagesPtr = 0;
    int32_t stagesCnt = 0;
    if (!Rd(smgr + oStages, stagesPtr) || !Rd(smgr + oStages + 8, stagesCnt) || !stagesPtr || stagesCnt <= 0 || stagesCnt > 32)
        return false;

    auto readMs = [&](int32_t sIdx, int32_t mIdx) -> bool {
        if (sIdx < 0 || sIdx >= stagesCnt || mIdx < 0) return false;
        uintptr_t st = stagesPtr + (uintptr_t)sIdx * stageStride, msPtr = 0;
        int32_t msCnt = 0;
        if (!Rd(st + oMilestones, msPtr) || !Rd(st + oMilestones + 8, msCnt) || !msPtr || mIdx >= msCnt || msCnt > 64) return false;
        uintptr_t ms = msPtr + (uintptr_t)mIdx * msStride;
        if (Fixed(ms + oMission, 16) != id) return false;
        std::string t = Tr(Fixed(ms + oTitle, 32));
        if (LooksRawKey(t, id)) t = Tr(Fixed(ms + oTitleUpper, 32));
        if (!LooksRawKey(t, id)) outTitle = t;
        outPhase = std::to_string(sIdx + 1) + "." + std::to_string(mIdx + 1);
        return !outTitle.empty();
    };

    if (hasSeed && seed < 320 && readMs((int32_t)(seed / 10), (int32_t)(seed % 10))) return true;
    for (int32_t sIdx = 0; sIdx < stagesCnt; ++sIdx) {
        uintptr_t st = stagesPtr + (uintptr_t)sIdx * stageStride, msPtr = 0;
        int32_t msCnt = 0;
        if (!Rd(st + oMilestones, msPtr) || !Rd(st + oMilestones + 8, msCnt) || !msPtr || msCnt <= 0 || msCnt > 64) continue;
        for (int32_t mIdx = 0; mIdx < msCnt; ++mIdx) {
            if (readMs(sIdx, mIdx)) return true;
        }
    }
    return false;
}

void LoadMissions()
{
    uintptr_t d = game::Data();
    if (!d) { g_loading = false; return; }
    uint32_t stride = 0x4D0, oId = 0x3C8, oTitle = 0x360, oSub = 0x378, oPageHint = 0x3A0, oObj = 0x490, oStages = 0x408;
    if (auto* ci = meta::Get("cGcGenericMissionSequence")) stride = ci->size;
    if (auto m = meta::Path("cGcGenericMissionSequence", "MissionID")) oId = m->offset;
    if (auto m = meta::Path("cGcGenericMissionSequence", "MissionTitles")) oTitle = m->offset;
    if (auto m = meta::Path("cGcGenericMissionSequence", "MissionSubtitles")) oSub = m->offset;
    if (auto m = meta::Path("cGcGenericMissionSequence", "MissionPageHint")) oPageHint = m->offset;
    if (auto m = meta::Path("cGcGenericMissionSequence", "MissionObjective")) oObj = m->offset;
    if (auto m = meta::Path("cGcGenericMissionSequence", "Stages")) oStages = m->offset;

    // Missions in progress, from MissionMgr + 0x1C8.
    struct ActiveRaw
    {
        uintptr_t ptr = 0;
        int index = -1;
        std::string id, vtitle;
        int stage = 0, stages = 0;
        uint64_t seed = 0;
        bool hasSeed = false;
    };
    std::vector<ActiveRaw> activeRaws;
    uintptr_t mgr = MissionMgr(), arr = 0;
    int32_t activeCap = 0;
    if (mgr && Rd(mgr + 0x1C4, activeCap) && Rd(mgr + 0x1C8, arr) && arr && activeCap > 0 && activeCap <= 2000) {
        for (int32_t i = 0; i < activeCap; ++i) {
            uintptr_t a = 0;
            int32_t st = -1, cnt = 0;
            if (!Rd(arr + (uintptr_t)i * 8, a) || !a || !Rd(a + 0x48, st) || !Rd(a + 0x1C, cnt) || st < 0 || st >= cnt)
                continue;
            std::string id = Fixed(a + 0x08, 16);
            if (id.empty()) continue;
            ActiveRaw ar;
            ar.ptr = a;
            ar.index = i;
            ar.id = id;
            ar.stage = st;
            ar.stages = cnt;
            uint8_t useSeed = 0;
            Rd(a + 0x100, ar.seed);
            Rd(a + 0x108, useSeed);
            ar.hasSeed = useSeed != 0;

            uintptr_t vt = 0, fmtFn = 0;
            if (Rd(a, vt) && vt && Rd(vt + 0x20, fmtFn) && fmtFn) {
                char buf[0x80] = {};
                ((void (*)(uintptr_t, char*))fmtFn)(a, buf);
                buf[sizeof(buf) - 1] = 0;
                ar.vtitle = buf;
            }
            if (ar.vtitle.empty()) ar.vtitle = Fixed(a + 0x430, 0x80);
            activeRaws.push_back(std::move(ar));
        }
    }

    // The 27 static tables.
    std::vector<Mission> staticMissions;
    for (int t = 0; t < 27; ++t) {
        if (HiddenTable(t)) continue;
        uintptr_t table = 0, data = 0;
        int32_t n = 0;
        if (!Rd(d + 0x130 + t * 8, table) || !table || !Rd(table, data) || !Rd(table + 8, n) || !data || n <= 0 || n > 5000) continue;
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t e = data + (uintptr_t)i * stride;
            Mission m;
            m.id = Fixed(e + oId, 16);
            if (m.id.empty()) continue;
            m.key = "TBL:" + std::to_string(t) + ":" + std::to_string(i) + ":" + m.id;
            int32_t stageCount = 0;
            if (!Rd(e + oStages + 8, stageCount) || stageCount <= 0) continue;
            std::string fmt = Var(e + oTitle);
            int32_t cnt = 0;
            Rd(e + oTitle + 0x10, cnt);
            if (cnt > 0 && fmt.find('%') != std::string::npos) {
                char b[128];
                snprintf(b, sizeof b, fmt.c_str(), 1);
                fmt = b;
            }
            // A real mission has a log title or is an expedition milestone; the rest are hints and trackers.
            m.title = fmt.empty() ? std::string() : Tr(fmt);
            if (LooksRawKey(m.title, m.id)) {
                std::string msTitle, msPhase;
                m.title = FindMilestoneInfo(d, m.id, false, 0, msTitle, msPhase) ? msTitle : std::string();
            }
            if (LooksRawKey(m.title, m.id)) continue;
            m.table = kTables[t];
            m.tableIndex = t;
            m.subtitle = m.table + "   /   " + m.id;
            for (auto& ar : activeRaws) {
                if (ar.id == m.id) {
                    m.running = true;
                    m.activeIndex = ar.index;
                    m.stage = ar.stage;
                    m.stages = ar.stages;
                    m.seed = ar.seed;
                    m.hasSeed = ar.hasSeed;
                    break;
                }
            }
            staticMissions.push_back(std::move(m));
        }
    }

    // One Active entry per running mission, so expedition milestones that share an id all show.
    std::vector<Mission> out;
    out.reserve(activeRaws.size() + staticMissions.size());
    for (auto& ar : activeRaws) {
        Mission m;
        m.key = "ACT:" + std::to_string(ar.index) + ":" + ar.id + ":" + std::to_string(ar.seed);
        m.id = ar.id;
        m.active = true;
        m.running = true;
        m.activeIndex = ar.index;
        m.stage = ar.stage;
        m.stages = ar.stages;
        m.seed = ar.seed;
        m.hasSeed = ar.hasSeed;
        m.table = "Active";

        const Mission* staticMatch = nullptr;
        for (auto& sm : staticMissions) {
            if (sm.id == ar.id) {
                staticMatch = &sm;
                if (sm.tableIndex == 17 || sm.tableIndex == 18) break;
            }
        }
        if (staticMatch) {
            m.table = staticMatch->table;
            m.tableIndex = staticMatch->tableIndex;
        }

        std::string msTitle, msPhase;
        bool isMs = FindMilestoneInfo(d, ar.id, ar.hasSeed, ar.seed, msTitle, msPhase);
        if (isMs && !msTitle.empty()) {
            m.title = msTitle;
        }
        else if (!LooksRawKey(ar.vtitle, ar.id)) {
            m.title = ar.vtitle;
        }
        else if (staticMatch && !LooksRawKey(staticMatch->title, ar.id)) {
            m.title = staticMatch->title;
        }
        else continue; // hints and trackers the game runs without a log entry

        m.subtitle = m.table;
        if (!msPhase.empty()) m.subtitle += " " + msPhase;
        m.subtitle += "   /   " + m.id + "   /   " + L::Tf("mis.stage", std::to_string(m.stage + 1), std::to_string(m.stages));
        out.push_back(std::move(m));
    }

    for (auto& sm : staticMissions) out.push_back(std::move(sm));

    std::lock_guard lk(g_mx);
    g_missions = std::move(out);
    g_loading = false;
}

void StartMission(std::string id, int table)
{
    game::BackupSaveOnce();
    game::Post([id, table] {
        if (!F.missionCreate || !F.missionAdd || FindActive(id)) return;
        char key[16] = {};
        memcpy(key, id.data(), std::min<size_t>(id.size(), 16));
        struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } seed{ 0, 1, {} };
        if (table == 1 || table == 13 || table == 16) seed.v = Rand();
        void* m = F.missionCreate(key, nullptr, &seed, false);
        if (!m) { log::Warn("Missions", "Unknown mission {}", id); return; }
        F.missionAdd((void*)MissionMgr(), m);
        if (F.missionSetCurrent) F.missionSetCurrent((void*)MissionMgr(), m, false);
        log::Info("Missions", "Started {}", id);
        LoadMissions();
    });
}

// Ends the mission the way its own update does: index past the last stage, then the completion handler.
void CompleteMission(uintptr_t m)
{
    int32_t count = 0;
    if (!Rd(m + 0x1C, count)) return;
    Wr<int32_t>(m + 0x48, count);
    Wr<uint64_t>(m + 0x38, 0);
    Wr<uint8_t>(m + 0xF8, 0);
    if (F.missionComplete) F.missionComplete((void*)m);
}

// Moves one stage forward the way the mission's update does: the game begins the new stage on its next update.
// Returns false once the mission is complete.
bool StepMission(uintptr_t m)
{
    int32_t stage = 0, count = 0;
    uintptr_t stages = 0, s = 0;
    if (!Rd(m + 0x48, stage) || !Rd(m + 0x1C, count) || !Rd(m + 0x20, stages) || !stages) return false;
    int32_t next = stage + 1;
    if (next >= count) { CompleteMission(m); return false; }
    if (Rd(stages + (uintptr_t)next * 8, s) && s) Wr<uint8_t>(s + 0x6C8, 0);
    Wr<int32_t>(m + 0x48, next);
    Wr<uint64_t>(m + 0x38, 0);
    Wr<uint8_t>(m + 0xF8, 1); // begin pending
    return true;
}

void FindSignal(int cls)
{
    game::Post([cls] {
        uintptr_t d = game::Data(), mgr = 0;
        if (!d || F.scanSlot < 0 || !Rd(d + F.scanSlot, mgr) || !mgr || !F.scanStart) return;
        int32_t where = 0;
        if (Rd(d + 0x57A584, where) && where == 0xE) { Registry::Get().Post([] { ui::Toast(L::T("sig.noplanet")); }); return; }
        uint32_t size = 0x4D0, oSearch = 0x488, oClass = 0x4C4, oLoc = 0x448, oName = 0x228;
        if (auto* ci = meta::Get("cGcScanEventData")) size = ci->size;
        if (auto m = meta::Path("cGcScanEventData", "SearchType")) oSearch = m->offset;
        if (auto m = meta::Path("cGcScanEventData", "SpecificBuildingClass")) oClass = m->offset;
        if (auto m = meta::Path("cGcScanEventData", "BuildingLocation")) oLoc = m->offset;
        if (auto m = meta::Path("cGcScanEventData", "Name")) oName = m->offset;
        // Planet table: an event that searches for this building class, else a copy of any building search.
        uintptr_t table = 0, data = 0, pick = 0, any = 0;
        int32_t n = 0;
        if (!Rd(mgr + 0x18 + 1 * 8, table) || !table || !Rd(table, data) || !Rd(table + 8, n) || !data || n <= 0) return;
        for (int32_t i = 0; i < n && !pick; ++i) {
            uintptr_t e = data + (uintptr_t)i * size;
            int32_t st = -1;
            uint8_t c = 0;
            if (!Rd(e + oSearch, st) || st != 3) continue;
            if (!any) any = e;
            if (Rd(e + oClass, c) && c == cls) pick = e;
        }
        if (!pick && any) {
            static std::vector<std::unique_ptr<uint8_t[]>> clones; // the manager keeps the pointer
            clones.emplace_back(new uint8_t[size]);
            uint8_t* c = clones.back().get();
            memcpy(c, (void*)any, size);
            Wr<uint8_t>((uintptr_t)c + oClass, (uint8_t)cls);
            Wr<int32_t>((uintptr_t)c + oLoc, 7); // NearestUnmarked
            char name[32] = {};
            snprintf(name, sizeof name, "OS_SIGNAL_%d_%zu", cls, clones.size());
            memcpy(c + oName, name, 32);
            pick = (uintptr_t)c;
        }
        if (!pick) return;
        struct { void* ev; int32_t status; } out{};
        struct alignas(8) { uint64_t v; uint8_t use; uint8_t pad[7]; } seed{ 0, 1, {} };
        F.scanStart((void*)mgr, &out, 1, (void*)pick, nullptr, false, 0.f, nullptr, 0, false, true, false, &seed, false);
        log::Info("Signals", "Scan for class {}: status {}", cls, out.status);
        if (!out.ev) Registry::Get().Post([] { ui::Toast(L::T("sig.none")); });
    });
}

// ---- UI: missions page ----

struct MissionUi
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    size_t built = SIZE_MAX;
} gm;

void MissionList(const char* query)
{
    if (!game::PlayerState()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T("items.ingame"));
        return;
    }
    static int tick = 0;
    if ((gm.built == SIZE_MAX || ++tick % 45 == 1) && !g_loading && game::FrameHookReady()) {
        g_loading = true;
        if (gm.built == SIZE_MAX) gm.built = SIZE_MAX - 1;
        game::Post(LoadMissions);
    }
    std::vector<Mission> list;
    {
        std::lock_guard lk(g_mx);
        list = g_missions;
    }
    size_t sig = list.size();
    for (auto& m : list) {
        sig = sig * 131 + (m.active ? 19 : 0) + (m.running ? 7 : 0) + (size_t)(m.stage + 2) * 31 + (size_t)m.activeIndex;
    }
    if (sig != gm.built && !list.empty()) {
        gm.built = sig;
        gm.view.clear();
        for (auto& s : kSignals) {
            ui::BrowserEntry b;
            b.key = "SIG:" + std::to_string(s.cls);
            b.title = L::T(s.key);
            b.subtitle = L::T("sig.subtitle");
            b.glyph = icon::Satellite;
            b.chip = 3;
            gm.view.push_back(std::move(b));
        }
        for (auto& m : list) {
            ui::BrowserEntry b;
            b.key = m.key;
            b.title = m.title;
            b.subtitle = m.subtitle;
            b.glyph = icon::Scroll;
            b.marked = m.running;
            b.chip = m.active ? 2 : 1;
            gm.view.push_back(std::move(b));
        }
        gm.browser.Invalidate();
    }
    gm.browser.Draw(gm.view, query);
}

float MissionInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = gm.browser.Focused(gm.view);
    ui::BrowserEntry head;
    if (f) head = *f;
    else { head.title = L::T("tab.missions"); head.subtitle = L::T("mis.subtitle"); head.body = L::T("mis.help"); head.glyph = icon::Scroll; }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    bool live = game::FrameHookReady();
    if (f && f->key.starts_with("SIG:")) {
        int cls = std::stoi(f->key.substr(4));
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##sig", L::T("sig.find"), bw, ui::Tone::Green, live && F.scanStart)) { FindSignal(cls); ui::Toast(L::T("sig.started"), f->title, icon::Satellite); }
        y += S(46);
    }
    else if (f) {
        Mission m;
        {
            std::lock_guard lk(g_mx);
            for (auto& x2 : g_missions) if (x2.key == f->key) { m = x2; break; }
        }
        auto refresh = [] { if (!g_loading.exchange(true)) game::Post(LoadMissions); };
        if (!m.running) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (ui::Button("##start", L::T("mis.start"), bw, ui::Tone::Green, live && F.missionCreate && !m.id.empty())) StartMission(m.id, m.tableIndex);
            y += S(46);
        }
        else {
            std::string st = m.stage >= m.stages ? L::T("mis.done") : L::Tf("mis.stage", std::to_string(m.stage + 1), std::to_string(m.stages));
            dl->AddText(ui::theme::Body(), S(16), ImVec2(x, y), P().textDim, st.c_str());
            y += S(28);
            float half = (bw - S(8)) * 0.5f;
            std::string id = m.id;
            int actIdx = m.activeIndex;
            bool hasSeed = m.hasSeed;
            uint64_t seed = m.seed;
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (ui::Button("##next", L::T("mis.next"), half, ui::Tone::Teal, live)) {
                game::BackupSaveOnce();
                game::Post([id, actIdx, hasSeed, seed] {
                    if (uintptr_t a2 = FindActive(id, actIdx, hasSeed, seed)) StepMission(a2);
                    LoadMissions();
                });
            }
            ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
            if (ui::Button("##finish", L::T("mis.finish"), half, ui::Tone::Green, live)) {
                game::BackupSaveOnce();
                game::Post([id, actIdx, hasSeed, seed] { g_finish = { FindActive(id, actIdx, hasSeed, seed), -2, 0 }; });
            }
            y += S(46);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            if (ui::HoldButton("##skip", L::T("mis.skip"), bw)) {
                game::BackupSaveOnce();
                game::Post([id, actIdx, hasSeed, seed] {
                    if (uintptr_t a2 = FindActive(id, actIdx, hasSeed, seed)) CompleteMission(a2);
                    LoadMissions();
                });
            }
            y += S(46);
        }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##refresh", L::T("mis.refresh"), bw, ui::Tone::Neutral, live)) refresh();
        y += S(46);
    }
    const char* note = L::T(f && f->key.starts_with("SIG:") ? "sig.note" : "mis.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    return y + ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
}

// ---- UI: fleet and companions page ----

struct CrewItem { bool pet = false; int index = 0; uintptr_t at = 0; std::string name, sub; };

struct CrewUi
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    std::vector<CrewItem> items;
    std::string sig;
} gc;

const char* kFrigateClasses[] = { "Combat", "Exploration", "Mining", "Diplomacy", "Support", "Normandy", "DeepSpace", "DeepSpaceCommon", "Pirate", "GhostShip", "Swarm" };

std::vector<CrewItem> ReadCrew()
{
    std::vector<CrewItem> out;
    int i = 0;
    for (uintptr_t f : Frigates()) {
        CrewItem c;
        c.index = i++;
        c.at = f;
        c.name = Fixed(f + 0xB0, 0x20);
        int32_t cls = -1;
        Rd(f + 0xD0, cls);
        c.sub = cls >= 0 && cls < 11 ? kFrigateClasses[cls] : "?";
        out.push_back(c);
    }
    for (int p = 0; p < 30; ++p) {
        uintptr_t r = Pet(p);
        uint64_t filled = 0;
        if (!r || !Rd(r + kPetFilled, filled) || !filled) continue;
        CrewItem c;
        c.pet = true;
        c.index = p;
        c.at = r;
        c.name = Fixed(r + kPetName, 0x20);
        c.sub = Fixed(r + kPetId, 16);
        out.push_back(c);
    }
    return out;
}

void CrewList(const char* query)
{
    if (!game::PlayerState()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T("items.ingame"));
        return;
    }
    static int tick = 0;
    if (++tick % 30 == 1) gc.items = ReadCrew(); // plain reads, safe from the render thread
    std::string sig;
    for (auto& c : gc.items) sig += std::to_string(c.at) + c.name + c.sub + "|";
    if (sig != gc.sig) {
        gc.sig = sig;
        gc.view.clear();
        for (auto& c : gc.items) {
            ui::BrowserEntry b;
            b.key = std::to_string(c.at);
            b.title = c.name.empty() ? (c.pet ? L::Tf("crew.pet", std::to_string(c.index + 1)) : L::Tf("crew.frigate", std::to_string(c.index + 1))) : c.name;
            b.subtitle = std::string(L::T(c.pet ? "crew.chip.pets" : "crew.chip.frigates")) + "   /   " + c.sub;
            b.glyph = c.pet ? icon::Paw : icon::Ship;
            b.chip = c.pet ? 2 : 1;
            gc.view.push_back(std::move(b));
        }
        gc.browser.Invalidate();
    }
    gc.browser.Draw(gc.view, query);
}

float Bar(const char* id, const char* label, float* v, float lo, float hi, const char* fmt, float x, float y, float w)
{
    ImGui::GetWindowDrawList()->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, label);
    ImGui::SetCursorScreenPos(ImVec2(x, y + S(20)));
    ui::EnergyBar(id, v, lo, hi, fmt, w);
    return y + S(46);
}

float CrewInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = gc.browser.Focused(gc.view);
    CrewItem c;
    if (f) for (auto& i : gc.items) if (std::to_string(i.at) == f->key) c = i;
    ui::BrowserEntry head;
    if (f) head = *f;
    else { head.title = L::T("tab.crew"); head.subtitle = L::T("crew.subtitle"); head.body = L::T("crew.help"); head.glyph = icon::Users; }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    if (!f || !c.at) return y;
    if (!c.pet) {
        // class
        std::vector<std::string> names;
        static const int kAllowed[] = { 0, 1, 2, 3, 4, 6, 7, 8, 9, 10 }; // no Normandy
        int32_t cls = -1;
        Rd(c.at + 0xD0, cls);
        int pick = -1;
        for (int i = 0; i < 10; ++i) { names.push_back(kFrigateClasses[kAllowed[i]]); if (kAllowed[i] == cls) pick = i; }
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("crew.class"));
        ImGui::SetCursorScreenPos(ImVec2(x, y + S(20)));
        if (ui::Dropdown("##fcls", names, &pick, bw)) { game::BackupSaveOnce(); Wr<int32_t>(c.at + 0xD0, kAllowed[pick]); }
        y += S(62);
        static const char* statKeys[] = { "crew.combat", "crew.exploration", "crew.mining", "crew.diplomacy", "crew.fuelburn", nullptr, "crew.speed" };
        float hi = (float)std::max(F.frigateMax, 30);
        for (int s : { 0, 1, 2, 3, 6, 4 }) {
            int32_t v = 0;
            Rd(c.at + F.stats + s * 4, v);
            float fv = (float)v;
            char id[16];
            snprintf(id, sizeof id, "##st%d", s);
            float ny = Bar(id, L::T(statKeys[s]), &fv, -10.f, hi, "%.0f", x, y, bw);
            if ((int)fv != v) { game::BackupSaveOnce(); Wr<int32_t>(c.at + F.stats + s * 4, (int32_t)fv); }
            y = ny;
        }
        float half = (bw - S(8)) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##fmax", L::T("crew.maxstats"), half, ui::Tone::Green)) {
            game::BackupSaveOnce();
            for (int s : { 0, 1, 2, 3, 6 }) Wr<int32_t>(c.at + F.stats + s * 4, (int32_t)hi);
            Wr<int32_t>(c.at + F.stats + 4 * 4, 0);
        }
        ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
        if (ui::Button("##frep", L::T("crew.repair"), half, ui::Tone::Teal)) Wr<uint64_t>(c.at + F.damage, 0);
        y += S(46);
    }
    else {
        int32_t treats = 0;
        Rd(c.at + F.petTreats, treats);
        float tv = (float)treats;
        y = Bar("##treats", L::T("crew.genes"), &tv, 0.f, 99.f, "%.0f", x, y, bw);
        if ((int)tv != treats) { game::BackupSaveOnce(); Wr<int32_t>(c.at + F.petTreats, (int32_t)tv); }
        static const char* traitKeys[] = { "crew.helpful", "crew.aggressive", "crew.independent" };
        for (int t = 0; t < 3; ++t) {
            float v = 0;
            Rd(c.at + F.petTraits + t * 4, v);
            float nv = v;
            char id[16];
            snprintf(id, sizeof id, "##tr%d", t);
            y = Bar(id, L::T(traitKeys[t]), &nv, -1.f, 1.f, "%.2f", x, y, bw);
            if (nv != v) Wr<float>(c.at + F.petTraits + t * 4, nv);
        }
        float sc = 1;
        Rd(c.at + F.petScale, sc);
        float ns = sc;
        y = Bar("##scale", L::T("crew.scale"), &ns, 0.1f, 3.f, "%.2fx", x, y, bw);
        auto refreshPet = [] {
            if (!game::FrameHookReady()) return;
            game::Post([] {
                if (uintptr_t ps = game::PlayerState(); ps && F.petRefresh) F.petRefresh((void*)ps);
                if (uintptr_t d = game::Data(); d && F.petRefreshFlag > 0) Wr<uint8_t>(d + F.petRefreshFlag, 1);
            });
        };
        if (ns != sc) { game::BackupSaveOnce(); Wr<float>(c.at + F.petScale, ns); refreshPet(); }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##pmax", L::T("crew.maxgenes"), bw, ui::Tone::Green)) {
            game::BackupSaveOnce();
            for (int k = 0; k < 3; ++k) Wr<int32_t>(c.at + F.petOverrides + k * 4, 3);
            Wr<uint8_t>(c.at + F.petUseOverrides, 1);
            refreshPet();
        }
        y += S(46);
    }
    const char* note = L::T(c.pet ? "crew.note.pet" : "crew.note.frigate");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    return y + ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
}

}

void InstallWorldActions()
{
    BindFn("Mission.Create", F.missionCreate);
    BindFn("Mission.Add", F.missionAdd);
    BindFn("Mission.SetCurrent", F.missionSetCurrent);
    BindFn("Mission.Complete", F.missionComplete);
    BindFn("ScanEvent.FindEvent", F.scanFind);
    BindFn("ScanEvent.Start", F.scanStart);
    BindFn("Customise.RefreshSummonedPet", F.petRefresh);
    F.missionMgr = Off("Mission.Manager");
    F.seasonMgr = Off("Expedition.SeasonMgr", 0x4BF7F0);
    F.scanSlot = Off("ScanEvent.ManagerSlot");
    F.fleet = Off("Fleet.Manager");
    F.currency = Off("Difficulty.CurrencyCost");
    F.craftFree = Off("Difficulty.CraftingIsFree");
    F.startTime = Off("Expedition.StartTime", 0xA0);
    F.stats = Off("Frigate.Stats", 0x104);
    F.damage = Off("Frigate.DamageMask", 0x130);
    F.petTreats = Off("Pet.TreatsAvailable", 0x2420);
    F.petOverrides = Off("Pet.ClassOverrides", 0x2408);
    F.petUseOverrides = Off("Pet.UseClassOverrides", 0x2404);
    F.petTraits = Off("Pet.Traits", 0x23E0);
    F.petScale = Off("Pet.Scale", 0x23F4);
    F.petRefreshFlag = Off("Customise.PetRefreshFlag", 0xE10C0);
    if (uintptr_t g = Loc().GlobalsInstance("cGcFleetGlobals")) {
        uint32_t off = 0x12A4;
        if (auto m = meta::Path("cGcFleetGlobals", "MaxFrigateStatValue")) off = m->offset;
        Rd(g + off, F.frigateMax);
    }
    log::Info("World", "Ready: missions +0x{:X}, scan +0x{:X}, fleet +0x{:X}, currency +0x{:X}, craft free +0x{:X}",
              F.missionMgr, F.scanSlot, F.fleet, F.currency, F.craftFree);

    // Free shop and free building
    g_shop.off = F.currency;
    g_build.off = F.craftFree;
    if (const Target& t = Loc().Resolve("Difficulty.CurrencyCost"); t.ok) {
        // movsxd rcx, [rax+setting]; mov rax, [rip+config]; add rcx, rowIndex
        auto in = Dec(t.site);
        auto cfg = in ? Next(*in) : std::nullopt;
        auto add = cfg ? Next(*cfg) : std::nullopt;
        if (cfg && cfg->ripTarget && add && add->mnemonic == "add" && add->ops.size() == 2 && add->ops[1].kind == hooks::Operand::Imm) {
            g_freeRow.slot = Abs(*cfg->ripTarget);
            g_freeRow.index = add->ops[1].imm; // option Free is 0
        }
        namespace meta = hooks::meta;
        if (auto m = meta::Path("cGcDifficultyCurrencyCostOptionData", "Multipliers")) g_freeRow.multipliers = m->offset;
        if (auto m = meta::Path("cGcDifficultyCurrencyCostOptionData", "FreeCostTypes")) g_freeRow.freeTypes = m->offset;
        if (auto m = meta::Path("cGcDifficultyCurrencyCostOptionData", "CostManagerCostsAreFree")) g_freeRow.managerFree = m->offset;
        if (auto m = meta::Path("cGcDifficultyCurrencyCostOptionData", "InteractionsCostsAreFree")) g_freeRow.interactFree = m->offset;
    }
    BindPin("eco.free_shop", g_shop);
    BindPin("build.free", g_build);
    if (const Target& t = Loc().Resolve("PlayerState.SaveToData"); t.ok && (g_shop.off >= 0 || g_build.off >= 0)) {
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t.func)), reinterpret_cast<void*>(&SaveHk));
        if (h) g_save = std::move(*h);
        else log::Warn("World", "Save hook could not be placed; free shop may be saved");
    }

    if (auto* t = Need("Cost.Check", { "eco.gifts" })) BindMid("eco.gifts", t->func, GiftCb);

    if (auto* t = Need("Frigate.GetStat", { "fleet.max_stats" })) {
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t->func)), reinterpret_cast<void*>(&StatHk));
        if (!h) Fail("fleet.max_stats", "hook could not be placed");
        else {
            g_stat = std::move(*h);
            Bound b;
            b.id = "fleet.max_stats";
            b.toggle = [](bool on) { g_maxStats = on; return true; };
            Ok(std::move(b));
        }
    }
    for (auto [id, fn] : { std::pair{ "fleet.finish", &FinishExpeditions }, { "fleet.repair", &RepairFrigates } }) {
        if (F.fleet < 0) { Fail(id, "fleet not found"); continue; }
        Bound b;
        b.id = id;
        b.action = [fn] { if (game::FrameHookReady()) { game::BackupSaveOnce(); game::Post(fn); } };
        Ok(std::move(b));
    }

    Registry::Get().Post([] {
        gm.browser.chips = { L::T("mis.chip.all"), L::T("mis.chip.available"), L::T("mis.chip.active"), L::T("mis.chip.signals") };
        ui::Page m;
        m.list = MissionList;
        m.inspector = MissionInspector;
        ui::RegisterPage("missions", std::move(m));
        gc.browser.chips = { L::T("crew.chip.all"), L::T("crew.chip.frigates"), L::T("crew.chip.pets") };
        ui::Page c;
        c.list = CrewList;
        c.inspector = CrewInspector;
        ui::RegisterPage("crew", std::move(c));
    });
}

void WorldActionsTick()
{
    PinTick(g_shop);
    PinTick(g_build);
    SetFreeRow(g_shop.on); // follows the toggle, and catches a config reload
    if (g_finish.mission && game::FrameHookReady()) {
        game::Post([] {
            uintptr_t m = g_finish.mission;
            if (!m) return;
            int32_t stage = 0, count = 0;
            if (!IsActiveMissionPtr(m) || !Rd(m + 0x48, stage) || !Rd(m + 0x1C, count) || stage >= count) {
                g_finish.mission = 0;
                LoadMissions();
                return;
            }
            // Give each stage a moment to run (reward stages finish by themselves), then push it on.
            if (stage != g_finish.stage) { g_finish.stage = stage; g_finish.waits = 0; return; }
            if (++g_finish.waits < 4) return;
            g_finish.waits = 0;
            if (!StepMission(m)) { g_finish.mission = 0; LoadMissions(); }
            else g_finish.stage = stage + 1;
        });
    }
}

}
