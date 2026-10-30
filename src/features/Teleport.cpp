#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "core/Settings.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <cctype>
#include <cstdlib>
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <mutex>

namespace os::features {

namespace {

using ui::theme::P;
using ui::theme::S;
using json = nlohmann::json;

// cGcPlayer and the player state's location, relative to mpData.
int64_t g_player = 0x72CDE0, g_loc = 0xC1A0, g_sysPtr = 0x72B610; // g_loc: PlayerState.Location
// cGcUniverseAddressData / cGcGalacticAddressData fields, from the reflection data
struct { uint32_t planet = 0, system = 4, x = 8, y = 0xC, z = 0x10, galaxy = 0x14; } A;
constexpr uint32_t kSysAddress = 0x26B0;
constexpr uint32_t kMatrix = 0x310, kBigPos = 0x360;

struct alignas(16) Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct alignas(16) BigPos { Vec4 local, offset; };

struct Mark
{
    std::string key, name;
    uint64_t ua = 0;
    BigPos pos;
    Vec4 dir;
    bool noPos = false; // a system saved from Scout: warp there, no ground position
};

using SetToPositionFn = void (*)(uintptr_t player, const BigPos* pos, const Vec4* dir, const Vec4* vel);
SetToPositionFn g_setPos = nullptr;

std::mutex g_mx;
std::vector<Mark> g_marks;
bool g_loaded = false;
uint64_t g_nextKey = 1;

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    bool dirty = true;
    char newName[64] = {};
    char rename[64] = {};
    char addr[64] = {};
    int galaxy = -1; // -1 follows the current galaxy
    std::string renameKey; // spot the rename buffer was filled from
} g;

std::filesystem::path File() { return std::filesystem::path(paths::Root()) / L"OpenSkies.bookmarks.json"; }

void Load()
{
    g_loaded = true;
    std::ifstream in(File());
    if (!in) return;
    try {
        json j = json::parse(in);
        for (auto& m : j) {
            Mark k;
            k.key = std::to_string(g_nextKey++);
            k.name = m.value("name", "Spot");
            k.ua = m.value("ua", 0ull);
            k.noPos = m.value("nopos", false);
            auto v = [](const json& a, Vec4& o) {
                if (a.is_array() && a.size() >= 3) { o.x = a[0]; o.y = a[1]; o.z = a[2]; }
            };
            v(m["local"], k.pos.local);
            v(m["offset"], k.pos.offset);
            v(m["dir"], k.dir);
            g_marks.push_back(std::move(k));
        }
    }
    catch (...) { log::Warn("Teleport", "Bookmarks file could not be read"); }
}

void Save()
{
    json j = json::array();
    for (auto& m : g_marks) {
        auto v = [](const Vec4& o) { return json::array({ o.x, o.y, o.z }); };
        json e = { { "name", m.name }, { "ua", m.ua }, { "local", v(m.pos.local) }, { "offset", v(m.pos.offset) }, { "dir", v(m.dir) } };
        if (m.noPos) e["nopos"] = true;
        j.push_back(std::move(e));
    }
    std::ofstream(File()) << j.dump(1);
}

template <class T> bool RdObj(uintptr_t a, T& v)
{
    __try { memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Finite(const Vec4& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) < 1e9f && std::fabs(v.y) < 1e9f && std::fabs(v.z) < 1e9f; }

bool ReadHere(uint64_t& ua, BigPos& pos, Vec4& dir)
{
    uintptr_t d = game::Data();
    if (!d) return false;
    uintptr_t p = d + g_player;
    if (!CurrentAddress(ua) || !RdObj(p + kBigPos, pos) || !RdObj(p + kMatrix + 0x20, dir)) return false;
    return Finite(pos.local) && Finite(pos.offset) && Finite(dir);
}

uint64_t SystemOf(uint64_t ua) { return ua & 0x000FFFFFFFFFFFFFull; }

std::string Where(uint64_t ua)
{
    return L::Tf("tp.where", std::to_string(((ua >> 32) & 0xFF) + 1), std::to_string((ua >> 40) & 0xFFF)) + ", " +
           L::Tf("tp.planet", std::to_string((ua >> 52) & 0xF));
}

// 12 hex digit portal address: planet, system, Y, Z, X.
std::string Glyphs(uint64_t ua)
{
    char b[16];
    snprintf(b, sizeof b, "%01X%03X%02X%03X%03X", (unsigned)((ua >> 52) & 0xF), (unsigned)((ua >> 40) & 0xFFF),
             (unsigned)((ua >> 24) & 0xFF), (unsigned)((ua >> 12) & 0xFFF), (unsigned)(ua & 0xFFF));
    return b;
}

// "Spot N" with the lowest N not already used.
std::string FreeName()
{
    for (int n = 1;; ++n) {
        std::string s = L::Tf("tp.spotname", std::to_string(n));
        bool used = false;
        for (auto& m : g_marks) if (m.name == s) { used = true; break; }
        if (!used) return s;
    }
}

// ---- warp to any system or galaxy, through the game's teleporter request ----

// Data offsets of cGcPlayer's warp request: jump target, endpoint, mode.
int64_t g_reqJump = 0x72F120, g_reqEndpoint = 0x72F150, g_reqMode = 0x72F1E0;
// cGcSpaceshipWarp; Warp.ArrivalFlag is set while it holds an arrival endpoint, Reset clears it.
int64_t g_warp = 0x72B630, kWarpArrival = 0x710;

int Signed(uint64_t v, int bits) { int s = (int)(v & ((1ull << bits) - 1)); return s >= (1 << (bits - 1)) ? s - (1 << bits) : s; }

uint64_t Pack(int x, int y, int z, int system, int galaxy, int planet)
{
    return (uint64_t)(x & 0xFFF) | (uint64_t)(z & 0xFFF) << 12 | (uint64_t)(y & 0xFF) << 24 | (uint64_t)(galaxy & 0xFF) << 32 |
           (uint64_t)(system & 0xFFF) << 40 | (uint64_t)(planet & 0xF) << 52;
}

// A typed address: 12 glyph hex digits, or XXXX:YYYY:ZZZZ:SSSS. Galaxy comes from the picker,
// or from a copied "Galaxy N ..." line.
bool ParseAddress(std::string s, int& galaxy, uint64_t& ua)
{
    if (size_t g = s.find("Galaxy"); g != std::string::npos) {
        int n = atoi(s.c_str() + g + 6);
        if (n >= 1 && n <= 256) galaxy = n - 1;
        s = s.substr(g + 6);
        while (!s.empty() && (isdigit((unsigned char)s[0]) || s[0] == ' ')) s.erase(0, 1);
    }
    std::string hex, parts[4];
    int part = 0;
    bool colon = s.find(':') != std::string::npos;
    for (char c : s) {
        if (isxdigit((unsigned char)c)) { hex += (char)toupper((unsigned char)c); if (colon && part < 4) parts[part] += c; }
        else if (c == ':' && ++part > 3) return false;
    }
    if (colon) {
        if (part != 3) return false;
        unsigned v[4];
        for (int i = 0; i < 4; ++i) {
            if (parts[i].empty() || parts[i].size() > 4) return false;
            v[i] = (unsigned)strtoul(parts[i].c_str(), nullptr, 16);
        }
        int x = (int)v[0] - 0x7FF, y = (int)v[1] - 0x7F, z = (int)v[2] - 0x7FF;
        if (x < -2047 || x > 2047 || z < -2047 || z > 2047 || y < -127 || y > 127 || v[3] > 0xFFF) return false;
        ua = Pack(x, y, z, (int)v[3], galaxy, 0);
        return true;
    }
    if (hex.size() != 12) return false;
    uint64_t g = strtoull(hex.c_str(), nullptr, 16);
    int planet = (int)(g >> 44) & 0xF, sys = (int)(g >> 32) & 0xFFF;
    if (planet > 6) return false; // a system has at most 6 planets; a higher index crashes the arrival
    int y = Signed(g >> 24, 8), z = Signed(g >> 12, 12), x = Signed(g, 12);
    ua = Pack(x, y, z, sys, galaxy, planet);
    return true;
}

struct Pending
{
    bool active = false, hasPos = false;
    bool taken = false, inArrival = false, landed = false; // stages, each logged once
    uint64_t ua = 0, start = 0, settled = 0;
    BigPos pos;
    Vec4 dir;
} g_pending; // game thread

// The player's forward row, unit length. The arrival builds a matrix from the endpoint facing,
// and a zero facing normalizes to NaN.
Vec4 Forward(uintptr_t d)
{
    Vec4 f, m;
    f.z = 1.f;
    if (RdObj(d + g_player + kMatrix + 0x20, m) && Finite(m)) {
        float l = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
        if (l > 1e-3f) { f.x = m.x / l; f.y = m.y / l; f.z = m.z / l; }
    }
    return f;
}

// Game thread. Fills the jump target and endpoint the way the game's own writers do (0x179A170, 0x6F0450),
// then the mode last.
// mode 2: the teleporter request (any galaxy, on foot). mode 1: the hyperdrive request (same galaxy, by ship).
bool RequestWarp(uint64_t ua, const std::string& name, uint32_t reqMode)
{
    uintptr_t d = game::Data();
    uint32_t mode = 1;
    if (!d || !Rd(d + g_reqMode, mode) || mode != 0) return false;
    int x = Signed(ua, 12), z = Signed(ua >> 12, 12), y = Signed(ua >> 24, 8);
    int galaxy = (int)(ua >> 32) & 0xFF, sys = (int)(ua >> 40) & 0xFFF;
    Vec4 fwd = Forward(d);

    uint8_t jt[0x30] = {};
    int16_t v16[3] = { (int16_t)x, (int16_t)z, (int16_t)y };
    memcpy(jt, v16, 6);
    jt[6] = 1;
    uint16_t s16 = (uint16_t)sys, g16 = (uint16_t)galaxy;
    memcpy(jt + 8, &s16, 2);
    memcpy(jt + 0xA, &g16, 2);
    memcpy(jt + 0x10, &fwd, 16); // 0x6F0450 puts the player's forward row here
    float dist = -1.f;
    memcpy(jt + 0x20, &dist, 4);

    uint8_t ep[0x90] = {};
    Vec4 origin;
    origin.w = 1.f; // the endpoint constructor (0x294B370) starts from {0,0,0,1}
    memcpy(ep + 0x00, &fwd, 16);
    memcpy(ep + 0x10, &origin, 16);
    // planet 0: a saved spot's planet comes from mCurrentUA, which isn't always a valid index on arrival
    int32_t uaData[6] = { 0, sys, x, y, z, galaxy }; // cGcUniverseAddressData
    memcpy(ep + 0x20, uaData, sizeof uaData);
    strncpy_s((char*)ep + 0x4C, 64, name.c_str(), _TRUNCATE);
    ep[0x8C] = 1; // the game picks the arrival spot

    for (size_t i = 0; i < sizeof jt; ++i) Wr<uint8_t>(d + g_reqJump + i, jt[i]);
    if (reqMode == 2) for (size_t i = 0; i < sizeof ep; ++i) Wr<uint8_t>(d + g_reqEndpoint + i, ep[i]);
    Wr<uint32_t>(d + g_reqMode, reqMode);
    log::Info("Teleport", "Warp requested (mode {}) to galaxy {} system 0x{:X}, facing {:.2f} {:.2f} {:.2f}", reqMode, galaxy + 1, sys, fwd.x, fwd.y,
              fwd.z);
    return true;
}

// Game thread, every frame: follows the warp and, for a saved spot, puts the player there once the arrival is over.
void PendingTick()
{
    if (!g_pending.active) return;
    auto& p = g_pending;
    uint64_t now = game::Frame();
    uintptr_t d = game::Data();
    if (!d || now - p.start > 60 * 180) {
        log::Warn("Teleport", "Arrival not confirmed, kept the game's own spot");
        p.active = false;
        return;
    }
    uint32_t mode = 1;
    uint8_t arrival = 0;
    uint64_t here = 0;
    if (!Rd(d + g_reqMode, mode) || !Rd(d + g_warp + kWarpArrival, arrival) || !CurrentAddress(here)) return;
    if (mode != 0) return;
    if (!p.taken) { p.taken = true; log::Info("Teleport", "Request taken, arrival flag {}", arrival); }
    if (arrival && !p.inArrival) { p.inArrival = true; log::Info("Teleport", "Arrival under way"); }
    // the planet nibble follows the nearest planet, so compare the system only
    bool there = SystemOf(here) == SystemOf(p.ua);
    if (!p.inArrival || arrival || !there) { p.settled = 0; return; }
    if (!p.landed) { p.landed = true; log::Info("Teleport", "Arrived in system 0x{:X}", (here >> 40) & 0xFFF); }
    if (!p.hasPos || !g_setPos) { p.active = false; return; }
    if (!p.settled) p.settled = now;
    if (now - p.settled < 300) return; // let the planet and terrain stream in
    p.active = false;
    BigPos at;
    Vec4 look;
    uint64_t ua = 0;
    if (!ReadHere(ua, at, look)) { log::Warn("Teleport", "Player not readable, kept the game's own spot"); return; }
    BigPos to = p.pos;
    to.local.y += 2.f;
    Vec4 vel;
    g_setPos(d + g_player, &to, &p.dir, &vel);
    log::Info("Teleport", "Placed at the saved spot");
}

// cGcPlayer.mePlayerMode: 1 in a ship, 2 on foot.
// The teleporter request (mode 2) always lands on foot and broke the HUD from the cockpit, so ship warps don't use it.
int64_t g_playerMode = 0x56AC;
int32_t g_nexusExit = 0xC; // eWarpEffectType NexusExit: the Anomaly take-off, the game's one ship warp that changes galaxy
using WarpDistantFn = void (*)(void* warp, const void* jumpTarget, int32_t effect, bool b);
using GoToGalaxyFn = void (*)(void* ps, int32_t galaxy);
WarpDistantFn g_warpDistant = nullptr;
GoToGalaxyFn g_goToGalaxy = nullptr;

bool InShip()
{
    uintptr_t d = game::Data();
    uint32_t m = 0;
    return d && Rd(d + g_player + g_playerMode, m) && m == 1;
}

// Game thread. In the ship, to any galaxy: the same calls the Anomaly take-off makes. Warp out first, then the galaxy.
bool ShipWarp(uint64_t ua)
{
    uintptr_t d = game::Data(), ps = game::PlayerState();
    uint32_t mode = 1, phase = 1;
    if (!d || !ps || !g_warpDistant || !Rd(d + g_reqMode, mode) || mode != 0 || !Rd(d + g_warp + 0x98, phase) || phase != 0) return false;
    uint64_t here = 0;
    CurrentAddress(here);
    int x = Signed(ua, 12), z = Signed(ua >> 12, 12), y = Signed(ua >> 24, 8);
    int galaxy = (int)(ua >> 32) & 0xFF, sys = (int)(ua >> 40) & 0xFFF;
    uint8_t jt[0x30] = {};
    int16_t v16[3] = { (int16_t)x, (int16_t)z, (int16_t)y };
    memcpy(jt, v16, 6);
    jt[6] = 1;
    uint16_t s16 = (uint16_t)sys, g16 = (uint16_t)galaxy;
    memcpy(jt + 8, &s16, 2);
    memcpy(jt + 0xA, &g16, 2);
    Vec4 fwd = Forward(d);
    memcpy(jt + 0x10, &fwd, 16);
    float dist = -1.f;
    memcpy(jt + 0x20, &dist, 4);
    for (size_t i = 0; i < sizeof jt; ++i) Wr<uint8_t>(d + g_reqJump + i, jt[i]); // keep the game's copy in step
    bool other = galaxy != (int)((here >> 32) & 0xFF);
    g_warpDistant((void*)(d + g_warp), jt, other ? g_nexusExit : 0, false);
    if (other && g_goToGalaxy) g_goToGalaxy((void*)ps, galaxy);
    log::Info("Teleport", "Ship warp to galaxy {} system 0x{:X}{}", galaxy + 1, sys, other ? " (new galaxy)" : "");
    return true;
}

// ---- real stars and the galaxy map ----

// cGcFrontendPagePortalRunes::CheckUAIsValid: true if ua is a real star; *out gets a real system in that region.
using CheckUAFn = bool (*)(uint64_t ua, uint64_t* out, bool seeded, bool strict);
// cGcGalaxyMap::TriggerGalacticMapLocationSyncToGameState: recentres the map on PS.mLocation.
using SyncMapFn = void (*)();
CheckUAFn g_checkUA = nullptr;
SyncMapFn g_syncMap = nullptr;

// Toasts raised on the game thread, shown by the page.
std::mutex g_noteMx;
std::vector<std::pair<std::string, std::string>> g_notes;

void Note(std::string title, std::string body)
{
    std::lock_guard lk(g_noteMx);
    g_notes.emplace_back(std::move(title), std::move(body));
}

void DrainNotes()
{
    std::lock_guard lk(g_noteMx);
    for (auto& [t, b] : g_notes) ui::Toast(t, b, icon::Pin);
    g_notes.clear();
}

bool TryCheckUA(uint64_t ua, uint64_t* out)
{
    __try { g_checkUA(ua, out, true, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Game thread. Seeded, so the same address always gives the same system. The check also flags
// planet 0 (planets count from 1), so compare the system it returns, not its answer.
bool RealStar(uint64_t ua, uint64_t& out)
{
    out = ua;
    if (!g_checkUA || !TryCheckUA(ua, &out)) { out = ua; return true; }
    return SystemOf(out) == SystemOf(ua);
}

bool TrySyncMap()
{
    __try { g_syncMap(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SyncMap()
{
    if (g_syncMap && !TrySyncMap()) log::Warn("Teleport", "Galaxy map sync failed");
}

// Game thread. A typed address can name a system its region doesn't have. The game still loads it,
// but the galaxy map has no star there and stays on the old spot. Use the system a portal would pick.
uint64_t Corrected(uint64_t ua)
{
    uint64_t real = ua;
    if (RealStar(ua, real)) return ua;
    real = (real & 0x000FFFFFFFFFFFFFull & ~(0xFFull << 32)) | (ua & (0xFFull << 32)); // same galaxy, planet 0
    log::Info("Teleport", "No star at {}, using {}", Glyphs(ua), Glyphs(real));
    Note(L::T("tp.corrected"), Where(real) + "   " + Glyphs(real));
    return real;
}

// Game thread: once a warp has settled in its target system, recentre the galaxy map.
struct
{
    bool active = false, ship = false;
    uint64_t ua = 0, start = 0, settled = 0;
} g_mapSync;

void MapSyncTick()
{
    auto& s = g_mapSync;
    if (!s.active) return;
    uint64_t now = game::Frame();
    uintptr_t d = game::Data();
    if (!d || now - s.start > 60 * 180) { s.active = false; return; }
    uint32_t mode = 1, phase = 1;
    uint8_t arrival = 1;
    uint64_t here = 0;
    if (!Rd(d + g_reqMode, mode) || !Rd(d + g_warp + 0x98, phase) || !Rd(d + g_warp + kWarpArrival, arrival) || !CurrentAddress(here)) return;
    // only the teleporter arrival sets the endpoint flag
    if (mode || phase || (arrival && !s.ship) || SystemOf(here) != SystemOf(s.ua)) { s.settled = 0; return; }
    if (!s.settled) s.settled = now;
    if (now - s.settled < 120) return;
    s.active = false;
    SyncMap();
    log::Info("Teleport", "Galaxy map moved to system 0x{:X}", (here >> 40) & 0xFFF);
}

// Game thread. Ship: arrive with it, in space. On foot: the teleporter request, then the saved spot if any.
void StartWarp(uint64_t ua, const std::string& name, Pending p, bool ship)
{
    if (ship) {
        if (!ShipWarp(ua)) { log::Warn("Teleport", "A warp is already under way"); return; }
    }
    else {
        if (!RequestWarp(ua, name, 2)) { log::Warn("Teleport", "A warp is already under way"); return; }
        p.active = true;
        p.ua = ua;
        p.start = game::Frame();
        g_pending = p;
    }
    g_mapSync = {};
    g_mapSync.active = true;
    g_mapSync.ship = ship;
    g_mapSync.ua = ua;
    g_mapSync.start = game::Frame();
}

void Warp(uint64_t ua, std::string name, const Mark* spot)
{
    bool ship = InShip();
    if (!ship) game::BackupSaveOnce();
    Pending p;
    // a saved spot's ground position doesn't apply from the ship
    if (spot && !ship) { p.hasPos = true; p.pos = spot->pos; p.dir = spot->dir; }
    bool typed = !spot;
    game::Post([ua, name, p, ship, typed] { StartWarp(typed ? Corrected(ua) : ua, name, p, ship); });
}

// The galaxy map stuck on an old spot: recentre it. If the player stands in a system that isn't a real
// star, the map has nothing to centre on, so warp to the real system the portals would use instead.
void FixMap()
{
    bool ship = InShip();
    game::Post([ship] {
        uintptr_t d = game::Data();
        uint64_t here = 0, real = 0;
        if (!d || !CurrentAddress(here)) return;
        if (RealStar(SystemOf(here), real)) {
            SyncMap();
            Note(L::T("tp.mapfixed"), Where(here));
            return;
        }
        real = (real & 0x000FFFFFFFFFFFFFull & ~(0xFFull << 32)) | (here & (0xFFull << 32));
        log::Info("Teleport", "Standing in {}, not a real star; warping to {}", Glyphs(here), Glyphs(real));
        Note(L::T("tp.mapfix.warp"), Where(real) + "   " + Glyphs(real));
        if (!ship) game::BackupSaveOnce();
        StartWarp(real, "", Pending{}, ship);
    });
}

std::string GlyphIcon(int k)
{
    char p[80];
    snprintf(p, sizeof p, "TEXTURES/UI/FRONTEND/ICONS/UPDATE3/PORTALSYMBOL.%X.DDS", k);
    return p;
}

// One glyph in a w x h cell, per the address pad setting: the game's icon, its hex digit, or both.
// k 16 is the delete key.
void DrawGlyph(ImDrawList* dl, ImVec2 a, float w, float h, int k)
{
    int mode = Settings::Get().Data().glyphMode;
    char lbl[2] = { k < 16 ? "0123456789ABCDEF"[k] : '<', 0 };
    ImTextureID t = k < 16 && mode != 1 ? ui::LoadIcon(GlyphIcon(k)) : ImTextureID{};
    if (!t) {
        float fs = std::min(S(16), h * 0.6f);
        ImVec2 ts = ui::theme::Body()->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
        dl->AddText(ui::theme::Body(), fs, ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), P().text, lbl);
        return;
    }
    float side = std::min(w, h) - S(6), ox = (w - side) * 0.5f, oy = (h - side) * 0.5f;
    dl->AddImage(ImTextureRef(t), ImVec2(a.x + ox, a.y + oy), ImVec2(a.x + ox + side, a.y + oy + side));
    if (mode != 2) return;
    float fs = std::max(S(10), h * 0.28f);
    ImVec2 ts = ui::theme::Body()->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
    ImVec2 at(a.x + w - ts.x - S(3), a.y + h - ts.y - S(1));
    dl->AddText(ui::theme::Body(), fs, ImVec2(at.x + S(1), at.y + S(1)), IM_COL32(0, 0, 0, 200), lbl);
    dl->AddText(ui::theme::Body(), fs, at, P().textDim, lbl);
}

const char* kGalaxyNames[] = { "Euclid", "Hilbert Dimension", "Calypso", "Hesperius Dimension", "Hyades", "Ickjamatew", "Budullangr",
                               "Kikolgallr", "Eltiensleen", "Eissentam" };

void DrawList(const char* query)
{
    DrainNotes();
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    uint64_t here = 0;
    BigPos pos;
    Vec4 dir;
    bool live = ReadHere(here, pos, dir);
    static uint64_t lastSys = 0;
    if (live && SystemOf(here) != lastSys) { lastSys = SystemOf(here); g.dirty = true; }
    if (g.dirty) {
        g.dirty = false;
        g.view.clear();
        for (auto& m : g_marks) {
            ui::BrowserEntry b;
            b.key = m.key;
            b.title = m.name;
            b.subtitle = Where(m.ua) + "   " + Glyphs(m.ua);
            b.glyph = icon::Pin;
            b.marked = live && SystemOf(m.ua) == SystemOf(here);
            b.chip = b.marked ? 1 : 2;
            g.view.push_back(std::move(b));
        }
        g.browser.Invalidate();
    }
    g.browser.Draw(g.view, query);
}

float DrawInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::lock_guard lk(g_mx);
    const ui::BrowserEntry* f = g.browser.Focused(g.view);
    Mark sel;
    if (f) for (auto& m : g_marks) if (m.key == f->key) sel = m;
    ui::BrowserEntry head;
    if (f) head = *f;
    else {
        head.title = L::T("tab.teleport");
        head.subtitle = L::T("tp.subtitle");
        head.body = L::T("tp.help");
        head.glyph = icon::Pin;
    }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2;
    uint64_t here = 0;
    BigPos pos;
    Vec4 dir;
    bool live = game::FrameHookReady() && ReadHere(here, pos, dir);

    // a new spot, always available whatever is focused
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    bool enter = ui::TextField("##newname", g.newName, sizeof g.newName, bw, FreeName().c_str());
    y += S(42);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    if ((ui::Button("##save", L::T("tp.save"), bw, ui::Tone::Green, live) || (enter && live))) {
        Mark m;
        m.key = std::to_string(g_nextKey++);
        m.name = g.newName[0] ? std::string(g.newName) : FreeName();
        m.ua = here;
        m.pos = pos;
        m.dir = dir;
        g_marks.push_back(m);
        Save();
        g.newName[0] = 0;
        g.dirty = true;
        g.browser.SetFocus(m.key);
        ui::Toast(L::T("tp.saved"), m.name, icon::Pin);
    }
    y += S(46);

    // any address: glyphs or galactic coordinates, any galaxy
    if (!f) {
        // how glyphs are shown: icons, hex digits, or both
        static std::vector<std::string> modes;
        if (modes.empty()) modes = { L::T("tp.glyphs.icons"), L::T("tp.glyphs.hex"), L::T("tp.glyphs.both") };
        int& mode = Settings::Get().Data().glyphMode;
        float mw = S(150);
        dl->AddText(ui::theme::Body(), S(15), ImVec2(a.x + pad, y + S(8)), P().textDim, L::T("tp.addr.title"));
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad + bw - mw, y));
        if (ui::Dropdown("##glyphmode", modes, &mode, mw)) Settings::Get().MarkDirty();
        y += S(42);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        ui::TextField("##addr", g.addr, sizeof g.addr, bw, L::T("tp.addr.hint"));
        y += S(42);
        // the typed address as the game's glyphs, 12 slots
        {
            std::string hex;
            for (const char* c = g.addr; *c; ++c) if (isxdigit((unsigned char)*c)) hex += (char)toupper((unsigned char)*c);
            if (!strchr(g.addr, ':') && !hex.empty()) {
                float gs = std::min(S(36), (bw - S(2) * 11) / 12);
                for (int k = 0; k < 12; ++k) {
                    ImVec2 ga(a.x + pad + k * (gs + S(2)), y);
                    dl->AddRectFilled(ga, ImVec2(ga.x + gs, ga.y + gs), IM_COL32(0, 0, 0, 110));
                    if (k < (int)hex.size()) DrawGlyph(dl, ga, gs, gs, hex[k] <= '9' ? hex[k] - '0' : hex[k] - 'A' + 10);
                }
                y += gs + S(8);
            }
        }
        // glyph pad: the 16 glyphs, plus delete
        float kw = (bw - S(4) * 8) / 9, kh = std::min(kw, S(44));
        for (int k = 0; k < 17; ++k) {
            ImVec2 ka(a.x + pad + (k % 9) * (kw + S(4)), y + (k / 9) * (kh + S(4)));
            ImGui::SetCursorScreenPos(ka);
            ImGui::PushID(k);
            bool click = ImGui::InvisibleButton("##glyph", ImVec2(kw, kh));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(ka, ImVec2(ka.x + kw, ka.y + kh), hov ? P().rowHover : IM_COL32(0, 0, 0, 110));
            dl->AddRect(ka, ImVec2(ka.x + kw, ka.y + kh), hov ? P().text : P().line, 0.f, S(1));
            DrawGlyph(dl, ka, kw, kh, k);
            if (click) {
                size_t n = strlen(g.addr);
                if (k == 16) { if (n) g.addr[n - 1] = 0; }
                else if (n + 1 < sizeof g.addr) { g.addr[n] = "0123456789ABCDEF"[k]; g.addr[n + 1] = 0; }
            }
        }
        y += 2 * (kh + S(4)) + S(6);
        static std::vector<std::string> galaxies;
        if (galaxies.empty()) {
            galaxies.push_back(L::T("tp.galaxy.here"));
            for (int i = 0; i < 256; ++i)
                galaxies.push_back(std::to_string(i + 1) + (i < 10 ? std::string("  ") + kGalaxyNames[i] : std::string()));
        }
        int gi = g.galaxy + 1;
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Dropdown("##galaxy", galaxies, &gi, bw)) g.galaxy = gi - 1;
        y += S(42);
        int galaxy = g.galaxy >= 0 ? g.galaxy : (int)((here >> 32) & 0xFF);
        uint64_t ua = 0;
        bool ok = ParseAddress(g.addr, galaxy, ua);
        if (g.addr[0]) {
            std::string info = ok ? Where(ua) + "   " + Glyphs(ua) : std::string(L::T("tp.addr.bad"));
            dl->AddText(ui::theme::Body(), S(14), ImVec2(a.x + pad, y), ok ? P().textBody : P().textFaint, info.c_str());
            y += S(22);
        }
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (live && ok && ui::HoldButton("##warpaddr", L::T("tp.warp"), bw)) {
            // the warp always lands at the system; a planet index it doesn't have crashes the arrival
            Warp(ua & ~(0xFull << 52), g.addr, nullptr);
            ui::Toast(L::T("tp.warping"), Where(ua), icon::Pin);
        }
        if (live && ok) y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##mapfix", L::T("tp.mapfix"), bw, ui::Tone::Neutral, live && g_syncMap)) FixMap();
        y += S(46);
    }

    if (f && !sel.key.empty()) {
        auto edit = [&](auto fn) {
            for (auto& m : g_marks) if (m.key == sel.key) fn(m);
            Save();
            g.dirty = true;
        };
        dl->AddLine(ImVec2(a.x + pad, y + S(4)), ImVec2(a.x + pad + bw, y + S(4)), P().line, S(1));
        y += S(16);
        bool same = live && SystemOf(sel.ua) == SystemOf(here);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (same && sel.noPos) ui::Button("##here", L::T("scout.youarehere"), bw, ui::Tone::Neutral, false);
        else if (same) {
            if (ui::Button("##go", L::T("tp.go"), bw, ui::Tone::Teal, g_setPos != nullptr)) {
                Mark m = sel;
                game::Post([m] {
                    uintptr_t d = game::Data();
                    if (!d || !g_setPos) return;
                    Vec4 vel;
                    g_setPos(d + g_player, &m.pos, &m.dir, &vel);
                });
            }
        }
        else if (!live) ui::Button("##warpno", L::T("tp.notready"), bw, ui::Tone::Neutral, false);
        else if (ui::HoldButton("##warpgo", L::T("tp.warpgo"), bw)) {
            Warp(sel.ua, sel.name, sel.noPos ? nullptr : &sel);
            ui::Toast(L::T("tp.warping"), sel.name, icon::Pin);
        }
        y += S(46);

        if (g.renameKey != sel.key) {
            g.renameKey = sel.key;
            strncpy_s(g.rename, sel.name.c_str(), _TRUNCATE);
        }
        float rw = bw - S(118);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        bool ren = ui::TextField("##rename", g.rename, sizeof g.rename, rw, L::T("tp.rename.hint"));
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad + rw + S(8), y));
        ren |= ui::Button("##renbtn", L::T("tp.rename"), S(110), ui::Tone::Neutral, g.rename[0] && sel.name != g.rename);
        if (ren && g.rename[0]) edit([](Mark& m) { m.name = g.rename; });
        y += S(46);

        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##copy", L::T("tp.copy"), bw, ui::Tone::Neutral, true)) {
            std::string s = L::Tf("tp.addr", std::to_string(((sel.ua >> 32) & 0xFF) + 1), Glyphs(sel.ua));
            ImGui::SetClipboardText(s.c_str());
            ui::Toast(L::T("tp.copied"), s, icon::Pin);
        }
        y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (live && ui::HoldButton("##over", L::T("tp.overwrite"), bw)) {
            edit([&](Mark& m) { m.ua = here; m.pos = pos; m.dir = dir; m.noPos = false; });
            ui::Toast(L::T("tp.overwritten"), sel.name, icon::Pin);
        }
        if (live) y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::HoldButton("##del", L::T("tp.delete"), bw)) {
            std::erase_if(g_marks, [&](const Mark& m) { return m.key == sel.key; });
            Save();
            g.dirty = true;
            g.browser.ClearSelection();
        }
        y += S(46);
    }
    const char* note = L::T("tp.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(a.x + pad, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
    return y;
}

}

