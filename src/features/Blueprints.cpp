#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "input/Input.h"
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

// Blueprints tab: copy a base, or the parts around you, into a file and paste it into
// one of your bases. Only the game's own part ids go in, so pasted parts save and upload like built ones.
namespace os::features {

namespace {

using json = nlohmann::json;
using ui::theme::P;
using ui::theme::S;
namespace meta = hooks::meta;

struct V3 { float x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a)
{
    float l = Len(a);
    return l > 1e-6f ? a * (1 / l) : V3{};
}

struct alignas(16) Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct alignas(16) Mat { Vec4 right, up, at, local, offset; }; // cTkPhysRelMat34
V3 Of(const Vec4& v) { return { v.x, v.y, v.z }; }
Vec4 To(V3 v, float w = 0) { return { v.x, v.y, v.z, w }; }

// Base.PlaceObject(mgr, id, baseIndex*, PhysRelMat34*, {userData, flags}*, false, 0, true); scale comes from the row lengths.
// Base.DeleteObject(mgr, {u32 handle; u16 baseIndex}*, true)
using PlaceFn = void* (*)(void* mgr, const void* id16, const uint16_t* baseIdx, const void* xf, void* extra, bool a5, float a6, bool a7);
using DeleteFn = void (*)(void* mgr, const void* handle, bool sync);
PlaceFn g_place = nullptr;
DeleteFn g_delete = nullptr;
uintptr_t g_mgr = 0;
int64_t g_playerOff = 0x72CDE0, g_baseList = 0x31AE60; // Player.InstanceOffset, Base.List (count 4 bytes before)
uint32_t g_handleOff = 0x118;                           // object's handle, base index 4 bytes after

// cGcPlayerBasePersistentBuffer (code in Base.BufferAddObject): objects {cap, count, data} at +0x30, a 0xD0 slot each
// with the entry at +0x10, the address it is loaded in now +0x40, base matrix at +0x50, the base's own address +0xA0,
// owner (cGcDiscoveryOwner: timestamp, local id +0x4, platform +0x84, username +0xC4) at +0xF0,
// name +0x1F4, base type +0x23C. The list holds your bases and the other players' bases the game has loaded.
struct { uint32_t count = 0x34, data = 0x38, slot = 0xD0, entry = 0x10, loadedIn = 0x40, matrix = 0x50, ua = 0xA0, owner = 0xF0, name = 0x1F4, type = 0x23C; } B;
// cGcPersistentBaseEntry, from the game's metadata when it has them. A zero timestamp is a free slot.
struct { uint32_t at = 0, pos = 0x10, up = 0x20, id = 0x30, time = 0x40, user = 0x48; } R;
constexpr int32_t kShipBase = 12;   // corvette base, it follows the ship
constexpr float kReach = 600.f;     // how far from a base computer counts as at that base
constexpr int kPerFrame = 12;       // parts placed or removed per game frame
constexpr uint32_t kUploadLimit = 3000;

struct Part { std::string id; uint64_t user = 0; V3 p, up, at; };
struct Blueprint
{
    std::string file, name, from;
    int64_t made = 0;
    std::vector<Part> parts;
    V3 lo, hi;
};

std::mutex g_mx; // g_list, g_near, g_status, g_focusKey
std::vector<Blueprint> g_list;
bool g_loaded = false;
uint64_t g_version = 1;
struct Near { bool ok = false, mine = false; std::string name; uint32_t parts = 0; float dist = 0; } g_near;
std::string g_status, g_focusKey;

// Where a paste goes: anchor and axes relative to `off`, and the base it goes into.
struct Place
{
    V3 a, r, u, f, flatF; // flatF: the facing before the turn
    float sign = 1, w = 1;
    Vec4 off;
    uint16_t base = 0xFFFF;
    std::string baseName;
    uint32_t used = 0;
    bool ok = false; // one of your bases takes it
};


// game thread
struct Handle { uint32_t h = 0; uint16_t base = 0; uint16_t pad = 0; };
struct Job
{
    bool active = false, undo = false;
    std::vector<Part> parts;
    size_t next = 0;
    uint16_t base = 0;
    Place pl;
    int placed = 0, failed = 0;
    std::vector<Handle> made; // this paste, kept for undo
} g_job;
std::vector<Handle> g_undo;
std::atomic<bool> g_stop{ false };
std::atomic<int> g_done{ 0 }, g_total{ 0 }, g_undoCount{ 0 };
std::atomic<bool> g_busy{ false }, g_busyUndo{ false };

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> entries;
    uint64_t seen = 0;
    bool around = false;
    float radius = 30, ahead = 10, raise = 0, turn = 0;
    bool withComputer = false; // paste the base computer too
    char name[64] = {};
} g;

bool Live() { return g_place && g_mgr && game::FrameHookReady() && game::PlayerState(); }

std::atomic<bool> g_anywhere{ false }; // paste outside your bases: the parts go into the game's loose buffer, not a base

void SetStatus(std::string s)
{
    std::lock_guard lk(g_mx);
    g_status = std::move(s);
}

// ---- reading the world (game thread) ----

bool Rd4(uintptr_t a, Vec4& v) { return Rd(a, v.x) && Rd(a + 4, v.y) && Rd(a + 8, v.z) && Rd(a + 12, v.w); }
bool Rd3(uintptr_t a, V3& v) { return Rd(a, v.x) && Rd(a + 4, v.y) && Rd(a + 8, v.z); }

struct Me { V3 pos, fwd; Vec4 off; float w = 1; };
bool Player(Me& m)
{
    uintptr_t d = game::Data();
    if (!d) return false;
    uintptr_t p = d + (uintptr_t)g_playerOff;
    Vec4 at, local;
    if (!Rd4(p + 0x330, at) || !Rd4(p + 0x360, local) || !Rd4(p + 0x370, m.off)) return false;
    m.pos = Of(local);
    m.fwd = Of(at) * -1; // the body's at row points backwards
    m.w = local.w;
    return true;
}

struct BaseInfo
{
    uint16_t idx = 0;
    uintptr_t p = 0;
    V3 o, r, u, a; // origin relative to the player's offset, then the base's axes
    double pos[3] = {}; // origin in the system
    std::string name, owner, platform;
    int32_t type = 0;
    uint64_t ua = 0;
    bool others = false, generated = false; // someone else's base, or a game-made one (settlements)
};

// GcPersistentBaseTypes names from the game's metadata, by value.
const std::vector<std::pair<int, std::string>>& TypeNames()
{
    static const auto names = meta::EnumNames("cGcPersistentBase", "BaseType");
    return names;
}

// Yours: home, freighter, corvette and player station bases. Others: External and Friends kinds.
// Everything else (Civilian stations, Generated, UI scratch bases) belongs to the game.
void Classify(BaseInfo& b)
{
    for (auto& [v, n] : TypeNames())
        if (v == b.type) {
            b.others = n.find("External") != std::string::npos || n.find("Friends") != std::string::npos;
            bool mine = n.starts_with("Home") || n == "FreighterBase" || n == "SpaceBase" || n.starts_with("Player");
            b.generated = !b.others && !mine;
            return;
        }
    // no metadata: the 7.x order (0 Home, 1 Freighter, 12 PlayerShip, 16 PlayerSpaceStation)
    b.others = b.type == 2 || b.type == 4 || b.type == 5 || b.type == 7 || b.type == 13 || b.type == 17 || b.type == 18;
    b.generated = !b.others && b.type != 0 && b.type != 1 && b.type != 6 && b.type != 12 && b.type != 16;
}

std::string Text(uintptr_t a, size_t n)
{
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        char c = 0;
        if (!Rd(a + i, c) || !c) break;
        if ((unsigned char)c < 0x20) return {};
        s += c;
    }
    return s;
}

