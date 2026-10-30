#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/Catalog.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// Scout tab: a report on the current system and its planets, and a scan of the
// regions around the player for real systems, classified by the game's own galaxy generator.
namespace os::features {

namespace {

namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

using ClassifyVoxelFn = void (*)(const void* coord, void* out);
using ClassifyStarFn = void (*)(uint64_t ua, void* out);
using CheckUAFn = bool (*)(uint64_t ua, uint64_t* out, bool seeded, bool strict);
ClassifyVoxelFn g_voxel = nullptr;
ClassifyStarFn g_star = nullptr;
CheckUAFn g_check = nullptr;
// Planet biomes of any system: the game's GenerateWeatherInfo recipe on one scratch cGcSolarSystemData.
using SysDataFn = void (*)(void* data);
using GenBiomesFn = void (*)(void* generator, uint64_t ua, void* data);
SysDataFn g_sysDefaults = nullptr, g_sysDestroy = nullptr;
GenBiomesFn g_genBiomes = nullptr;
// The galaxy map's words for economy, wealth, conflict and race.
using AttrStringsFn = void (*)(uint64_t ua, int32_t trade, int32_t wealth, int32_t conflict, int32_t race, bool noInfo, char* economy,
                               char* wealthOut, char* conflictOut, char* raceOut, int32_t variants);
AttrStringsFn g_attrStrings = nullptr;
int64_t g_generator = 0x521180; // cGcSolarSystem.mSolarSystemGenerator
uint8_t* g_sysBuf = nullptr;    // game thread
constexpr size_t kSysBuf = 0x4000;

// Data offset of the loaded cGcSolarSystem*, its planet array and the cGcPlanet size.
int64_t g_sysPtr = 0x72AFB0, g_planets = 0x2E30, g_stride = 0xD9170;
constexpr uint32_t kPlanetData = 0x60; // cGcPlanet.mPlanetData, checked against PlanetIndex
constexpr int kLyPerRegion = 400;
constexpr uint32_t kStarBuf = 0x800, kVoxBuf = 0x200;

// ---- reflection: offsets and enum names, filled once on the game thread ----

struct Names
{
    std::vector<std::pair<int, std::string>> v;
    std::string Of(int x) const
    {
        for (auto& [k, n] : v) if (k == x) return n;
        return {};
    }
};

struct Offsets
{
    uint32_t starSize = 0x6B0;
    uint32_t sType = 0x6A0, sAnomaly = 0x688, sPlanets = 0x690, sPrime = 0x694, sTrade = 0x680, sWealth = 0x684, sConflict = 0x68C,
             sRace = 0x69C, sAbandoned = 0x6A4, sGasGiant = 0x6A5, sGiant = 0x6A6, sPirate = 0x6A7;
    uint32_t vAtlas = 0x0, vHole = 0x30, vAtlasN = 0x68, vHoleN = 0x6C, vPurpleN = 0x78, vPurpleStart = 0x7C, vGap = 0x8C;
    uint32_t yName = 0x2554, yStar = 0x2550, yTrade = 0x2520, yWealth = 0x2524, yConflict = 0x2530, yRace = 0x2534, yPlanets = 0x2544,
             yInputs = 0x2180, yInputsN = 8, inSize = 0x58, inBiome = 0x30, inSub = 0x34, inIndex = 0x3C, inPlanetSize = 0x40, inPrime = 0x52;
    uint32_t pCommon = 0x33D0, pUncommon = 0x3430, pRare = 0x3400, iDesc = 0x3548 + 0x300;
    uint32_t pIndex = 0x353C, pName = 0x3A4E, iType = 0x3548 + 0x380, iWeather = 0x3548 + 0x480, iRes = 0x3548 + 0x400,
             iFlora = 0x3548 + 0x280, iFauna = 0x3548 + 0x200, iSent = 0x3548, iSentN = 4, iExtreme = 0x3548 + 0x504,
             iSpecial = 0x3548 + 0x505;
    Names star, anomaly, trade, wealth, conflict, race, biome, subtype, size;
} O;
std::atomic<bool> g_ready{ false };

uint32_t Off(const char* cls, const char* path, uint32_t fallback)
{
    auto m = meta::Path(cls, path);
    return m ? m->offset : fallback;
}

void InitOffsets()
{
    if (g_ready) return;
    const char* sa = "cGcGalaxyStarAttributesData";
    if (auto* ci = meta::Get(sa); ci && ci->size) O.starSize = ci->size;
    O.sType = Off(sa, "Type", O.sType);
    O.sAnomaly = Off(sa, "Anomaly", O.sAnomaly);
    O.sPlanets = Off(sa, "NumberOfPlanets", O.sPlanets);
    O.sPrime = Off(sa, "NumberOfPrimePlanets", O.sPrime);
    O.sTrade = Off(sa, "TradingData.TradingClass", O.sTrade);
    O.sWealth = Off(sa, "TradingData.WealthClass", O.sWealth);
    O.sConflict = Off(sa, "ConflictData", O.sConflict);
    O.sRace = Off(sa, "Race", O.sRace);
    O.sAbandoned = Off(sa, "AbandonedSystem", O.sAbandoned);
    O.sGasGiant = Off(sa, "IsGasGiantSystem", O.sGasGiant);
    O.sGiant = Off(sa, "IsGiantSystem", O.sGiant);
    O.sPirate = Off(sa, "IsPirateSystem", O.sPirate);
    const char* va = "cGcGalaxyVoxelAttributesData";
    O.vAtlas = Off(va, "AtlasStationIndices", O.vAtlas);
    O.vHole = Off(va, "BlackholeIndices", O.vHole);
    O.vAtlasN = Off(va, "AtlasStationCount", O.vAtlasN);
    O.vHoleN = Off(va, "BlackholeCount", O.vHoleN);
    O.vPurpleN = Off(va, "PurpleSystemsCount", O.vPurpleN);
    O.vPurpleStart = Off(va, "PurpleSystemsStart", O.vPurpleStart);
    O.vGap = Off(va, "InsideGoalGap", O.vGap);
    const char* sd = "cGcSolarSystemData";
    O.yName = Off(sd, "Name", O.yName);
    O.yStar = Off(sd, "StarType", O.yStar);
    O.yTrade = Off(sd, "TradingData.TradingClass", O.yTrade);
    O.yWealth = Off(sd, "TradingData.WealthClass", O.yWealth);
    O.yConflict = Off(sd, "ConflictData", O.yConflict);
    O.yRace = Off(sd, "InhabitingRace", O.yRace);
    O.yPlanets = Off(sd, "Planets", O.yPlanets);
    if (auto m = meta::Path(sd, "Planet Generation Inputs")) { O.yInputs = m->offset; if (m->count) O.yInputsN = m->count; }
    const char* gi = "cGcPlanetGenerationInputData";
    if (auto* ci = meta::Get(gi); ci && ci->size) O.inSize = ci->size;
    O.inBiome = Off(gi, "Biome", O.inBiome);
    O.inSub = Off(gi, "BiomeSubType", O.inSub);
    O.inPlanetSize = Off(gi, "PlanetSize", O.inPlanetSize);
    O.inIndex = Off(gi, "PlanetIndex", O.inIndex);
    O.inPrime = Off(gi, "Prime", O.inPrime);
    const char* pd = "cGcPlanetData";
    O.pIndex = Off(pd, "PlanetIndex", O.pIndex);
    O.pName = Off(pd, "Name", O.pName);
    O.pCommon = Off(pd, "CommonSubstanceID", O.pCommon);
    O.pUncommon = Off(pd, "UncommonSubstanceID", O.pUncommon);
    O.pRare = Off(pd, "RareSubstanceID", O.pRare);
    O.iDesc = Off(pd, "PlanetInfo.PlanetDescription", O.iDesc);
    O.iType = Off(pd, "PlanetInfo.PlanetType", O.iType);
    O.iWeather = Off(pd, "PlanetInfo.Weather", O.iWeather);
    O.iRes = Off(pd, "PlanetInfo.Resources", O.iRes);
    O.iFlora = Off(pd, "PlanetInfo.Flora", O.iFlora);
    O.iFauna = Off(pd, "PlanetInfo.Fauna", O.iFauna);
    if (auto m = meta::Path(pd, "PlanetInfo.SentinelsPerDifficulty")) { O.iSent = m->offset; if (m->count) O.iSentN = m->count; }
    O.iExtreme = Off(pd, "PlanetInfo.IsWeatherExtreme", O.iExtreme);
    O.iSpecial = Off(pd, "PlanetInfo.SpecialFauna", O.iSpecial);
    O.star.v = meta::EnumNames(sa, "Type");
    O.anomaly.v = meta::EnumNames(sa, "Anomaly");
    O.trade.v = meta::EnumNames("cGcPlanetTradingData", "TradingClass");
    O.wealth.v = meta::EnumNames("cGcPlanetTradingData", "WealthClass");
    O.conflict.v = meta::EnumNames("cGcPlayerConflictData", "ConflictLevel");
    O.race.v = meta::EnumNames(sa, "Race");
    O.biome.v = meta::EnumNames(gi, "Biome");
    O.subtype.v = meta::EnumNames(gi, "BiomeSubType");
    O.size.v = meta::EnumNames(gi, "PlanetSize");
    log::Info("Scout", "Offsets: star size 0x{:X}, {} star types, {} races, {} biomes, {} subtypes", O.starSize, O.star.v.size(),
              O.race.v.size(), O.biome.v.size(), O.subtype.v.size());
    std::string names;
    for (auto* e : { &O.biome, &O.size }) {
        for (auto& [k, n] : e->v) names += " " + n;
        names += " /";
    }
    log::Info("Scout", "Biomes and planet sizes:{}", names);
    g_ready = true;
}

// "HighTech" -> "High Tech", or the locale's name for it.
std::string Pretty(const std::string& name)
{
    if (name.empty()) return "?";
    if (L::Has("scout.n." + name)) return L::T("scout.n." + name);
    std::string s;
    for (size_t i = 0; i < name.size(); ++i) {
        if (i && isupper((unsigned char)name[i]) && islower((unsigned char)name[i - 1])) s += ' ';
        s += name[i];
    }
    return s;
}

std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

ImU32 StarColour(const std::string& star)
{
    std::string s = Lower(star);
    if (s == "red") return IM_COL32(255, 110, 80, 255);
    if (s == "green") return IM_COL32(110, 230, 120, 255);
    if (s == "blue") return IM_COL32(110, 170, 255, 255);
    if (s == "purple") return IM_COL32(190, 120, 255, 255);
    return IM_COL32(255, 214, 90, 255);
}

// ---- packed universe addresses ----

uint64_t Pack(int x, int y, int z, int system, int galaxy, int planet)
{
    return (uint64_t)(x & 0xFFF) | (uint64_t)(z & 0xFFF) << 12 | (uint64_t)(y & 0xFF) << 24 | (uint64_t)(galaxy & 0xFF) << 32 |
           (uint64_t)(system & 0xFFF) << 40 | (uint64_t)(planet & 0xF) << 52;
}

int Signed(uint64_t v, int bits) { int s = (int)(v & ((1ull << bits) - 1)); return s >= (1 << (bits - 1)) ? s - (1 << bits) : s; }

std::string Glyphs(uint64_t ua)
{
    char b[16];
    snprintf(b, sizeof b, "%01X%03X%02X%03X%03X", (unsigned)((ua >> 52) & 0xF), (unsigned)((ua >> 40) & 0xFFF), (unsigned)((ua >> 24) & 0xFF),
             (unsigned)((ua >> 12) & 0xFFF), (unsigned)(ua & 0xFFF));
    return b;
}

// ---- game calls, guarded ----

bool CallVoxel(const void* coord, void* out)
{
    __try { g_voxel(coord, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallStar(uint64_t ua, void* out)
{
    __try { g_star(ua, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 1 valid, 0 corrected into *out, -1 failed
int CallCheck(uint64_t ua, uint64_t* out, bool strict)
{
    __try { return g_check(ua, out, true, strict) ? 1 : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// ---- star count of a region ----

// The portal check's seeded pick for an index past the region's stars: index = (h * count) >> 32,
// with h hashed from the typed address.
uint32_t PickHash(uint64_t ua)
{
    uint32_t lo = (uint32_t)ua, hi = (uint32_t)(ua >> 32);
    uint32_t a = lo + (lo == 0 ? 1u : 0u);
    uint32_t b = _rotr(lo, 16) ^ lo ^ hi;
    return a * 0x5A76F899u + b;
}

constexpr uint64_t kSystemMask = 0x000FFFFFFFFFFFFFull; // region, galaxy and system; not the planet

// The check returns false for planet 0 too (planets count from 1), so a system is real when the
// address comes back with the same system.
bool IsValid(uint64_t base, int system)
{
    uint64_t ua = base | (uint64_t)system << 40, out = 0;
    return CallCheck(ua, &out, true) >= 0 && (out & kSystemMask) == (ua & kSystemMask);
}

std::atomic<int> g_hashHits{ 0 }, g_hashMisses{ 0 }; // how often the hash route held, for the log

// Number of real systems in the region, -1 if unknown. Two seeded checks with the largest hashes
// pin it down, two strict checks confirm it; a binary search covers the rest.
int CountSystems(uint64_t base)
{
    uint32_t best[2] = {};
    uint64_t bestUa[2] = {};
    for (int s = 0x300; s <= 0xFFF; ++s)
        for (int p = 0; p < 16; ++p) {
            uint64_t ua = base | (uint64_t)s << 40 | (uint64_t)p << 52;
            uint32_t h = PickHash(ua);
            if (h > best[0]) { best[1] = best[0]; bestUa[1] = bestUa[0]; best[0] = h; bestUa[0] = ua; }
            else if (h > best[1]) { best[1] = h; bestUa[1] = ua; }
        }
    int64_t lo = 0, hi = 0x2FF;
    for (int k = 0; k < 2 && lo < hi; ++k) {
        uint64_t out = 0;
        if (CallCheck(bestUa[k], &out, true) < 0) break;
        if ((out & 0xFFFFFFFFFFull) != (base & 0xFFFFFFFFFFull)) return -1; // moved to another region
        if ((out & kSystemMask) == (bestUa[k] & kSystemMask)) break; // index was real: the hash route doesn't apply
        uint64_t idx = (out >> 40) & 0xFFF, h = best[k];
        int64_t cmin = (int64_t)(((idx << 32) + h - 1) / h), cmax = (int64_t)((((idx + 1) << 32) - 1) / h);
        lo = std::max(lo, cmin);
        hi = std::min(hi, cmax);
    }
    // index 0 is never real (the game moves it to 1), so a count of c means systems 1 to c - 1
    if (lo == hi && (lo <= 1 ? !IsValid(base, 1) : IsValid(base, (int)lo - 1) && !IsValid(base, (int)lo))) {
        ++g_hashHits;
        return (int)std::max<int64_t>(lo, 1);
    }
    ++g_hashMisses;
    lo = 1;
    hi = 0x2FF;
    while (lo < hi) {
        int64_t mid = (lo + hi) / 2;
        if (IsValid(base, (int)mid)) lo = mid + 1;
        else hi = mid;
    }
    return (int)lo;
}

// ---- systems ----

// Planet kinds, as bits of Sys::planetKinds.
enum PlanetKind { kpExotic, kpColour, kpGiant, kpMega, kpInfested, kpWater, kpLava, kpSwamp, kpCount };

struct Sys
{
    uint64_t ua = 0;
    float dist = 0; // regions from the player
    int star = -1, anomaly = 0, planets = 0, prime = 0, trade = -1, wealth = -1, conflict = -1, race = -1;
    bool abandoned = false, pirate = false, giant = false, gasGiant = false, hole = false, atlas = false, purple = false;
    int rarity = 0;
    // generated planets: biome and subtype enum values, -1 when unknown
    int8_t nBiomes = 0, biome[8] = {}, sub[8] = {};
    uint16_t planetKinds = 0;
};

// Huge* subtypes are giant flora and terrain ("mega flora"); giant planets come from PlanetSize.
uint16_t KindsOf(const std::string& biome, const std::string& sub, const std::string& size)
{
    std::string b = Lower(biome), s = Lower(sub);
    uint16_t k = 0;
    if (b == "weird") k |= 1 << kpExotic;
    if (b == "red" || b == "green" || b == "blue") k |= 1 << kpColour;
    if (Lower(size) == "giant") k |= 1 << kpGiant;
    if (s.rfind("huge", 0) == 0) k |= 1 << kpMega;
    if (s == "infested" || b == "infested") k |= 1 << kpInfested;
    if (b == "waterworld" || b == "water") k |= 1 << kpWater;
    if (b == "lava" || s == "lava") k |= 1 << kpLava;
    if (b == "swamp" || s == "swamp") k |= 1 << kpSwamp;
    return k;
}

bool CallSysData(SysDataFn fn, uint8_t* buf)
{
    __try { fn(buf); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallBiomes(uintptr_t gen, uint64_t ua, uint8_t* buf)
{
    __try { g_genBiomes((void*)gen, ua, buf); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Game thread. Every planet's biome and subtype for a system, from the loaded system's generator.
bool GeneratePlanets(uintptr_t generator, Sys& s)
{
    if (!g_sysBuf || !g_sysDefaults || !g_genBiomes || !g_sysDestroy || !generator) return false;
    memset(g_sysBuf, 0, kSysBuf);
    if (!CallSysData(g_sysDefaults, g_sysBuf)) return false;
    bool ok = CallBiomes(generator, s.ua & 0x000FFFFFFFFFFFFFull, g_sysBuf);
    if (ok) {
        int32_t n = 0;
        memcpy(&n, g_sysBuf + O.yPlanets, 4);
        n = std::clamp(n, 0, (int)std::min<uint32_t>(O.yInputsN, 8));
        s.nBiomes = (int8_t)n;
        for (int k = 0; k < n; ++k) {
            const uint8_t* in = g_sysBuf + O.yInputs + k * O.inSize;
            int32_t b = -1, sb = -1, sz = -1;
            memcpy(&b, in + O.inBiome, 4);
            memcpy(&sb, in + O.inSub, 4);
            memcpy(&sz, in + O.inPlanetSize, 4);
            s.biome[k] = (int8_t)b;
            s.sub[k] = (int8_t)sb;
            s.planetKinds |= KindsOf(O.biome.Of(b), O.subtype.Of(sb), O.size.Of(sz));
        }
    }
    CallSysData(g_sysDestroy, g_sysBuf);
    return ok;
}

// Traits a system can have; planet kinds follow kPlanetBase, star colours kStarBase.
enum Trait { kHole, kAtlas, kPurple, kAbandoned, kPirate, kGiant, kGasGiant, kPlanetBase, kStarBase = kPlanetBase + kpCount };
constexpr int kTraits = kStarBase + 16;

bool IsPurple(const Sys& s) { return s.purple || Lower(O.star.Of(s.star)) == "purple"; }

std::vector<int> Traits(const Sys& s)
{
    std::vector<int> t;
    if (s.hole) t.push_back(kHole);
    if (s.atlas) t.push_back(kAtlas);
    if (IsPurple(s)) t.push_back(kPurple);
    else if (s.star >= 0 && s.star < 16) t.push_back(kStarBase + s.star);
    if (s.abandoned) t.push_back(kAbandoned);
    if (s.pirate) t.push_back(kPirate);
    if (s.giant) t.push_back(kGiant);
    if (s.gasGiant) t.push_back(kGasGiant);
    for (int k = 0; k < kpCount; ++k)
        if (s.planetKinds & (1 << k)) t.push_back(kPlanetBase + k);
    return t;
}

const char* kPlanetKindKeys[kpCount] = { "scout.pk.exotic", "scout.pk.colour", "scout.pk.giant", "scout.pk.mega",
                                         "scout.pk.infested", "scout.pk.water", "scout.pk.lava", "scout.pk.swamp" };

std::string TraitName(int t)
{
    switch (t) {
    case kHole: return L::T("scout.r.hole");
    case kAtlas: return L::T("scout.r.atlas");
    case kPurple: return L::T("scout.r.purple");
    case kAbandoned: return L::T("scout.r.abandoned");
    case kPirate: return L::T("scout.r.pirate");
    case kGiant: return L::T("scout.r.giant");
    case kGasGiant: return L::T("scout.r.gasgiant");
    default:
        if (t >= kPlanetBase && t < kStarBase) return L::T(kPlanetKindKeys[t - kPlanetBase]);
        return L::Tf("scout.r.star", Pretty(O.star.Of(t - kStarBase)));
    }
}

// How often each trait shows up in the scan results.
struct Freq
{
    int total = 0;
    int n[kTraits] = {};
};

// A trait is rare when under 2.5% of the scanned systems have it; purple always is.
// Without a big enough scan, only purple and giant systems count.
constexpr double kRareShare = 0.025;
constexpr int kMinSample = 200;

bool RareTrait(int t, const Freq& f)
{
    if (t == kPurple) return true;
    if (f.total < kMinSample) return t == kGiant;
    return f.n[t] < f.total * kRareShare;
}

// Sort weight: rarer traits weigh more (bits of surprise), so a lone purple outranks a pirate system.
int Rarity(const Sys& s, const Freq& f)
{
    int r = 0;
    for (int t : Traits(s)) {
        if (!RareTrait(t, f)) continue;
        double share = f.total >= kMinSample ? std::max(f.n[t], 1) / (double)f.total : 0.005;
        r += std::max(1, (int)std::lround(std::log2(1.0 / share)));
    }
    return r;
}

// "Blue star, 1.2% of nearby systems" for each rare trait.
std::vector<std::string> Reasons(const Sys& s, const Freq& f)
{
    std::vector<std::string> out;
    for (int t : Traits(s)) {
        if (!RareTrait(t, f)) continue;
        if (f.total < kMinSample) { out.push_back(TraitName(t)); continue; }
        char pct[16];
        snprintf(pct, sizeof pct, "%.1f", 100.0 * f.n[t] / f.total);
        out.push_back(L::Tf("scout.r.share", TraitName(t), pct));
    }
    return out;
}

Freq Count(const std::vector<Sys>& all)
{
    Freq f;
    f.total = (int)all.size();
    for (auto& s : all)
        for (int t : Traits(s)) ++f.n[t];
    return f;
}

template <class T> T At(const uint8_t* b, uint32_t off) { T v; memcpy(&v, b + off, sizeof v); return v; }

bool Classify(uint64_t ua, Sys& s)
{
    alignas(16) uint8_t buf[kStarBuf] = {};
    if (O.starSize > kStarBuf || !CallStar(ua, buf)) return false;
    s.ua = ua;
    s.star = At<int32_t>(buf, O.sType);
    s.anomaly = At<int32_t>(buf, O.sAnomaly);
    s.planets = At<int32_t>(buf, O.sPlanets);
    s.prime = At<int32_t>(buf, O.sPrime);
    s.trade = At<int32_t>(buf, O.sTrade);
    s.wealth = At<int32_t>(buf, O.sWealth);
    s.conflict = At<int32_t>(buf, O.sConflict);
    s.race = At<int32_t>(buf, O.sRace);
    s.abandoned = buf[O.sAbandoned] != 0;
    s.gasGiant = buf[O.sGasGiant] != 0;
    s.giant = buf[O.sGiant] != 0;
    s.pirate = buf[O.sPirate] != 0;
    return true;
}

// ---- the scan (game thread) ----

struct Region { int x, y, z; float dist; };

struct
{
    bool active = false;
    int galaxy = 0;
    uint64_t origin = 0; // the player's system when the scan started
    std::vector<Region> regions;
    size_t next = 0;
    // the region being classified, in chunks
    std::vector<int> todo;
    size_t todoNext = 0;
    uint64_t base = 0;
    float dist = 0;
    alignas(16) uint8_t vox[kVoxBuf] = {};
} g_scan;

std::atomic<int> g_request{ -1 }; // radius asked for by the page
std::atomic<int> g_regionsDone{ 0 }, g_regionsTotal{ 0 };
std::atomic<bool> g_scanning{ false };
std::atomic<uint64_t> g_version{ 0 };
std::mutex g_mx;
std::vector<Sys> g_found;

constexpr int kChunk = 48; // systems classified per frame, planets included

void StartScan(int radius)
{
    uint64_t here = 0;
    if (!g_voxel || !g_star || !g_check || !CurrentAddress(here)) { log::Warn("Scout", "Scan not available"); return; }
    InitOffsets();
    auto& s = g_scan;
    s = {};
    s.galaxy = (int)((here >> 32) & 0xFF);
    s.origin = here & kSystemMask;
    int hx = Signed(here, 12), hz = Signed(here >> 12, 12), hy = Signed(here >> 24, 8);
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dz = -radius; dz <= radius; ++dz) {
                int x = hx + dx, y = hy + dy, z = hz + dz;
                if (x < -2047 || x > 2047 || z < -2047 || z > 2047 || y < -127 || y > 127) continue;
                s.regions.push_back({ x, y, z, std::sqrt((float)(dx * dx + dy * dy + dz * dz)) });
            }
    std::stable_sort(s.regions.begin(), s.regions.end(), [](const Region& a, const Region& b) { return a.dist < b.dist; });
    s.active = true;
    {
        std::lock_guard lk(g_mx);
        g_found.clear();
    }
    g_regionsDone = 0;
    g_regionsTotal = (int)s.regions.size();
    g_scanning = true;
    ++g_version;
    log::Info("Scout", "Scanning {} regions around {}", s.regions.size(), Glyphs(here));
}

// Classify the region's voxel, count its stars and queue every real index.
void BeginRegion(const Region& r)
{
    auto& s = g_scan;
    s.todo.clear();
    s.todoNext = 0;
    s.base = Pack(r.x, r.y, r.z, 0, s.galaxy, 0);
    s.dist = r.dist;
    memset(s.vox, 0, sizeof s.vox);
    uint8_t coord[8] = {};
    int16_t xyz[3] = { (int16_t)r.x, (int16_t)r.z, (int16_t)r.y };
    memcpy(coord, xyz, 6);
    coord[6] = 1;
    if (!CallVoxel(coord, s.vox) || s.vox[O.vGap]) return; // the galaxy's core gap has no stars
    auto t0 = std::chrono::steady_clock::now();
    int n = CountSystems(s.base);
    if (n < 0) { log::Warn("Scout", "Star count unknown at {}", Glyphs(s.base)); return; }
    if (s.next <= 3) {
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        log::Info("Scout", "Region {}: {} systems, counted in {:.1f} ms", Glyphs(s.base), n, ms);
    }
    for (int i = 1; i < n; ++i) s.todo.push_back(i);
    int pn = At<int32_t>(s.vox, O.vPurpleN), ps = At<int32_t>(s.vox, O.vPurpleStart);
    if (pn > 0 && pn <= 64 && ps >= n && ps + pn <= 0xFFF)
        for (int i = ps; i < ps + pn; ++i) s.todo.push_back(i);
}

void ScoutTick()
{
    int req = g_request.exchange(-1);
    if (req >= 0) StartScan(req);
    auto& s = g_scan;
    if (!s.active) return;
    // the planet generator belongs to the loaded system: stop if the player leaves it
    uint64_t here = 0;
    uintptr_t d = game::Data(), sysPtr = 0;
    if (!CurrentAddress(here) || (here & kSystemMask) != s.origin || !d || !Rd(d + g_sysPtr, sysPtr) || !sysPtr) {
        s.active = false;
        g_scanning = false;
        ++g_version;
        log::Info("Scout", "Scan stopped: the player left the system");
        return;
    }
    if (s.todoNext >= s.todo.size()) {
        if (s.next >= s.regions.size()) {
            s.active = false;
            g_scanning = false;
            ++g_version;
            Freq f;
            { std::lock_guard lk(g_mx); f = Count(g_found); }
            log::Info("Scout", "Scan done: {} systems, star counts by hash {} of {}", f.total, g_hashHits.load(), g_hashHits + g_hashMisses);
            std::string share;
            for (int t = 0; t < kTraits; ++t)
                if (f.n[t]) share += std::format("{}{} {}", share.empty() ? "" : ", ", TraitName(t), f.n[t]);
            log::Info("Scout", "Traits: {}", share);
            return;
        }
        BeginRegion(s.regions[s.next++]);
        g_regionsDone = (int)s.next;
        return; // the count costs a voxel generation; classify from the next frame
    }
    int holeN = std::clamp(At<int32_t>(s.vox, O.vHoleN), 0, 12), atlasN = std::clamp(At<int32_t>(s.vox, O.vAtlasN), 0, 12);
    int pn = At<int32_t>(s.vox, O.vPurpleN), ps = At<int32_t>(s.vox, O.vPurpleStart);
    std::vector<Sys> batch;
    for (int k = 0; k < kChunk && s.todoNext < s.todo.size(); ++k) {
        int i = s.todo[s.todoNext++];
        Sys sys;
        if (!Classify(s.base | (uint64_t)i << 40, sys)) continue;
        sys.dist = s.dist;
        // the region lists count stars from 0, addresses from 1 (black hole 0x78 is system 0x79)
        for (int h = 0; h < holeN; ++h) sys.hole |= At<int32_t>(s.vox, O.vHole + 4 * h) + 1 == i;
        for (int a = 0; a < atlasN; ++a) sys.atlas |= At<int32_t>(s.vox, O.vAtlas + 4 * a) + 1 == i;
        sys.purple = pn > 0 && i >= ps && i < ps + pn;
        GeneratePlanets(sysPtr + g_generator, sys);
        batch.push_back(sys);
    }
    if (!batch.empty()) {
        std::lock_guard lk(g_mx);
        g_found.insert(g_found.end(), batch.begin(), batch.end());
        ++g_version;
    }
}

// ---- the current system report (game thread) ----

// The galaxy map's words for a system (Mercantile, Developing, Unstable).
struct Words
{
    std::string economy, wealth, conflict, race;
};

struct PlanetRow
{
    std::string name, type, weather, sentinels, resources, flora, fauna, biome, subtype;
    bool prime = false, extreme = false, special = false, exotic = false;
};

struct Report
{
    bool ok = false;
    uint64_t ua = 0;
    std::string name;
    Sys sys;
    int planets = 0;
    std::vector<PlanetRow> rows;
    Words words;
};

Report g_report; // under g_mx
std::atomic<uint64_t> g_reportVersion{ 0 };

std::string Str(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return ui::StripTags(s);
}

bool Printable(const std::string& s)
{
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20) return false;
    return true;
}

// Planet info fields hold either text or a language key like WEATHER_LUSH_CLEAR5.
std::string Text(uintptr_t at)
{
    std::string s = Str(at, 0x80);
    bool key = !s.empty();
    for (unsigned char c : s) key &= isupper(c) || isdigit(c) || c == '_';
    return key ? ui::StripTags(game::catalog::Translate(s)) : s;
}

// ---- the galaxy map's words ----

std::string Clean(const char* s)
{
    std::string t(s, strnlen(s, 0x100));
    bool key = !t.empty();
    for (unsigned char c : t) key &= isupper(c) || isdigit(c) || c == '_';
    return ui::StripTags(key ? game::catalog::Translate(t) : t);
}

bool CallAttr(const Sys& s, char (*out)[0x100])
{
    __try {
        g_attrStrings(s.ua & 0x000FFFFFFFFFFFFFull, s.trade, s.wealth, s.conflict, s.race, s.abandoned, out[0], out[1], out[2], out[3], 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Game thread. Same arguments as the galaxy map, which asks for one name variant.
Words MakeWords(const Sys& s)
{
    char out[4][0x100] = {};
    if (!g_attrStrings || !CallAttr(s, out)) return {};
    return { Clean(out[0]), Clean(out[1]), Clean(out[2]), Clean(out[3]) };
}

std::mutex g_wordsMx;
std::unordered_map<uint64_t, Words> g_words; // by system address
std::unordered_set<uint64_t> g_wordsPending;

// UI thread. The words for a system, or null until the game thread has made them.
const Words* WordsFor(const Sys& s)
{
    if (!g_attrStrings) return nullptr;
    std::lock_guard lk(g_wordsMx);
    if (auto it = g_words.find(s.ua); it != g_words.end()) return &it->second;
    if (g_wordsPending.insert(s.ua).second)
        game::Post([s] {
            Words w = MakeWords(s);
            std::lock_guard lk(g_wordsMx);
            g_words[s.ua] = std::move(w);
            g_wordsPending.erase(s.ua);
        });
    return nullptr;
}

std::string EconomyText(const Sys& s, const Words* w)
{
    if (w && !w->economy.empty()) return w->economy + (w->wealth.empty() ? "" : ", " + w->wealth);
    return Pretty(O.trade.Of(s.trade)) + ", " + Pretty(O.wealth.Of(s.wealth));
}

std::string ConflictText(const Sys& s, const Words* w) { return w && !w->conflict.empty() ? w->conflict : Pretty(O.conflict.Of(s.conflict)); }
std::string RaceText(const Sys& s, const Words* w) { return w && !w->race.empty() ? w->race : Pretty(O.race.Of(s.race)); }

// "ACTIVATED COPPER" -> "Activated Copper"; mixed case is left alone.
std::string TitleCase(std::string s)
{
    for (unsigned char c : s) if (islower(c)) return s;
    bool start = true;
    for (auto& c : s) {
        c = start ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        start = c == ' ' || c == '-';
    }
    return s;
}

// A substance id (TkID) as its in-game name.
std::string Substance(uintptr_t at)
{
    std::string id = Str(at, 16);
    if (!Printable(id)) return {};
    const game::Item* it = game::catalog::Find(id);
    return TitleCase(it && !it->name.empty() ? ui::StripTags(it->name) : id);
}

// Planet kinds people hunt for.
bool RareBiome(const std::string& biome, const std::string& sub, const std::string& size)
{
    return KindsOf(biome, sub, size) & (1 << kpExotic | 1 << kpColour | 1 << kpGiant | 1 << kpInfested);
}

void ReadReport()
{
    uintptr_t d = game::Data(), sys = 0;
    uint64_t ua = 0;
    Report r;
    // ua 0 while a save is still loading
    if (d && Rd(d + g_sysPtr, sys) && sys && CurrentAddress(ua) && ua) {
        InitOffsets();
        r.ua = ua;
        r.name = Str(sys + O.yName, 0x80);
        if (g_star) Classify(ua & 0x000FFFFFFFFFFFFFull, r.sys);
        // the loaded system data wins over the generator for what the player sees
        Rd(sys + O.yStar, r.sys.star);
        Rd(sys + O.yTrade, r.sys.trade);
        Rd(sys + O.yWealth, r.sys.wealth);
        Rd(sys + O.yConflict, r.sys.conflict);
        Rd(sys + O.yRace, r.sys.race);
        Rd(sys + O.yPlanets, r.planets);
        r.ok = true;
        // keyed on the values too: while a save loads, the system reads as raceless and the words come back empty
        static uint64_t wordsKey = ~0ull;
        static Words words;
        uint64_t key = r.sys.ua ^ (uint64_t)(r.sys.trade & 0xFF) << 56 ^ (uint64_t)(r.sys.wealth & 0xFF) << 48
            ^ (uint64_t)(r.sys.conflict & 0xFF) << 40 ^ (uint64_t)(r.sys.race & 0xFF) << 32 ^ (uint64_t)r.planets << 24;
        if (key != wordsKey) {
            wordsKey = key;
            words = MakeWords(r.sys);
            log::Info("Scout", "Galaxy map words at {}: {} / {} / {} / {}", Glyphs(ua), words.economy, words.wealth, words.conflict, words.race);
        }
        r.words = words;
        int n = std::clamp(r.planets, 0, 6);
        for (int i = 0; i < n; ++i) {
            uintptr_t pd = sys + g_planets + (uintptr_t)i * g_stride + kPlanetData;
            int32_t idx = -1;
            if (!Rd(pd + O.pIndex, idx) || idx != i) break; // layout moved: show the system only
            PlanetRow p;
            p.name = Str(pd + O.pName, 0x80);
            if (!Printable(p.name)) p.name = L::Tf("scout.planet", std::to_string(i + 1));
            // the description carries %PLANETCLASS% for the class word (planet, moon, giant)
            std::string cls = Text(pd + O.iType);
            p.type = Text(pd + O.iDesc);
            if (p.type.empty()) p.type = cls;
            if (size_t k = p.type.find("%PLANETCLASS%"); k != std::string::npos) p.type.replace(k, 13, cls);
            p.weather = Text(pd + O.iWeather);
            for (uint32_t off : { O.pCommon, O.pUncommon, O.pRare }) {
                std::string s = Substance(pd + off);
                if (!s.empty()) p.resources += (p.resources.empty() ? "" : ", ") + s;
            }
            if (p.resources.empty()) p.resources = Text(pd + O.iRes);
            p.flora = Text(pd + O.iFlora);
            p.fauna = Text(pd + O.iFauna);
            // per sentinel difficulty; the normal one is the third, fall back to any
            for (int k : { 2, 1, 3, 0 })
                if (k < (int)O.iSentN && p.sentinels.empty()) p.sentinels = Text(pd + O.iSent + 0x80 * k);
            uint8_t b = 0;
            p.extreme = Rd(pd + O.iExtreme, b) && b;
            p.special = Rd(pd + O.iSpecial, b) && b;
            for (uint32_t k = 0; k < O.yInputsN; ++k) {
                uintptr_t in = sys + O.yInputs + k * O.inSize;
                int32_t pi = -1, bio = -1, sub = -1, sz = -1;
                if (!Rd(in + O.inIndex, pi) || pi != i) continue;
                Rd(in + O.inBiome, bio);
                Rd(in + O.inSub, sub);
                Rd(in + O.inPlanetSize, sz);
                std::string bn = O.biome.Of(bio), sn = O.subtype.Of(sub);
                p.biome = Pretty(bn);
                if (!sn.empty() && Lower(sn) != "none" && Lower(sn) != "standard") p.subtype = Pretty(sn);
                p.exotic = RareBiome(bn, sn, O.size.Of(sz));
                p.prime = Rd(in + O.inPrime, b) && b;
                break;
            }
            r.rows.push_back(std::move(p));
        }
    }
    // once per system: the planet generator against what the game loaded,
    // only once the loaded data is this system (the generator stores the address in its Seed)
    static uint64_t checked = 0;
    static const uint32_t seedOff = [] { auto m = meta::Path("cGcSolarSystemData", "Seed"); return m ? m->offset : 0x2480u; }();
    uint64_t loadedSeed = 0;
    bool loaded = sys && Rd(sys + seedOff, loadedSeed) && loadedSeed == (ua & 0x000FFFFFFFFFFFFFull);
    if (r.ok && loaded && g_sysBuf && (ua & 0x000FFFFFFFFFFFFFull) != checked) {
        checked = ua & 0x000FFFFFFFFFFFFFull;
        Sys gen;
        gen.ua = checked;
        int same = 0, n = 0;
        if (GeneratePlanets(sys + g_generator, gen))
            for (int k = 0; k < gen.nBiomes; ++k) {
                int32_t b = -1, sb = -1;
                Rd(sys + O.yInputs + k * O.inSize + O.inBiome, b);
                Rd(sys + O.yInputs + k * O.inSize + O.inSub, sb);
                ++n;
                same += b == gen.biome[k] && sb == gen.sub[k];
            }
        log::Info("Scout", "Planet generator check at {}: {} of {} planets match the loaded system", Glyphs(ua), same, n);
    }
    std::lock_guard lk(g_mx);
    g_report = std::move(r);
    ++g_reportVersion;
}

void ReportTick()
{
    static uint64_t last = 0;
    uint64_t now = game::Frame();
    if (now - last < 90) return;
    last = now;
    ReadReport();
}

void Tick()
{
    ReportTick();
    ScoutTick();
}

// ---- the page (UI thread) ----

struct Filters
{
    int radius = 1, star = 0, race = 0, trade = 0, wealth = 0, conflict = 0, special = 0, planets = 0, planetKind = 0;
    bool operator==(const Filters&) const = default;
};

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    std::vector<Sys> shown; // same order as view
    uint64_t version = ~0ull;
    Filters f, lastF;
    double lastBuild = 0;
    Freq freq;   // trait shares over the whole scan
    int tab = 0; // 0 this system, 1 nearby systems
} g;

// Dropdown items: "Any" then every enum name; index k > 0 is enum entry k - 1.
std::vector<std::string> Options(const Names& n)
{
    std::vector<std::string> v{ L::T("scout.any") };
    for (auto& [k, name] : n.v) v.push_back(Pretty(name));
    return v;
}

bool Match(int value, const Names& n, int pick) { return pick <= 0 || pick > (int)n.v.size() || n.v[pick - 1].first == value; }

bool Pass(const Sys& s, const Filters& f)
{
    if (!Match(s.star, O.star, f.star) || !Match(s.race, O.race, f.race) || !Match(s.trade, O.trade, f.trade) ||
        !Match(s.wealth, O.wealth, f.wealth) || !Match(s.conflict, O.conflict, f.conflict))
        return false;
    if (f.planets > 0 && s.planets < f.planets + 1) return false;
    if (f.planetKind > 0 && !(s.planetKinds & (1 << (f.planetKind - 1)))) return false;
    switch (f.special) {
    case 1: return s.rarity > 0;
    case 2: return s.hole;
    case 3: return s.atlas;
    case 4: return s.purple || Lower(O.star.Of(s.star)) == "purple";
    case 5: return s.abandoned;
    case 6: return s.pirate;
    case 7: return s.giant || s.gasGiant;
    default: return true;
    }
}

std::string Ly(float regions)
{
    int ly = (int)std::lround(regions * kLyPerRegion);
    std::string s = std::to_string(ly), out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return out;
}

std::string SysName(const Sys& s)
{
    char b[8];
    snprintf(b, sizeof b, "%03X", (unsigned)((s.ua >> 40) & 0xFFF));
    return L::Tf("scout.sysname", b);
}

// Every region has the same indices, so tiles outside the player's region carry their distance.
std::string TileName(const Sys& s)
{
    if (s.dist < 0.5f) return SysName(s);
    char b[8];
    snprintf(b, sizeof b, "%03X", (unsigned)((s.ua >> 40) & 0xFFF));
    return L::Tf("scout.tilename", b, Ly(s.dist));
}

std::string Line(const Sys& s)
{
    return Pretty(O.star.Of(s.star)) + "   /   " + Pretty(O.race.Of(s.race)) + "   /   " + Pretty(O.trade.Of(s.trade)) + "   /   " +
           L::Tf("scout.planets", std::to_string(s.planets));
}

void Rebuild()
{
    std::vector<Sys> all;
    {
        std::lock_guard lk(g_mx);
        all = g_found;
    }
    g.view.clear();
    g.shown.clear();
    g.freq = Count(all);
    for (auto& s : all) {
        s.rarity = Rarity(s, g.freq);
        if (!Pass(s, g.f)) continue;
        ui::BrowserEntry e;
        e.key = Glyphs(s.ua);
        e.title = TileName(s);
        e.subtitle = Line(s);
        e.glyph = icon::Sun;
        e.tint = StarColour(O.star.Of(s.star));
        e.group = Pretty(O.star.Of(s.star));
        e.value = 100000.0 - s.dist; // sorts nearest first
        e.rarity = s.rarity;
        e.rare = s.rarity > 0;
        e.chip = e.rare ? 1 : 0;
        g.view.push_back(std::move(e));
        g.shown.push_back(s);
    }
    g.browser.Invalidate();
}

void Fact(ImDrawList* dl, float x, float& y, float w, const std::string& k, const std::string& v, ImU32 col = 0)
{
    if (v.empty()) return;
    std::string t = "<TECHNOLOGY>" + k + "<> " + v;
    y += ui::RichText(dl, ui::theme::Body(), S(15), ImVec2(x, y), w, col ? col : P().textBody, t) + S(4);
}

// The report: the system line, then one block per planet.
void DrawReport()
{
    Report r;
    {
        std::lock_guard lk(g_mx);
        r = g_report;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x - S(8);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = p.x, y = p.y;
    if (!r.ok || !g_ready) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textFaint, L::T("scout.here.none"));
        ImGui::Dummy(ImVec2(w, y + S(30) - p.y));
        return;
    }
    std::string star = O.star.Of(r.sys.star);
    std::string title = Printable(r.name) ? r.name : SysName(r.sys);
    // long names wrap, leaving room for the star
    float tw = w - S(24);
    ImVec2 ts = ui::theme::Title()->CalcTextSizeA(S(20), FLT_MAX, tw, title.c_str());
    dl->AddText(ui::theme::Title(), S(20), ImVec2(x, y), P().text, title.c_str(), nullptr, tw);
    if (Rarity(r.sys, g.freq) > 0) {
        std::string st = ui::Utf8(icon::Star);
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x + std::min(ts.x, tw) + S(8), y + S(3)), P().gold, st.c_str());
    }
    y += ts.y + S(6);
    Fact(dl, x, y, w, L::T("scout.f.star"), Pretty(star), 0);
    Fact(dl, x, y, w, L::T("scout.f.economy"), EconomyText(r.sys, &r.words));
    Fact(dl, x, y, w, L::T("scout.f.conflict"), ConflictText(r.sys, &r.words));
    Fact(dl, x, y, w, L::T("scout.f.race"), RaceText(r.sys, &r.words));
    Fact(dl, x, y, w, L::T("scout.f.planets"), std::to_string(r.planets));
    Fact(dl, x, y, w, L::T("scout.f.glyphs"), Glyphs(r.ua));
    auto why = Reasons(r.sys, g.freq);
    if (!why.empty()) {
        std::string j;
        for (auto& s : why) j += (j.empty() ? "" : ", ") + s;
        Fact(dl, x, y, w, L::T("scout.f.rare"), j, P().gold);
    }
    y += S(6);
    for (auto& pl : r.rows) {
        dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), P().line, S(1));
        y += S(8);
        std::string head = pl.name + (pl.biome.empty() ? "" : "   /   " + pl.biome) + (pl.subtype.empty() ? "" : "   /   " + pl.subtype);
        ImVec2 hs = ui::theme::Title()->CalcTextSizeA(S(17), FLT_MAX, tw, head.c_str());
        dl->AddText(ui::theme::Title(), S(17), ImVec2(x, y), P().text, head.c_str(), nullptr, tw);
        if (pl.exotic || pl.special) {
            std::string st = ui::Utf8(icon::Star);
            dl->AddText(ui::theme::Body(), S(13), ImVec2(x + std::min(hs.x, tw) + S(8), y + S(2)), P().gold, st.c_str());
        }
        y += hs.y + S(4);
        Fact(dl, x, y, w, L::T("scout.p.type"), pl.type);
        Fact(dl, x, y, w, L::T("scout.p.weather"), pl.weather, pl.extreme ? P().gold : 0);
        Fact(dl, x, y, w, L::T("scout.p.sentinels"), pl.sentinels);
        Fact(dl, x, y, w, L::T("scout.p.resources"), pl.resources);
        Fact(dl, x, y, w, L::T("scout.p.flora"), pl.flora);
        Fact(dl, x, y, w, L::T("scout.p.fauna"), pl.fauna, pl.special ? P().gold : 0);
        y += S(4);
    }
    if (r.rows.empty() && r.planets > 0) {
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, L::T("scout.planets.none"));
        y += S(22);
    }
    ImGui::Dummy(ImVec2(w, y + S(10) - p.y));
}

void DrawList(const char* query)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(8);
    // two views, so neither hides below the other
    {
        size_t n;
        { std::lock_guard lk(g_mx); n = g_found.size(); }
        std::string nearby = L::T("scout.nearby");
        if (n) nearby += "  (" + std::to_string(n) + ")";
        float bw = (w - S(8)) / 2;
        ImGui::SetCursorScreenPos(p);
        if (ui::Button("##tabhere", L::T("scout.here"), bw, g.tab == 0 ? ui::Tone::Teal : ui::Tone::Neutral)) g.tab = 0;
        ImGui::SetCursorScreenPos(ImVec2(p.x + bw + S(8), p.y));
        if (ui::Button("##tabnear", nearby.c_str(), bw, g.tab == 1 ? ui::Tone::Teal : ui::Tone::Neutral)) g.tab = 1;
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(46)));
    }
    uint64_t v = g_version;
    double now = ImGui::GetTime();
    // while scanning, rebuild a few times a second rather than every frame
    if (g.f != g.lastF || (v != g.version && (!g_scanning || now - g.lastBuild > 0.4))) {
        g.version = v;
        g.lastF = g.f;
        g.lastBuild = now;
        Rebuild();
    }
    if (g.tab == 0) { DrawReport(); return; }
    if (g.view.empty()) {
        const char* t = g_scanning ? L::T("scout.scanning") : g_version ? L::T("scout.empty") : L::T("scout.start");
        dl->AddText(ui::theme::Body(), S(15), ImGui::GetCursorScreenPos(), P().textFaint, t);
        ImGui::Dummy(ImVec2(w, S(24)));
        return;
    }
    g.browser.Draw(g.view, query);
}

float DrawInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = g.browser.Focused(g.view);
    const Sys* sel = nullptr;
    if (f)
        for (size_t i = 0; i < g.view.size(); ++i)
            if (g.view[i].key == f->key) { sel = &g.shown[i]; break; }
    ui::BrowserEntry head;
    if (f) head = *f;
    else {
        head.title = L::T("tab.scout");
        head.subtitle = L::T("scout.subtitle");
        head.body = L::T("scout.help");
        head.glyph = icon::Binoculars;
    }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    uint64_t here = 0;
    bool live = game::FrameHookReady() && CurrentAddress(here);

    if (sel) {
        Fact(dl, x, y, bw, L::T("scout.f.star"), Pretty(O.star.Of(sel->star)));
        const Words* words = WordsFor(*sel);
        Fact(dl, x, y, bw, L::T("scout.f.economy"), EconomyText(*sel, words));
        Fact(dl, x, y, bw, L::T("scout.f.conflict"), ConflictText(*sel, words));
        Fact(dl, x, y, bw, L::T("scout.f.race"), RaceText(*sel, words));
        Fact(dl, x, y, bw, L::T("scout.f.planets"),
             sel->prime > 0 ? L::Tf("scout.planets.prime", std::to_string(sel->planets), std::to_string(sel->prime)) : std::to_string(sel->planets));
        if (sel->nBiomes > 0) {
            std::string list;
            for (int k = 0; k < sel->nBiomes; ++k) {
                std::string b = Pretty(O.biome.Of(sel->biome[k])), sb = O.subtype.Of(sel->sub[k]);
                std::string ls = Lower(sb);
                if (!sb.empty() && ls != "none" && ls != "standard" && ls != "highquality" && ls.rfind("variant", 0) != 0 && ls.rfind("remix", 0) != 0)
                    b += " (" + Pretty(sb) + ")";
                list += (list.empty() ? "" : ", ") + b;
            }
            Fact(dl, x, y, bw, L::T("scout.f.worlds"), list);
        }
        Fact(dl, x, y, bw, L::T("scout.f.distance"), sel->dist < 0.5f ? L::T("scout.sameregion") : L::Tf("scout.ly", Ly(sel->dist)));
        Fact(dl, x, y, bw, L::T("scout.f.glyphs"), Glyphs(sel->ua));
        auto why = Reasons(*sel, g.freq);
        if (!why.empty()) {
            std::string j;
            for (auto& s : why) j += (j.empty() ? "" : ", ") + s;
            Fact(dl, x, y, bw, L::T("scout.f.rare"), j, P().gold);
        }
        y += S(8);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool same = live && (here & 0x000FFFFFFFFFFFFFull) == (sel->ua & 0x000FFFFFFFFFFFFFull);
        if (same) ui::Button("##scouthere", L::T("scout.youarehere"), bw, ui::Tone::Neutral, false);
        else if (!live) ui::Button("##scoutno", L::T("tp.notready"), bw, ui::Tone::Neutral, false);
        else if (ui::HoldButton("##scoutwarp", L::T("tp.warpgo"), bw)) {
            WarpToAddress(sel->ua, SysName(*sel));
            ui::Toast(L::T("tp.warping"), SysName(*sel) + "   " + Glyphs(sel->ua), icon::Binoculars);
        }
        y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##scoutcopy", L::T("tp.copy"), bw, ui::Tone::Neutral, true)) {
            std::string s = L::Tf("tp.addr", std::to_string(((sel->ua >> 32) & 0xFF) + 1), Glyphs(sel->ua));
            ImGui::SetClipboardText(s.c_str());
            ui::Toast(L::T("tp.copied"), s, icon::Pin);
        }
        y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##scoutsave", L::T("scout.savespot"), bw, ui::Tone::Neutral, true)) {
            // no distance in the name: it changes once the player moves
            std::string name = SysName(*sel) + ", " + Pretty(O.star.Of(sel->star));
            SaveSystemSpot(sel->ua, name);
            ui::Toast(L::T("scout.saved"), name, icon::Pin);
        }
        y += S(46);
    }
    else {
        // scan controls
        static std::vector<std::string> radii;
        if (radii.empty()) radii = { L::T("scout.radius.0"), L::T("scout.radius.1"), L::T("scout.radius.2") };
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::Dropdown("##radius", radii, &g.f.radius, bw);
        y += S(42);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool can = live && g_voxel && g_star && g_check && !g_scanning;
        if (ui::Button("##scan", L::T(g_scanning ? "scout.scanning" : "scout.scan"), bw, ui::Tone::Teal, can)) {
            g.browser.ClearFocus();
            g_request = g.f.radius;
            g.tab = 1;
        }
        y += S(42);
        if (g_scanning || g_version) {
            size_t n;
            { std::lock_guard lk(g_mx); n = g_found.size(); }
            std::string s = L::Tf("scout.progress", std::to_string(g_regionsDone.load()) + " / " + std::to_string(g_regionsTotal.load()),
                                  std::to_string(n));
            dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textDim, s.c_str());
            y += S(24);
        }
        if (g_ready) {
            dl->AddLine(ImVec2(x, y + S(2)), ImVec2(x + bw, y + S(2)), P().line, S(1));
            y += S(12);
            static std::vector<std::string> stars, races, trades, wealths, conflicts, special, planets, kinds;
            if (stars.empty()) {
                stars = Options(O.star);
                races = Options(O.race);
                trades = Options(O.trade);
                wealths = Options(O.wealth);
                conflicts = Options(O.conflict);
                special = { L::T("scout.any"), L::T("scout.s.rare"), L::T("scout.r.hole"), L::T("scout.r.atlas"), L::T("scout.r.purple"),
                            L::T("scout.r.abandoned"), L::T("scout.r.pirate"), L::T("scout.s.giant") };
                planets = { L::T("scout.any"), "2+", "3+", "4+", "5+", "6" };
                kinds = { L::T("scout.any") };
                for (const char* k : kPlanetKindKeys) kinds.push_back(L::T(k));
            }
            float lw = S(110), dw = bw - lw;
            auto row = [&](const char* label, const char* id, const std::vector<std::string>& items, int* v) {
                dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(8)), P().textDim, L::T(label));
                ImGui::SetCursorScreenPos(ImVec2(x + lw, y));
                ui::Dropdown(id, items, v, dw);
                y += S(42);
            };
            row("scout.s.special", "##fspecial", special, &g.f.special);
            row("scout.f.star", "##fstar", stars, &g.f.star);
            row("scout.f.race", "##frace", races, &g.f.race);
            row("scout.f.economy", "##ftrade", trades, &g.f.trade);
            row("scout.f.wealth", "##fwealth", wealths, &g.f.wealth);
            row("scout.f.conflict", "##fconflict", conflicts, &g.f.conflict);
            row("scout.f.planets", "##fplanets", planets, &g.f.planets);
            if (g_sysBuf) row("scout.f.worlds", "##fkind", kinds, &g.f.planetKind);
        }
    }
    const char* note = L::T("scout.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
    return y;
}

}