// The player's packed universe address, built from cGcPlayerState.mLocation (cGcUniverseAddressData):
// X | Z << 12 | Y << 24 | galaxy << 32 | system << 40 | planet << 52, the same packing as mCurrentUA.
bool CurrentAddress(uint64_t& ua)
{
    uintptr_t d = game::Data();
    int32_t planet = 0, sys = 0, x = 0, y = 0, z = 0, galaxy = 0;
    uintptr_t l = d + g_loc;
    if (!d || !Rd(l + A.planet, planet) || !Rd(l + A.system, sys) || !Rd(l + A.x, x) || !Rd(l + A.y, y) || !Rd(l + A.z, z)
        || !Rd(l + A.galaxy, galaxy))
        return false;
    ua = (uint64_t)(x & 0xFFF) | (uint64_t)(z & 0xFFF) << 12 | (uint64_t)(y & 0xFF) << 24 | (uint64_t)(galaxy & 0xFF) << 32
        | (uint64_t)(sys & 0xFFF) << 40 | (uint64_t)(planet & 0xF) << 52;
    // once per session, check it against the current system's own address
    static std::atomic<bool> checked{ false };
    uintptr_t sp = 0;
    uint64_t sa = 0;
    if (!checked && Rd(d + g_sysPtr, sp) && sp && Rd(sp + kSysAddress, sa) && sa && !checked.exchange(true)) {
        constexpr uint64_t mask = 0x000FFFFFFFFFFFFFull;
        if ((ua & mask) == (sa & mask)) log::Info("Teleport", "Current address {:X} matches the system", ua);
        else log::Warn("Teleport", "Current address {:X}, system says {:X}", ua, sa);
    }
    return true;
}