uint32_t UsedParts(uintptr_t b)
{
    uint32_t n = 0, used = 0;
    uintptr_t data = 0;
    if (!Rd(b + B.count, n) || !Rd(b + B.data, data) || !data || n > 50000) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t t = 0;
        if (Rd(data + (uintptr_t)i * B.slot + B.entry + R.time, t) && t) ++used;
    }
    return used;
}

std::vector<BaseInfo> Bases(const Vec4& off)
{
    std::vector<BaseInfo> out;
    uintptr_t d = game::Data(), arr = 0;
    uint32_t cnt = 0;
    if (!d || !Rd(d + g_baseList - 4, cnt) || !Rd(d + g_baseList, arr) || !arr || cnt > 512) return out;
    for (uint32_t i = 0; i < cnt; ++i) {
        BaseInfo bi;
        Vec4 r, u, a, local, boff;
        if (!Rd(arr + i * 8, bi.p) || !bi.p) continue;
        uintptr_t m = bi.p + B.matrix;
        if (!Rd4(m, r) || !Rd4(m + 0x10, u) || !Rd4(m + 0x20, a) || !Rd4(m + 0x30, local) || !Rd4(m + 0x40, boff)) continue;
        bi.idx = (uint16_t)i;
        bi.r = Of(r);
        bi.u = Of(u);
        bi.a = Of(a);
        bi.o = Of(local) + (Of(boff) - Of(off));
        bi.pos[0] = (double)local.x + boff.x;
        bi.pos[1] = (double)local.y + boff.y;
        bi.pos[2] = (double)local.z + boff.z;
        if (Len(bi.u) < 0.5f) continue; // not set up
        Rd(bi.p + B.type, bi.type);
        if (!Rd(bi.p + B.ua, bi.ua) || !bi.ua) Rd(bi.p + B.loadedIn, bi.ua);
        bi.name = Text(bi.p + B.name, 64);
        bi.platform = Text(bi.p + B.owner + 0x84, 64);
        bi.owner = Text(bi.p + B.owner + 0xC4, 64);
        Classify(bi);
        out.push_back(std::move(bi));
    }
    return out;
}

// Copy works from any base; paste only goes into your own.
const BaseInfo* Nearest(const std::vector<BaseInfo>& bases, V3 at, float* dist = nullptr, bool mine = false)
{
    const BaseInfo* best = nullptr;
    float bd = kReach;
    for (auto& b : bases) {
        float dd = Len(b.o - at);
        if (b.type == kShipBase || (mine && (b.others || b.generated))) continue;
        if (dd < bd) { bd = dd; best = &b; }
    }
    if (dist) *dist = bd;
    return best;
}

// The frame a blueprint is stored in: base up, the player's facing flattened onto it.
void Frame(V3 up, V3 fwd, V3& r, V3& u, V3& f)
{
    u = Norm(up);
    f = Norm(fwd - u * Dot(fwd, u));
    if (Len(f) < 0.5f) f = Norm(Cross(u, V3{ 1, 0, 0 }));
    r = Cross(u, f);
}

// ---- files ----

std::filesystem::path Folder() { return std::filesystem::path(paths::Root()) / L"OpenSkies.blueprints"; }

json J3(V3 v) { return json::array({ v.x, v.y, v.z }); }
V3 V(const json& a)
{
    V3 v;
    if (a.is_array() && a.size() >= 3) { v.x = a[0]; v.y = a[1]; v.z = a[2]; }
    return v;
}

void Bounds(Blueprint& b)
{
    b.lo = b.hi = b.parts.empty() ? V3{} : b.parts[0].p;
    for (auto& p : b.parts) {
        b.lo = { std::min(b.lo.x, p.p.x), std::min(b.lo.y, p.p.y), std::min(b.lo.z, p.p.z) };
        b.hi = { std::max(b.hi.x, p.p.x), std::max(b.hi.y, p.p.y), std::max(b.hi.z, p.p.z) };
    }
}

bool Write(const Blueprint& b)
{
    json parts = json::array();
    for (auto& p : b.parts)
        parts.push_back({ { "id", p.id }, { "user", p.user }, { "p", J3(p.p) }, { "up", J3(p.up) }, { "at", J3(p.at) } });
    json j = { { "name", b.name }, { "from", b.from }, { "made", b.made }, { "parts", std::move(parts) } };
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    std::ofstream out(Folder() / paths::Wide(b.file));
    if (!out) return false;
    out << j.dump(1);
    return (bool)out;
}