bool CurrentSystemNames(uint64_t& ua, std::string& system, std::vector<std::string>& planets)
{
    uintptr_t d = game::Data(), sys = 0;
    if (!d || !Rd(d + g_sysPtr, sys) || !sys || !CurrentAddress(ua) || !ua) return false;
    InitOffsets();
    system = Str(sys + O.yName, 0x80);
    if (!Printable(system)) system.clear();
    int32_t n = 0;
    Rd(sys + O.yPlanets, n);
    planets.clear();
    for (int i = 0; i < std::clamp(n, 0, 6); ++i) {
        uintptr_t pd = sys + g_planets + (uintptr_t)i * g_stride + kPlanetData;
        int32_t idx = -1;
        if (!Rd(pd + O.pIndex, idx) || idx != i) break;
        std::string name = Str(pd + O.pName, 0x80);
        planets.push_back(Printable(name) ? name : std::string());
    }
    return true;
}

void InstallScout()
{
    if (const Target& t = Loc().Resolve("Galaxy.ClassifyVoxel"); t.ok) g_voxel = (ClassifyVoxelFn)Abs(t.func);
    else log::Warn("Scout", "ClassifyVoxel missing: {}", t.reason);
    if (const Target& t = Loc().Resolve("Galaxy.ClassifyStarSystem"); t.ok) g_star = (ClassifyStarFn)Abs(t.func);
    else log::Warn("Scout", "ClassifyStarSystem missing: {}", t.reason);
    if (const Target& t = Loc().Resolve("Portal.CheckUAIsValid"); t.ok) g_check = (CheckUAFn)Abs(t.func);
    else log::Warn("Scout", "Address check missing: {}", t.reason);
    if (const Target& t = Loc().Resolve("Data.SolarSystem"); t.ok && t.offset) g_sysPtr = *t.offset;
    if (const Target& t = Loc().Resolve("SolarSystem.Planets"); t.ok && t.offset) g_planets = *t.offset;
    if (const Target& t = Loc().Resolve("SolarSystem.PlanetStride"); t.ok && t.offset) g_stride = *t.offset;
    if (const Target& t = Loc().Resolve("SolarSystem.Generator"); t.ok && t.offset) g_generator = *t.offset;
    if (const Target& t = Loc().Resolve("SolarSystemData.SetDefaults"); t.ok) g_sysDefaults = (SysDataFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("SolarSystemData.Destroy"); t.ok) g_sysDestroy = (SysDataFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Galaxy.GenerateBiomesInfo"); t.ok) g_genBiomes = (GenBiomesFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Galaxy.AttributeStrings"); t.ok) g_attrStrings = (AttrStringsFn)Abs(t.func);
    else log::Warn("Scout", "Galaxy map words missing, using enum names: {}", t.reason);
    if (g_sysDefaults && g_sysDestroy && g_genBiomes) g_sysBuf = (uint8_t*)_aligned_malloc(kSysBuf, 16);
    log::Info("Scout", "Planet biomes {}, generator +0x{:X}", g_sysBuf ? "ready" : "unavailable", g_generator);
    log::Info("Scout", "System +0x{:X}, planets +0x{:X} x 0x{:X}, scan {}", g_sysPtr, g_planets, g_stride,
              g_voxel && g_star && g_check ? "ready" : "unavailable");
    game::EveryFrame(Tick);

    Registry::Get().Post([] {
        g.browser.chips = { L::T("scout.chip.all"), L::T("scout.chip.rare") };
        g.browser.captions = true;
        g.browser.sortable = true;
        g.browser.sortOptions = { { ui::SortBy::Value, L::T("scout.sort.scan") }, { ui::SortBy::Rarity, L::T("scout.sort.rarity") },
                                  { ui::SortBy::Name, L::T("browser.sort.name") }, { ui::SortBy::Type, L::T("scout.sort.star") } };
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { std::lock_guard lk(g_mx); return g_found.empty() ? std::string() : std::to_string(g_found.size()); };
        ui::RegisterPage("scout", std::move(page));
    });
}

}