void WarpToAddress(uint64_t ua, const std::string& name) { Warp(ua & ~(0xFull << 52), name, nullptr); }

bool GlyphsToAddress(const std::string& glyphs, int galaxy, uint64_t& ua) { return ParseAddress(glyphs, galaxy, ua); }

std::string AddressGlyphs(uint64_t ua) { return Glyphs(ua); }

std::string AddressWhere(uint64_t ua) { return Where(ua); }

void SaveSystemSpot(uint64_t ua, const std::string& name)
{
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    Mark m;
    m.key = std::to_string(g_nextKey++);
    m.name = name;
    m.ua = ua & ~(0xFull << 52);
    m.noPos = true;
    g_marks.push_back(m);
    Save();
    g.dirty = true;
}

void InstallTeleport()
{
    const Target& t = Loc().Resolve("Player.SetToPosition");
    if (t.ok) g_setPos = (SetToPositionFn)Abs(t.func);
    else log::Warn("Teleport", "SetToPosition missing: {}", t.reason);
    // the request offsets; the endpoint and jump target are listed from the simulation
    const Target& sim = Loc().Resolve("Data.Simulation");
    if (const Target& m = Loc().Resolve("Warp.RequestMode"); m.ok && m.offset) g_reqMode = *m.offset;
    if (const Target& w = Loc().Resolve("Data.SpaceshipWarp"); w.ok && w.offset) g_warp = *w.offset;
    if (const Target& w = Loc().Resolve("Warp.ArrivalFlag"); w.ok && w.offset) kWarpArrival = *w.offset;
    if (const Target& m = Loc().Resolve("Player.Mode"); m.ok && m.offset) g_playerMode = *m.offset;
    if (const Target& p = Loc().Resolve("Player.InstanceOffset"); p.ok && p.offset) g_player = *p.offset;
    if (const Target& s = Loc().Resolve("Data.SolarSystem"); s.ok && s.offset) g_sysPtr = *s.offset;
    if (const Target& l = Loc().Resolve("PlayerState.Location"); l.ok && l.offset) g_loc = *l.offset;
    {
        namespace meta = hooks::meta;
        auto get = [](const char* cls, const char* f, uint32_t& out) { if (auto m = meta::Path(cls, f)) out = m->offset; };
        uint32_t ga = 0;
        get("cGcUniverseAddressData", "GalacticAddress", ga);
        get("cGcUniverseAddressData", "RealityIndex", A.galaxy);
        get("cGcGalacticAddressData", "PlanetIndex", A.planet); get("cGcGalacticAddressData", "SolarSystemIndex", A.system);
        get("cGcGalacticAddressData", "VoxelX", A.x); get("cGcGalacticAddressData", "VoxelY", A.y); get("cGcGalacticAddressData", "VoxelZ", A.z);
        A.planet += ga; A.system += ga; A.x += ga; A.y += ga; A.z += ga;
    }
    if (const Target& e = Loc().Resolve("Warp.NexusExitEffect"); e.ok && e.offset) g_nexusExit = (int32_t)*e.offset;
    if (const Target& w = Loc().Resolve("SpaceshipWarp.WarpToDistantSystem"); w.ok) g_warpDistant = (WarpDistantFn)Abs(w.func);
    if (const Target& gt = Loc().Resolve("PlayerState.GoToRealityIndex"); gt.ok) g_goToGalaxy = (GoToGalaxyFn)Abs(gt.func);
    if (const Target& c = Loc().Resolve("Portal.CheckUAIsValid"); c.ok) g_checkUA = (CheckUAFn)Abs(c.func);
    else log::Warn("Teleport", "Address check missing, typed addresses go as typed: {}", c.reason);
    if (const Target& m = Loc().Resolve("GalaxyMap.SyncToGameState"); m.ok) g_syncMap = (SyncMapFn)Abs(m.func);
    else log::Warn("Teleport", "Galaxy map sync missing: {}", m.reason);
    if (sim.ok && sim.offset) {
        if (const Target& j = Loc().Resolve("Warp.RequestJumpTarget"); j.ok && j.offset) g_reqJump = *sim.offset + *j.offset;
        if (const Target& e = Loc().Resolve("Warp.RequestEndpoint"); e.ok && e.offset) g_reqEndpoint = *sim.offset + *e.offset;
    }
    log::Info("Teleport", "Warp request: jump +0x{:X}, endpoint +0x{:X}, mode +0x{:X}, warp +0x{:X}, location +0x{:X}", g_reqJump, g_reqEndpoint, g_reqMode, g_warp, g_loc);
    game::EveryFrame(PendingTick);
    game::EveryFrame(MapSyncTick);

    Registry::Get().Post([] {
        g.browser.chips = { L::T("tp.chip.all"), L::T("tp.chip.here"), L::T("tp.chip.away") };
        g.browser.captions = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { std::lock_guard lk(g_mx); return std::to_string(g_marks.size()); };
        ui::RegisterPage("teleport", std::move(page));
    });
}

}