void Load() // under g_mx
{
    g_loaded = true;
    g_list.clear();
    std::error_code ec;
    for (auto& f : std::filesystem::directory_iterator(Folder(), ec)) {
        if (f.path().extension() != L".json") continue;
        try {
            std::ifstream in(f.path());
            json j = json::parse(in);
            Blueprint b;
            b.file = paths::Utf8(f.path().filename().wstring());
            b.name = j.value("name", paths::Utf8(f.path().stem().wstring()));
            b.from = j.value("from", "");
            b.made = j.value("made", (int64_t)0);
            for (auto& e : j.value("parts", json::array())) {
                Part p;
                p.id = e.value("id", "");
                p.user = e.value("user", (uint64_t)0);
                p.p = V(e["p"]);
                p.up = V(e["up"]);
                p.at = V(e["at"]);
                if (!p.id.empty() && p.id.size() < 16) b.parts.push_back(std::move(p));
            }
            Bounds(b);
            g_list.push_back(std::move(b));
        }
        catch (...) { log::Warn("Blueprints", "Could not read {}", paths::Utf8(f.path().filename().wstring())); }
    }
    std::sort(g_list.begin(), g_list.end(), [](auto& a, auto& b) { return a.made > b.made; });
    ++g_version;
}

std::string FileFor(const std::string& name)
{
    std::string s;
    for (char c : name) s += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : c == ' ' ? '_' : '\0';
    std::erase(s, '\0');
    if (s.empty()) s = "blueprint";
    s = s.substr(0, 40);
    std::string f = s + ".json";
    for (int n = 2; std::filesystem::exists(Folder() / paths::Wide(f)); ++n) f = s + "_" + std::to_string(n) + ".json";
    return f;
}

// ---- copy (game thread) ----

void Copy(bool around, float radius, std::string name)
{
    Me me;
    if (!Player(me)) { SetStatus(L::T("bp.ingame")); return; }
    auto bases = Bases(me.off);
    const BaseInfo* b = Nearest(bases, me.pos);
    if (!b) { SetStatus(L::T("bp.nobase")); return; }
    V3 r, u, f;
    Frame(b->u, me.fwd, r, u, f);
    Blueprint bp;
    bp.from = b->others && !b->owner.empty() ? (b->name.empty() ? b->owner : b->name + " (" + b->owner + ")") : b->name;
    bp.name = name.empty() ? (b->name.empty() ? std::string("Blueprint") : b->name) : name;
    bp.made = (int64_t)std::time(nullptr);
    uint32_t n = 0;
    uintptr_t data = 0;
    if (Rd(b->p + B.count, n) && Rd(b->p + B.data, data) && data && n <= 50000)
        for (uint32_t i = 0; i < n; ++i) {
            uintptr_t e = data + (uintptr_t)i * B.slot + B.entry;
            uint64_t t = 0;
            if (!Rd(e + R.time, t) || !t) continue;
            Part p;
            V3 pos, up, at;
            p.id = Text(e + R.id, 16);
            if (p.id.empty() || !Rd3(e + R.pos, pos) || !Rd3(e + R.up, up) || !Rd3(e + R.at, at)) continue;
            Rd(e + R.user, p.user);
            V3 w = b->o + b->r * pos.x + b->u * pos.y + b->a * pos.z;
            if (around && Len(w - me.pos) > radius) continue;
            V3 rel = w - me.pos;
            V3 wu = b->r * up.x + b->u * up.y + b->a * up.z, wa = b->r * at.x + b->u * at.y + b->a * at.z;
            p.p = { Dot(rel, r), Dot(rel, u), Dot(rel, f) };
            p.up = { Dot(wu, r), Dot(wu, u), Dot(wu, f) };
            p.at = { Dot(wa, r), Dot(wa, u), Dot(wa, f) };
            bp.parts.push_back(std::move(p));
        }
    if (bp.parts.empty()) { SetStatus(L::T("bp.nothing")); return; }
    Bounds(bp);
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    bp.file = FileFor(bp.name);
    if (!Write(bp)) { g_status = L::T("bp.writefail"); return; }
    log::Info("Blueprints", "Copied {} parts from base {} ({}) into {}", bp.parts.size(), b->idx, b->name, bp.file);
    g_status = L::Tf("bp.copied", std::to_string(bp.parts.size()), bp.name);
    g_focusKey = bp.file;
    g_list.insert(g_list.begin(), std::move(bp));
    ++g_version;
}

// ---- paste and undo (game thread) ----

void* CallPlace(const char* id16, const uint16_t* idx, const Mat* xf, void* extra)
{
    __try { return g_place((void*)g_mgr, id16, idx, xf, extra, false, 0.f, true); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool CallDelete(const Handle* h)
{
    __try { g_delete((void*)g_mgr, h, true); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Ahead of you along your facing, flattened onto the local up. `fixed` keeps a locked anchor and facing.
bool MakePlace(float ahead, float raise, float turnDeg, V3 nudge, Place& pl, const Place* fixed = nullptr)
{
    Me me;
    if (!Player(me)) return false;
    auto bases = Bases(me.off);
    V3 r, u, f, a;
    if (fixed) {
        u = fixed->u;
        f = fixed->flatF;
        a = fixed->a + (Of(fixed->off) - Of(me.off)); // the locked anchor on the current origin
    }
    else {
        const BaseInfo* here = Nearest(bases, me.pos);
        Frame(here ? here->u : V3{ 0, 1, 0 }, me.fwd, r, u, f);
        a = me.pos + f * ahead;
    }
    pl.flatF = f;
    float t = turnDeg * 3.14159265f / 180.f;
    f = Norm(f * std::cos(t) + Cross(u, f) * std::sin(t));
    r = Cross(u, f);
    pl.a = a + u * raise + r * nudge.x + f * nudge.z;
    pl.r = r;
    pl.u = u;
    pl.f = f;
    pl.off = me.off;
    pl.w = me.w;
    const BaseInfo* b = Nearest(bases, pl.a, nullptr, true);
    pl.ok = b != nullptr || g_anywhere;
    pl.base = b ? b->idx : 0xFFFF;
    pl.baseName = b ? (b->name.empty() ? std::to_string(b->idx) : b->name) : std::string(L::T("bp.nobaseshort"));
    pl.used = b ? UsedParts(b->p) : 0;
    const BaseInfo* any = b ? b : bases.empty() ? nullptr : &bases[0]; // every base shares the handedness
    pl.sign = any ? (Dot(Cross(any->u, any->a), any->r) < 0 ? -1.f : 1.f) : 1.f;
    return true;
}

Mat Xf(const Place& pl, const Part& p)
{
    V3 w = pl.a + pl.r * p.p.x + pl.u * p.p.y + pl.f * p.p.z;
    V3 up = pl.r * p.up.x + pl.u * p.up.y + pl.f * p.up.z, at = pl.r * p.at.x + pl.u * p.at.y + pl.f * p.at.z;
    V3 right = Norm(Cross(up, at)) * (pl.sign * Len(up));
    Mat xf;
    xf.right = To(right);
    xf.up = To(up);
    xf.at = To(at);
    xf.local = To(w, pl.w);
    xf.offset = pl.off;
    return xf;
}

void StartPasteAt(std::vector<Part> parts, const Place& pl)
{
    if (g_job.active) return;
    if (!pl.ok) { SetStatus(L::T("bp.nobase")); return; }
    game::BackupSaveOnce();
    g_job = {};
    g_job.active = true;
    g_job.parts = std::move(parts);
    g_job.base = pl.base;
    g_job.pl = pl;
    g_stop = false;
    g_done = 0;
    g_total = (int)g_job.parts.size();
    g_busy = true;
    g_busyUndo = false;
    std::string msg = L::Tf("bp.pasting", std::to_string(g_job.parts.size()), pl.baseName);
    if (pl.used + g_job.parts.size() > kUploadLimit) msg += "  " + L::Tf("bp.overlimit", std::to_string(pl.used + g_job.parts.size()));
    SetStatus(msg);
    log::Info("Blueprints", "Paste {} parts into base {} ({} parts there), sign {}", g_job.parts.size(), pl.base, pl.used, pl.sign);
}

void StartPaste(std::vector<Part> parts, float ahead, float raise, float turnDeg)
{
    Place pl;
    if (!MakePlace(ahead, raise, turnDeg, {}, pl)) { SetStatus(L::T("bp.ingame")); return; }
    StartPasteAt(std::move(parts), pl);
}

void PlaceOne(const Part& p)
{
    Job& j = g_job;
    Mat xf = Xf(j.pl, p);
    char id16[16] = {};
    memcpy(id16, p.id.c_str(), std::min<size_t>(p.id.size(), 15));
    struct { uint64_t user; int32_t flags; int32_t pad; } extra{ p.user, p.id == "RACE_START" || p.id == "CHECKPOINT" ? 1 : 0, 0 };
    uint16_t idx = j.base;
    void* obj = CallPlace(id16, &idx, &xf, &extra);
    Handle h;
    if (obj && Rd((uintptr_t)obj + g_handleOff, h.h) && Rd((uintptr_t)obj + g_handleOff + 4, h.base)) {
        j.made.push_back(h);
        ++j.placed;
    }
    else {
        ++j.failed;
        if (j.failed <= 5) log::Warn("Blueprints", "Could not place {}", p.id);
    }
}

void StartUndo()
{
    if (g_job.active || g_undo.empty() || !g_delete) return;
    g_job = {};
    g_job.active = g_job.undo = true;
    g_job.made = std::move(g_undo);
    g_undo.clear();
    g_undoCount = 0;
    g_stop = false;
    g_done = 0;
    g_total = (int)g_job.made.size();
    g_busy = g_busyUndo = true;
    SetStatus(L::Tf("bp.undoing", std::to_string(g_job.made.size())));
}

// ---- paste preview: the game's own placement ghosts, moved with you until you place (game thread) ----

using AddGhostFn = uint32_t* (*)(void* mgr, uint32_t* out, const char* id16, const Mat* m, const uint16_t* base, const uint8_t* material, bool);
using NodeFn = void (*)(void* mgr, uint32_t node);
using FindObjFn = void* (*)(void* mgr, uint32_t node);
using SetMatrixFn = void (*)(void* obj, const Mat* m);
using UpdatePartsFn = void (*)(void* obj, bool, bool, bool);
using ScanFxFn = void (*)(void* fxMgr, uint32_t node, const void* effect);
AddGhostFn g_addGhost = nullptr;
NodeFn g_removeGhost = nullptr;
FindObjFn g_findObj = nullptr;
SetMatrixFn g_setMatrix = nullptr;
UpdatePartsFn g_updateParts = nullptr;
ScanFxFn g_scanFx = nullptr;
uintptr_t g_fxMgr = 0, g_ghostLook = 0; // effects manager, cGcGameplayGlobals.BaseBuildingGhostScanEffect
constexpr uint32_t kNoNode = 0x7FFFF;
constexpr size_t kMaxGhosts = 400; // bigger blueprints show an even sample
constexpr int kGhostsPerFrame = 20, kMovesPerFrame = 200, kRemovesPerFrame = 40;

struct Preview
{
    bool active = false, locked = false;
    std::vector<Part> parts;
    std::vector<size_t> shown;   // parts that get a ghost
    std::vector<uint32_t> nodes; // one per shown part, as they are made
    float ahead = 10, raise = 0, turn = 0;
    V3 nudge;                    // right, unused, forward
    Place pl, lockAt;
    size_t moveFrom = 0;         // ghosts still to move to the latest place
    uintptr_t data = 0;          // the game data it was made in; a reload drops it
    bool prevL = false, prevR = false, prevP = false;
} g_pv;
std::vector<uint32_t> g_dropGhosts;
std::atomic<bool> g_previewOn{ false }, g_previewLocked{ false }, g_previewOk{ false };
std::atomic<int> g_previewShown{ 0 }, g_previewTotal{ 0 };

bool PreviewKeys(uint16_t vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == input::kWheel || vk == 'P' || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_UP ||
           vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

uint32_t CallAddGhost(const char* id16, const Mat* m, uint16_t base)
{
    uint32_t node = kNoNode;
    uint8_t material[2] = {};
    __try { g_addGhost((void*)g_mgr, &node, id16, m, &base, material, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { node = kNoNode; }
    if (node != kNoNode && g_scanFx && g_fxMgr && g_ghostLook) {
        __try { g_scanFx((void*)g_fxMgr, node, (const void*)g_ghostLook); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return node;
}

void CallMoveGhost(uint32_t node, const Mat* m)
{
    __try {
        if (void* obj = g_findObj((void*)g_mgr, node)) {
            g_setMatrix(obj, m);
            g_updateParts(obj, false, false, false);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void CallRemoveGhost(uint32_t node)
{
    __try { g_removeGhost((void*)g_mgr, node); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool GhostsReady() { return g_addGhost && g_removeGhost && g_findObj && g_setMatrix && g_updateParts; }

void EndPreview(bool place)
{
    Preview& v = g_pv;
    if (!v.active) return;
    if (place) StartPasteAt(std::move(v.parts), v.pl);
    if (v.data == game::Data())
        for (uint32_t n : v.nodes)
            if (n != kNoNode) g_dropGhosts.push_back(n);
    log::Info("Blueprints", "Preview {}: {} ghosts", place ? "placed" : "ended", v.nodes.size());
    bool ok = v.pl.ok;
    v = {};
    input::SetSwallow(nullptr);
    g_previewOn = g_previewLocked = false;
    const char* key = !place ? "bp.preview.cancelled" : ok ? "bp.preview.placed" : "bp.nobase";
    Registry::Get().Post([key, place, ok] { ui::Toast(L::T(key), "", place && ok ? icon::Check : icon::Close); });
}

void StartPreview(std::vector<Part> parts, float ahead, float raise, float turn)
{
    if (g_pv.active) EndPreview(false);
    if (g_job.active || parts.empty()) return;
    Preview& v = g_pv;
    v = {};
    v.active = true;
    v.ahead = ahead;
    v.raise = raise;
    v.turn = turn;
    v.data = game::Data();
    size_t n = parts.size(), show = std::min(n, kMaxGhosts);
    for (size_t i = 0; i < show; ++i) v.shown.push_back(i * n / show);
    v.parts = std::move(parts);
    // a click still held from the menu does not place at once
    v.prevL = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    v.prevR = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    input::SetSwallow(PreviewKeys);
    g_previewOn = true;
    g_previewTotal = (int)n;
    g_previewShown = 0;
    log::Info("Blueprints", "Preview {} parts, {} ghosts", n, show);
}

void ToggleLock()
{
    Preview& v = g_pv;
    if (!v.active) return;
    v.locked = !v.locked;
    if (v.locked) { // keep the spot without height and nudge, which still apply on top
        v.lockAt = v.pl;
        v.lockAt.a = v.pl.a - v.pl.u * v.raise - v.pl.r * v.nudge.x - v.pl.f * v.nudge.z;
    }
    g_previewLocked = v.locked;
}

bool Pressed(int vk, bool& prev)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = down && !prev;
    prev = down;
    return edge;
}

bool GameFocused()
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

void PreviewTick()
{
    for (int k = 0; k < kRemovesPerFrame && !g_dropGhosts.empty(); ++k) {
        CallRemoveGhost(g_dropGhosts.back());
        g_dropGhosts.pop_back();
    }
    Preview& v = g_pv;
    if (!v.active) return;
    if (game::Data() != v.data || !game::PlayerState()) { // another save loaded, the ghosts went with it
        v.nodes.clear();
        EndPreview(false);
        return;
    }
    if (!ui::IsOpen() && GameFocused()) {
        if (Pressed(VK_RBUTTON, v.prevR)) { EndPreview(false); return; }
        if (Pressed(VK_LBUTTON, v.prevL)) { EndPreview(true); return; }
        if (Pressed('P', v.prevP)) ToggleLock();
        bool fine = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        float step = fine ? 0.02f : 0.1f;
        if (GetAsyncKeyState(VK_PRIOR) & 0x8000) v.raise += step;
        if (GetAsyncKeyState(VK_NEXT) & 0x8000) v.raise -= step;
        if (GetAsyncKeyState(VK_UP) & 0x8000) v.nudge.z += step;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) v.nudge.z -= step;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) v.nudge.x += step;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) v.nudge.x -= step;
        if (int wheel = input::TakeWheel()) v.turn += (float)wheel / WHEEL_DELTA * (fine ? 1.f : 15.f);
    }
    else input::TakeWheel();
    Place pl;
    // locked: the anchor stays put, height, nudge and turn still apply on top
    if (!MakePlace(v.ahead, v.raise, v.turn, v.nudge, pl, v.locked ? &v.lockAt : nullptr)) return;
    bool moved = Len(pl.a + Of(pl.off) - (v.pl.a + Of(v.pl.off))) > 0.005f || Dot(pl.f, v.pl.f) < 0.99999f || pl.base != v.pl.base;
    v.pl = pl;
    g_previewOk = pl.ok;
    if (moved) v.moveFrom = 0;
    // new ghosts go straight to the latest place
    for (int k = 0; k < kGhostsPerFrame && v.nodes.size() < v.shown.size(); ++k) {
        const Part& p = v.parts[v.shown[v.nodes.size()]];
        char id16[16] = {};
        memcpy(id16, p.id.c_str(), std::min<size_t>(p.id.size(), 15));
        Mat m = Xf(pl, p);
        v.nodes.push_back(CallAddGhost(id16, &m, pl.base));
    }
    g_previewShown = (int)v.nodes.size();
    size_t made = v.nodes.size();
    for (int k = 0; k < kMovesPerFrame && v.moveFrom < made; ++k, ++v.moveFrom) {
        uint32_t n = v.nodes[v.moveFrom];
        if (n == kNoNode) continue;
        Mat m = Xf(pl, v.parts[v.shown[v.moveFrom]]);
        CallMoveGhost(n, &m);
    }
}

void Tick()
{
    PreviewTick();
    Job& j = g_job;
    if (j.active) {
        size_t total = j.undo ? j.made.size() : j.parts.size();
        for (int k = 0; k < kPerFrame && j.next < total && !g_stop; ++k, ++j.next) {
            if (j.undo) CallDelete(&j.made[j.next]);
            else PlaceOne(j.parts[j.next]);
        }
        g_done = (int)j.next;
        if (j.next >= total || g_stop) {
            if (j.undo) {
                // anything not reached stays undoable
                g_undo.assign(j.made.begin() + j.next, j.made.end());
                SetStatus(L::Tf("bp.undone", std::to_string(j.next)));
                log::Info("Blueprints", "Undo removed {} parts", j.next);
            }
            else {
                g_undo = std::move(j.made);
                std::string s = L::Tf("bp.pasted", std::to_string(j.placed));
                if (j.failed) s += "  " + L::Tf("bp.failedn", std::to_string(j.failed));
                SetStatus(s);
                log::Info("Blueprints", "Paste done: {} placed, {} failed{}", j.placed, j.failed, g_stop ? ", stopped" : "");
            }
            g_undoCount = (int)g_undo.size();
            j = {};
            g_busy = g_busyUndo = false;
        }
        return;
    }
    if (game::Frame() % 30) return;
    Near n;
    Me me;
    if (Player(me)) {
        auto bases = Bases(me.off);
        if (const BaseInfo* b = Nearest(bases, me.pos, &n.dist)) {
            n.ok = true;
            n.name = b->name.empty() ? "#" + std::to_string(b->idx) : b->name;
            if (b->others && !b->owner.empty()) n.name += " (" + b->owner + ")";
            n.mine = !b->others && !b->generated;
            n.parts = UsedParts(b->p);
        }
    }
    std::lock_guard lk(g_mx);
    g_near = n;
}

// UI thread. The base computer (BASE_FLAG) is left out unless asked for: a second one can't be removed.
std::vector<Part> PartsOf(const std::string& key)
{
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    for (auto& b : g_list)
        if (b.file == (key.empty() ? g_focusKey : key)) {
            std::vector<Part> parts = b.parts;
            if (!g.withComputer) std::erase_if(parts, [](const Part& p) { return p.id == "BASE_FLAG"; });
            return parts;
        }
    return {};
}

// UI thread: show the blueprint as ghosts ahead of you; left click places it.
void ShowPreview(const std::string& key)
{
    std::vector<Part> parts = PartsOf(key);
    if (parts.empty()) { ui::Toast(L::T("bp.pickone"), "", icon::Warning); return; }
    if (!Live() || g_busy || !GhostsReady()) return;
    float ahead = g.ahead, raise = g.raise, turn = g.turn;
    ui::SetOpen(false);
    ui::Toast(L::T("bp.preview.title"), L::T("bp.preview.keys"), icon::Clone);
    game::Post([parts = std::move(parts), ahead, raise, turn]() mutable { StartPreview(std::move(parts), ahead, raise, turn); });
}

// UI thread: paste the given blueprint (or the focused one) with the page's placement.
void Paste(const std::string& key)
{
    std::vector<Part> parts = PartsOf(key);
    if (parts.empty()) { ui::Toast(L::T("bp.pickone"), "", icon::Warning); return; }
    if (!Live() || g_busy) return;
    float ahead = g.ahead, raise = g.raise, turn = g.turn;
    game::Post([parts = std::move(parts), ahead, raise, turn]() mutable { StartPaste(std::move(parts), ahead, raise, turn); });
}

// ---- UI ----

void Rebuild()
{
    g.entries.clear();
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    for (auto& b : g_list) {
        ui::BrowserEntry e;
        e.key = b.file;
        e.title = b.name;
        e.subtitle = L::Tf("bp.partsn", std::to_string(b.parts.size())) + (b.from.empty() ? "" : "   /   " + b.from);
        e.glyph = icon::Clone;
        e.count = std::to_string(b.parts.size());
        e.value = (double)b.parts.size();
        e.group = b.from;
        g.entries.push_back(std::move(e));
    }
    g.seen = g_version;
    g.browser.Invalidate();
}

void Label(ImDrawList* dl, float x, float& y, const char* key)
{
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T(key));
    y += S(22);
}

void DrawList(const char* query)
{
    {
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(8), half = (w - S(8)) / 2, x = p.x, y = p.y;
    bool live = Live();

    Near n;
    std::string status;
    {
        std::lock_guard lk(g_mx);
        n = g_near;
        status = g_status;
    }
    std::string where = !live ? std::string(L::T("bp.ingame"))
                       : n.ok ? L::Tf("bp.near", n.name, std::to_string(n.parts))
                              : std::string(L::T("bp.nobase"));
    dl->AddText(ui::theme::Body(), S(16), ImVec2(x, y), n.ok && live ? P().text : P().textDim, where.c_str());
    y += S(28);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##bpwhole", L::T("bp.whole"), half, g.around ? ui::Tone::Neutral : ui::Tone::Teal)) g.around = false;
    ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
    if (ui::Button("##bparound", L::T("bp.around"), half, g.around ? ui::Tone::Teal : ui::Tone::Neutral)) g.around = true;
    y += S(46);
    if (g.around) {
        Label(dl, x, y, "bp.radius");
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::EnergyBar("##bpradius", &g.radius, 2, 300, "%.0f m", w);
        y += S(36);
    }
    Label(dl, x, y, "bp.name");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::TextField("##bpname", g.name, sizeof g.name, w, L::T("bp.name.hint"));
    y += S(40);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##bpcopy", L::T(g.around ? "bp.copy.around" : "bp.copy.whole"), w, ui::Tone::Green, live && n.ok && !g_busy)) {
        bool around = g.around;
        float radius = g.radius;
        std::string name = g.name;
        g.name[0] = 0;
        game::Post([=] { Copy(around, radius, name); });
    }
    y += S(46);
    if (!status.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textBody, status.c_str(), nullptr, w);
        y += ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, w, status.c_str()).y + S(10);
    }
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), P().line, S(1));
    y += S(12);
    ImGui::SetCursorScreenPos(ImVec2(x, y));

    if (g.seen != g_version) Rebuild();
    if (g.entries.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("bp.empty"), nullptr, w);
        ImGui::Dummy(ImVec2(w, S(40)));
        return;
    }
    {
        std::lock_guard lk(g_mx);
        if (!g_focusKey.empty() && !g.browser.Focused(g.entries)) g.browser.SetFocus(g_focusKey);
    }
    g.browser.Draw(g.entries, query);
}

void Fact(ImDrawList* dl, float x, float& y, float w, const std::string& k, const std::string& v)
{
    if (v.empty()) return;
    y += ui::RichText(dl, ui::theme::Body(), S(15), ImVec2(x, y), w, P().textBody, "<TECHNOLOGY>" + k + "<> " + v) + S(4);
}

std::string Metres(float v)
{
    char b[24];
    snprintf(b, sizeof b, "%.0f", std::max(v, 1.f));
    return b;
}

float DrawInspector(ImVec2 a, float w)
{
    const ui::BrowserEntry* e = g.entries.empty() ? nullptr : g.browser.Focused(g.entries);
    if (!e) return 0; // default tab info
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ui::DrawEntryHeader(dl, a, w, *e);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    std::string key = e->key;
    size_t parts = 0;
    V3 lo, hi;
    std::string from, made;
    {
        std::lock_guard lk(g_mx);
        g_focusKey = key;
        for (auto& b : g_list)
            if (b.file == key) {
                parts = b.parts.size();
                lo = b.lo;
                hi = b.hi;
                from = b.from;
                char t[32] = {};
                time_t tt = (time_t)b.made;
                tm lt{};
                if (b.made && !localtime_s(&lt, &tt)) strftime(t, sizeof t, "%Y-%m-%d %H:%M", &lt);
                made = t;
            }
    }
    Fact(dl, x, y, bw, L::T("bp.f.parts"), std::to_string(parts));
    Fact(dl, x, y, bw, L::T("bp.f.size"), Metres(hi.x - lo.x) + " x " + Metres(hi.y - lo.y) + " x " + Metres(hi.z - lo.z) + " m");
    Fact(dl, x, y, bw, L::T("bp.f.from"), from);
    Fact(dl, x, y, bw, L::T("bp.f.made"), made);
    Fact(dl, x, y, bw, L::T("bp.f.file"), key);
    y += S(8);

    Label(dl, x, y, "bp.ahead");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::EnergyBar("##bpahead", &g.ahead, 0, 150, "%.0f m", bw);
    y += S(36);
    Label(dl, x, y, "bp.raise");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::EnergyBar("##bpraise", &g.raise, -30, 60, "%.1f m", bw);
    y += S(36);
    Label(dl, x, y, "bp.turn");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::EnergyBar("##bpturn", &g.turn, -180, 180, "%.0f deg", bw);
    y += S(40);
    auto toggle = [&](const char* id, const char* key, bool* v) {
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(4)), P().textDim, L::T(key));
        ImGui::SetCursorScreenPos(ImVec2(x + bw - S(70), y));
        ui::Switch(id, v);
        y += S(34);
    };
    toggle("##bpcomputer", "bp.withcomputer", &g.withComputer);
    bool anywhere = g_anywhere;
    toggle("##bpanywhere", "bp.anywhere", &anywhere);
    g_anywhere = anywhere;
    if (anywhere) {
        const char* note = L::T("bp.anywhere.note");
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().gold, note, nullptr, bw);
        y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + S(8);
    }
    y += S(6);

    bool live = Live();
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (g_busy) {
        std::string prog = L::Tf(g_busyUndo ? "bp.progress.undo" : "bp.progress", std::to_string(g_done.load()), std::to_string(g_total.load()));
        if (ui::Button("##bpstop", prog.c_str(), bw, ui::Tone::Red, true)) g_stop = true;
    }
    else if (g_previewOn) {
        // the preview is up: the same choices as the mouse, for when the menu is open
        std::string prog = L::Tf(g_previewLocked ? "bp.preview.statelocked" : "bp.preview.state", std::to_string(g_previewShown.load()),
                                 std::to_string(g_previewTotal.load()));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), g_previewOk ? P().textBody : P().gold, prog.c_str(), nullptr, bw);
        y += ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, bw, prog.c_str()).y + S(8);
        float third = (bw - S(16)) / 3;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##bppvplace", L::T("bp.preview.place"), third, ui::Tone::Green, live && g_previewOk)) game::Post([] { EndPreview(true); });
        ImGui::SetCursorScreenPos(ImVec2(x + third + S(8), y));
        if (ui::Button("##bppvlock", L::T(g_previewLocked ? "bp.preview.unlock" : "bp.preview.lock"), third, ui::Tone::Teal, live))
            game::Post(ToggleLock);
        ImGui::SetCursorScreenPos(ImVec2(x + (third + S(8)) * 2, y));
        if (ui::Button("##bppvcancel", L::T("bp.preview.cancel"), third, ui::Tone::Red, true)) game::Post([] { EndPreview(false); });
    }
    else {
        bool ghosts = GhostsReady();
        float main = ghosts ? bw - S(130) - S(8) : bw;
        if (ghosts && ui::Button("##bppreview", L::T(live ? "bp.preview" : "bp.ingame"), main, ui::Tone::Green, live && parts)) ShowPreview(key);
        ImGui::SetCursorScreenPos(ImVec2(ghosts ? x + main + S(8) : x, y));
        if (ui::Button("##bppaste", L::T(ghosts ? "bp.paste.now" : live ? "bp.paste" : "bp.ingame"), ghosts ? S(130) : bw,
                       ghosts ? ui::Tone::Neutral : ui::Tone::Green, live && parts))
            Paste(key);
    }
    y += S(46);
    int undo = g_undoCount;
    if (undo && g_delete) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        std::string lbl = L::Tf("bp.undo", std::to_string(undo));
        if (ui::Button("##bpundo", lbl.c_str(), bw, ui::Tone::Red, live && !g_busy)) game::Post(StartUndo);
        y += S(46);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##bpfolder", L::T("bp.folder"), bw, ui::Tone::Neutral, true)) {
        std::error_code ec;
        std::filesystem::create_directories(Folder(), ec);
        ShellExecuteW(nullptr, L"open", Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    y += S(46);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::HoldButton("##bpdelete", L::T("bp.delete"), bw)) {
        std::lock_guard lk(g_mx);
        std::error_code ec;
        std::filesystem::remove(Folder() / paths::Wide(key), ec);
        std::erase_if(g_list, [&](const Blueprint& b) { return b.file == key; });
        g_focusKey.clear();
        g.browser.ClearFocus();
        ++g_version;
    }
    y += S(50);
    return y;
}

}

namespace {

std::string TypeName(int32_t t)
{
    for (auto& [v, n] : TypeNames())
        if (v == t) return n;
    return std::to_string(t);
}

std::vector<SeenBase> Listed(bool others)
{
    std::vector<SeenBase> out;
    Vec4 off;
    for (auto& b : Bases(off)) {
        if (b.others != others || (!others && b.generated) || b.name.empty()) continue;
        SeenBase s{ b.name, b.owner, b.platform, TypeName(b.type), b.ua };
        for (int k = 0; k < 3; ++k) s.pos[k] = b.pos[k];
        out.push_back(std::move(s));
    }
    return out;
}

}

std::vector<SeenBase> OtherBasesHere() { return Listed(true); }

std::vector<SeenBase> MyBases() { return Listed(false); }

bool PlayerSpot(uint64_t& ua, double pos[3])
{
    Me me;
    if (!Player(me) || !CurrentAddress(ua)) return false;
    pos[0] = (double)me.pos.x + me.off.x;
    pos[1] = (double)me.pos.y + me.off.y;
    pos[2] = (double)me.pos.z + me.off.z;
    return true;
}

void InstallBlueprints()
{
    if (const Target& t = Loc().Resolve("Base.PlaceObject"); t.ok) g_place = (PlaceFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.Manager"); t.ok) g_mgr = t.global;
    if (const Target& t = Loc().Resolve("Player.InstanceOffset"); t.ok && t.offset) g_playerOff = *t.offset;
    if (const Target& t = Loc().Resolve("Base.List"); t.ok && t.offset) g_baseList = *t.offset;
    if (const Target& t = Loc().Resolve("Base.DeleteObject"); t.ok) {
        g_delete = (DeleteFn)Abs(t.func);
        // the handle offset is the first add to the object pointer: add rbx, 0x118
        auto in = Dec(t.func);
        for (int i = 0; in && i < 24; ++i, in = Next(*in))
            if (in->mnemonic == "add" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm && in->ops[1].imm > 0x40 &&
                in->ops[1].imm < 0x1000) {
                g_handleOff = (uint32_t)in->ops[1].imm;
                break;
            }
    }
    auto off = [](const char* f, uint32_t& out) { if (auto m = meta::Path("cGcPersistentBaseEntry", f)) out = m->offset; };
    off("At", R.at);
    off("Position", R.pos);
    off("Up", R.up);
    off("ObjectID", R.id);
    off("Timestamp", R.time);
    off("UserData", R.user);
    log::Info("Blueprints", "Place {}, delete {} (handle +0x{:X}), entry pos +0x{:X} id +0x{:X} time +0x{:X}", g_place ? "ready" : "missing",
              g_delete ? "ready" : "missing", g_handleOff, R.pos, R.id, R.time);
    if (const Target& t = Loc().Resolve("Base.AddGhost"); t.ok) g_addGhost = (AddGhostFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.RemoveGhost"); t.ok) g_removeGhost = (NodeFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.FindObject"); t.ok) g_findObj = (FindObjFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.ObjectSetMatrix"); t.ok) g_setMatrix = (SetMatrixFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.ObjectUpdateParts"); t.ok) g_updateParts = (UpdatePartsFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.ScanEffectOnChildren"); t.ok) g_scanFx = (ScanFxFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Base.EffectsManager"); t.ok) g_fxMgr = t.global;
    if (uintptr_t gp = Loc().GlobalsInstance("cGcGameplayGlobals")) {
        auto m = meta::Path("cGcGameplayGlobals", "BaseBuildingGhostScanEffect");
        g_ghostLook = gp + (m ? m->offset : 0x490); // fallback for the current build
    }
    log::Info("Blueprints", "Ghost preview {}, hologram look {}", GhostsReady() ? "ready" : "missing", g_scanFx && g_fxMgr && g_ghostLook ? "ready" : "missing");
    game::EveryFrame(Tick);

    if (!g_place || !g_mgr) Fail("bp.paste", "base placement not found");
    else {
        Bound b;
        b.id = "bp.paste";
        b.action = [] { if (GhostsReady()) ShowPreview(""); else Paste(""); };
        Ok(std::move(b));
    }
    if (!g_delete) Fail("bp.undo", "base delete not found");
    else {
        Bound b;
        b.id = "bp.undo";
        b.action = [] { if (Live() && !g_busy) game::Post(StartUndo); };
        Ok(std::move(b));
    }

    Registry::Get().Post([] {
        g.browser.chips = { L::T("bp.chip.all") };
        g.browser.searchBox = true;
        g.browser.sortable = true;
        g.browser.sortOptions = { { ui::SortBy::Default, L::T("bp.sort.newest") }, { ui::SortBy::Name, L::T("bp.sort.name") },
                                  { ui::SortBy::Value, L::T("bp.sort.parts") }, { ui::SortBy::Type, L::T("bp.sort.from") } };
        g.browser.captions = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] {
            std::lock_guard lk(g_mx);
            return g_loaded ? "(" + std::to_string(g_list.size()) + ")" : std::string();
        };
        ui::RegisterPage("blueprints", std::move(page));
    });
}

}
