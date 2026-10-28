#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "core/Settings.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>

// Effects tab: the game's own particle effects. Test one in front of you
// with the menu out of the way, place them as decorations that come back each visit. Most game
// effects play once, so the tab replays them: looped (again as soon as one ends) or on a timer.
namespace os::features {

namespace {

using json = nlohmann::json;
using ui::theme::P;
using ui::theme::S;

struct IdRow { const char* id; const char* table; };
const IdRow kIds[] = {
#include "features/EffectIds.inc"
};

// AddEffect_Internal(mgr, out, id, parent handle, PhysRelMat34*, permanent), IsActive(mgr, inst)
using AddFn = void* (*)(uintptr_t mgr, void* out, const void* id, uint32_t handle, const void* mat, bool permanent);
using ActiveFn = bool (*)(uintptr_t mgr, const void* inst);
using RetireFn = void (*)(uintptr_t mgr, void* inst);
AddFn g_add = nullptr;
ActiveFn g_active = nullptr;
RetireFn g_retire = nullptr;
uint32_t g_mgr = 0x4DE6C0, g_handle = 0x98; // Data offset, manager offset
int64_t g_playerOff = 0x72C6F0;

constexpr uint32_t kNone = 0x7FFFF; // EffectInstance handle of nothing
constexpr int kWarnAbove = 150;     // the game itself has room for 512 at once

struct alignas(16) Vec4 { float x = 0, y = 0, z = 0, w = 0; };
struct alignas(16) Mat { Vec4 right, up, at, local, offset; }; // cTkPhysRelMat34
struct alignas(16) Inst { uint32_t handle = kNone, pad = 0; uint64_t a = 0, b = 0, c = 0; };

// One effect being replayed: looped, or every `every` seconds.
struct Player
{
    std::string id;
    Mat m;
    bool loop = true;
    float every = 3;
    Inst inst;        // the copy playing now
    int64_t next = 0; // timer mode: next start, steady ms
};

struct Placed : Player
{
    int key = 0;
    uint64_t system = 0; // address without the planet digit
    bool live = false;   // playing in the current system
};

struct Test : Player
{
    bool on = false;
    int64_t end = 0;
};

// Custom weather: effects scattered around the player at random, wherever they go.
struct Sky
{
    std::string name;
    std::vector<std::string> set;
    float radius = 60, rate = 2, height = 0, spread = 10; // metres, per second, metres, metres
    float life = 6;      // seconds before a copy is stopped, for effects that never end on their own
    bool turn = false;   // random direction for each copy
    int base = -1;       // World > Force Weather type underneath, -1 = leave the real weather
    int intensity = 0;   // 0 normal, 1 extreme
    bool storms = false; // World > Force Storms
};

// A copy to stop at a set time if it is still playing (some game effects loop forever).
struct Reap { Inst inst; int64_t at = 0; };

// ---- state shared by the game thread and the page ----

std::mutex g_mx;
std::vector<Placed> g_placed; // under g_mx
std::set<std::string> g_favs; // under g_mx
int g_limit = 60;             // under g_mx
Test g_test;                  // under g_mx
Sky g_sky;                    // under g_mx
bool g_skyOn = false;         // under g_mx
std::vector<Sky> g_presets;   // under g_mx
std::vector<Reap> g_reap;     // under g_mx
int g_nextKey = 1;
bool g_loaded = false;
std::atomic<uint64_t> g_version{ 1 };
std::atomic<int> g_sel{ 0 };              // placed effect being edited; 0 = the newest here
std::atomic<float> g_moveStep{ 0.5f }, g_turnStep{ 15.f };

int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::filesystem::path File() { return std::filesystem::path(paths::Root()) / L"OpenSkies.effects.json"; }

json VecJ(const Vec4& v) { return json::array({ v.x, v.y, v.z }); }
void JVec(const json& a, Vec4& v)
{
    if (a.is_array() && a.size() >= 3) { v.x = a[0]; v.y = a[1]; v.z = a[2]; }
}

void Load() // under g_mx
{
    g_loaded = true;
    std::ifstream in(File());
    if (!in) return;
    try {
        json j = json::parse(in);
        g_limit = std::clamp(j.value("limit", 60), 1, 400);
        for (auto& f : j.value("favourites", json::array())) g_favs.insert(f.get<std::string>());
        auto sky = [](const json& e) {
            Sky k;
            k.name = e.value("name", "");
            for (auto& s : e.value("set", json::array())) k.set.push_back(s.get<std::string>());
            k.radius = std::clamp(e.value("radius", 60.f), 5.f, 1000.f);
            k.rate = std::clamp(e.value("rate", 2.f), 0.1f, 60.f);
            k.height = std::clamp(e.value("height", 0.f), -10.f, 500.f);
            k.spread = std::clamp(e.value("spread", 10.f), 0.f, 300.f);
            k.life = std::clamp(e.value("life", 6.f), 1.f, 30.f);
            k.turn = e.value("turn", false);
            k.base = std::clamp(e.value("base", -1), -1, 16);
            k.intensity = std::clamp(e.value("intensity", 0), 0, 1);
            k.storms = e.value("storms", false);
            return k;
        };
        if (j.contains("weather")) {
            g_sky = sky(j["weather"]);
            g_skyOn = j["weather"].value("on", false);
        }
        for (auto& e : j.value("presets", json::array())) g_presets.push_back(sky(e));
        for (auto& e : j.value("placed", json::array())) {
            Placed p;
            p.key = g_nextKey++;
            p.id = e.value("id", "");
            p.system = e.value("system", 0ull);
            p.loop = e.value("loop", true);
            p.every = std::clamp(e.value("every", 3.f), 0.5f, 60.f);
            JVec(e["right"], p.m.right);
            JVec(e["up"], p.m.up);
            JVec(e["at"], p.m.at);
            JVec(e["local"], p.m.local);
            JVec(e["offset"], p.m.offset);
            p.m.local.w = p.m.offset.w = 1;
            if (!p.id.empty()) g_placed.push_back(std::move(p));
        }
    }
    catch (...) { log::Warn("Effects", "Effects file could not be read"); }
}

void Save() // under g_mx
{
    json placed = json::array();
    for (auto& p : g_placed)
        placed.push_back({ { "id", p.id }, { "system", p.system }, { "loop", p.loop }, { "every", p.every }, { "right", VecJ(p.m.right) },
                           { "up", VecJ(p.m.up) }, { "at", VecJ(p.m.at) }, { "local", VecJ(p.m.local) }, { "offset", VecJ(p.m.offset) } });
    auto sky = [](const Sky& k) {
        return json{ { "name", k.name }, { "set", k.set }, { "radius", k.radius }, { "rate", k.rate }, { "height", k.height }, { "spread", k.spread },
                     { "life", k.life }, { "turn", k.turn }, { "base", k.base }, { "intensity", k.intensity }, { "storms", k.storms } };
    };
    json weather = sky(g_sky);
    weather["on"] = g_skyOn;
    json presets = json::array();
    for (auto& k : g_presets) presets.push_back(sky(k));
    json j = { { "limit", g_limit }, { "favourites", std::vector<std::string>(g_favs.begin(), g_favs.end()) }, { "placed", std::move(placed) },
               { "weather", std::move(weather) }, { "presets", std::move(presets) } };
    std::ofstream(File()) << j.dump(1);
}

// ---- game calls (game thread) ----

bool CallAdd(uintptr_t mgr, Inst* out, const char* id16, uint32_t handle, const Mat* m)
{
    __try { g_add(mgr, out, id16, handle, m, false); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallActive(uintptr_t mgr, const Inst* inst)
{
    __try { return g_active(mgr, inst); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uintptr_t Manager() { uintptr_t d = game::Data(); return d && g_add ? d + g_mgr : 0; }

uint64_t SystemNow()
{
    uint64_t ua = 0;
    return CurrentAddress(ua) && ua ? ua & 0x000FFFFFFFFFFFFFull : 0;
}

bool RdVec(uintptr_t a, Vec4& v) { return Rd(a, v.x) && Rd(a + 4, v.y) && Rd(a + 8, v.z); }

// The player's transform, moved `ahead` metres forward and `up` metres up. The body's `at`
// row points backwards, so ahead is along -at.
bool PlayerMat(float ahead, float up, Mat& m)
{
    uintptr_t d = game::Data();
    if (!d) return false;
    uintptr_t p = d + (uintptr_t)g_playerOff;
    if (!RdVec(p + 0x310, m.right) || !RdVec(p + 0x320, m.up) || !RdVec(p + 0x330, m.at) || !RdVec(p + 0x360, m.local) || !RdVec(p + 0x370, m.offset))
        return false;
    m.local.x += -m.at.x * ahead + m.up.x * up;
    m.local.y += -m.at.y * ahead + m.up.y * up;
    m.local.z += -m.at.z * ahead + m.up.z * up;
    m.local.w = m.offset.w = 1;
    return true;
}

// Plays one copy; the game cleans it up when it ends.
bool Spawn(const std::string& id, const Mat& m, Inst& out)
{
    uintptr_t mgr = Manager();
    if (!mgr) return false;
    char id16[16] = {};
    memcpy(id16, id.c_str(), std::min<size_t>(id.size(), 15));
    uint32_t handle = 0;
    Rd(mgr + g_handle, handle);
    out = Inst{};
    return CallAdd(mgr, &out, id16, handle, &m) && (out.handle & kNone) != kNone;
}

void CallRetire(uintptr_t mgr, Inst* inst)
{
    __try { g_retire(mgr, inst); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool Playing(const Inst& i)
{
    uintptr_t mgr = Manager();
    if ((i.handle & kNone) == kNone || !mgr) return false;
    return g_active ? CallActive(mgr, &i) : false;
}

// Stops a copy if it is still playing. IsActive checks the handle, so a slot the game has
// reused for something else is left alone.
void Kill(Inst& i)
{
    if (Playing(i) && g_retire)
        if (uintptr_t mgr = Manager()) CallRetire(mgr, &i);
    i = Inst{};
}

void ReapLater(const Inst& i, int64_t at) // under g_mx
{
    if ((i.handle & kNone) == kNone) return;
    if (g_reap.size() > 4000) g_reap.erase(g_reap.begin()); // oldest first
    g_reap.push_back({ i, at });
}

void ReapStep(int64_t t) // under g_mx
{
    std::erase_if(g_reap, [t](Reap& r) {
        if (t < r.at) return false;
        Kill(r.inst);
        return true;
    });
}

// Starts the next copy when it is due: looped once the last one ended, else on the timer.
// Timer copies are stopped after two rounds, so effects that never end don't pile up.
void Step(Player& p, int64_t t, int64_t stopAt = 0)
{
    if (p.loop && g_active) {
        if (!Playing(p.inst)) Spawn(p.id, p.m, p.inst);
        return;
    }
    if (t < p.next) return;
    float every = std::max(p.loop ? 1.f : p.every, 0.5f); // no IsActive: loop as a 1 s timer
    if (Spawn(p.id, p.m, p.inst)) ReapLater(p.inst, stopAt ? stopAt : t + (int64_t)(std::max(every * 2, 6.f) * 1000));
    p.next = t + (int64_t)(every * 1000);
}

int LiveCount() // under g_mx
{
    int n = 0;
    for (auto& p : g_placed) n += p.live;
    return n;
}

// Starts this system's placements up to the limit. Under g_mx.
void StartHere(uint64_t sys)
{
    int live = LiveCount();
    for (auto& p : g_placed) {
        if (p.system != sys || p.live || live >= g_limit) continue;
        p.live = true;
        p.inst = Inst{};
        p.next = 0;
        ++live;
    }
}

// ---- moving and turning a placed effect (game thread) ----

enum Op { MoveLeft, MoveRight, MoveFwd, MoveBack, MoveUp, MoveDown, TurnLeft, TurnRight, TiltUp, TiltDown, RollLeft, RollRight, ToMe, OpCount };
const char* const kOpIds[OpCount] = { "fx.move_left", "fx.move_right", "fx.move_fwd", "fx.move_back", "fx.move_up", "fx.move_down",
                                      "fx.turn_left", "fx.turn_right", "fx.tilt_up", "fx.tilt_down", "fx.roll_left", "fx.roll_right", "fx.to_me" };

// v turned by `rad` around the unit axis k (Rodrigues).
Vec4 Turn(const Vec4& v, const Vec4& k, float rad)
{
    float c = std::cos(rad), s = std::sin(rad), d = (k.x * v.x + k.y * v.y + k.z * v.z) * (1 - c);
    Vec4 x{ k.y * v.z - k.z * v.y, k.z * v.x - k.x * v.z, k.x * v.y - k.y * v.x, v.w };
    return { v.x * c + x.x * s + k.x * d, v.y * c + x.y * s + k.y * d, v.z * c + x.z * s + k.z * d, v.w };
}

void Nudge(Vec4& p, const Vec4& axis, float d) { p.x += axis.x * d; p.y += axis.y * d; p.z += axis.z * d; }

// Applies one edit to the selected placed effect (the newest one here if none is picked), then
// restarts it so the change shows at once.
void Adjust(int op)
{
    uint64_t sys = SystemNow();
    if (!sys) return;
    std::lock_guard lk(g_mx);
    Placed* p = nullptr;
    for (auto& q : g_placed)
        if (q.system == sys && (q.key == g_sel || (!g_sel && (!p || q.key > p->key)))) p = &q;
    if (!p) return;
    Mat& m = p->m;
    float step = g_moveStep, rad = g_turnStep * 3.14159265f / 180.f;
    switch (op) {
    case MoveLeft: Nudge(m.local, m.right, -step); break;
    case MoveRight: Nudge(m.local, m.right, step); break;
    case MoveFwd: Nudge(m.local, m.at, -step); break; // at points back
    case MoveBack: Nudge(m.local, m.at, step); break;
    case MoveUp: Nudge(m.local, m.up, step); break;
    case MoveDown: Nudge(m.local, m.up, -step); break;
    case TurnLeft: case TurnRight: {
        float r = op == TurnLeft ? rad : -rad;
        m.right = Turn(m.right, m.up, r);
        m.at = Turn(m.at, m.up, r);
        break;
    }
    case TiltUp: case TiltDown: {
        float r = op == TiltUp ? rad : -rad;
        m.up = Turn(m.up, m.right, r);
        m.at = Turn(m.at, m.right, r);
        break;
    }
    case RollLeft: case RollRight: {
        float r = op == RollLeft ? rad : -rad;
        m.right = Turn(m.right, m.at, r);
        m.up = Turn(m.up, m.at, r);
        break;
    }
    case ToMe: {
        Mat here;
        if (!PlayerMat(2, 0, here)) return;
        m.local = here.local;
        m.offset = here.offset;
        break;
    }
    }
    Kill(p->inst);
    p->next = 0;
    g_sel = p->key;
    Save();
    ++g_version;
}

// Custom weather: `rate` effects a second at random spots in a disc of `radius` around the
// player, lifted by `height` plus up to `spread`. Each one is a one-shot the game cleans up. Under g_mx.
void SkyStep(int64_t t)
{
    static int64_t last = 0;
    static double carry = 0;
    static std::mt19937 rng{ std::random_device{}() };
    int64_t dt = last ? std::min<int64_t>(t - last, 250) : 0;
    last = t;
    if (!g_skyOn || g_sky.set.empty() || !game::PlayerState()) { carry = 0; return; }
    carry += g_sky.rate * dt / 1000.0;
    std::uniform_real_distribution<float> u(0.f, 1.f);
    for (int n = 0; carry >= 1 && n < 60; ++n, carry -= 1) {
        Mat m;
        if (!PlayerMat(0, 0, m)) return;
        if (g_sky.turn) {
            float yaw = u(rng) * 6.2831853f, tilt = (u(rng) - 0.5f) * 1.5707963f;
            m.right = Turn(m.right, m.up, yaw);
            m.at = Turn(m.at, m.up, yaw);
            m.up = Turn(m.up, m.right, tilt);
            m.at = Turn(m.at, m.right, tilt);
        }
        float a = u(rng) * 6.2831853f, r = g_sky.radius * std::sqrt(u(rng)), h = g_sky.height + g_sky.spread * u(rng);
        float c = std::cos(a) * r, s = std::sin(a) * r;
        m.local.x += m.right.x * c + m.at.x * s + m.up.x * h;
        m.local.y += m.right.y * c + m.at.y * s + m.up.y * h;
        m.local.z += m.right.z * c + m.at.z * s + m.up.z * h;
        Inst i;
        if (Spawn(g_sky.set[rng() % g_sky.set.size()], m, i)) ReapLater(i, t + (int64_t)(g_sky.life * 1000));
    }
}

// Replays placed effects, runs the test and custom weather, and reopens the menu after a test.
void Tick()
{
    int64_t t = NowMs();
    static uint64_t lastSys = 0, settleFrame = 0, last = 0;
    {
        std::lock_guard lk(g_mx);
        if (g_test.on) {
            if (t >= g_test.end) {
                g_test.on = false;
                Kill(g_test.inst);
                Registry::Get().Post([] { ui::SetOpen(true); });
            }
            else Step(g_test, t, g_test.end);
        }
        ReapStep(t);
        if (lastSys && !settleFrame)
            for (auto& p : g_placed)
                if (p.live && p.system == lastSys) Step(p, t);
        if (!g_loaded) Load();
        SkyStep(t);
    }
    uint64_t now = game::Frame();
    if (now - last < 30) return;
    last = now;
    uint64_t sys = SystemNow();
    if (!sys || !game::PlayerState()) return;
    if (sys != lastSys) { // a new system: the old effects went with it
        lastSys = sys;
        settleFrame = now + 240;
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
        for (auto& p : g_placed) { p.live = false; p.inst = Inst{}; }
        ++g_version;
        return;
    }
    if (settleFrame && now >= settleFrame) {
        settleFrame = 0;
        std::lock_guard lk(g_mx);
        StartHere(sys);
        ++g_version;
    }
}

// ---- grouping ----

struct Group { const char* key; unsigned glyph; ImU32 tint; std::vector<const char*> words; };
const Group kGroups[] = {
    { "fx.g.fireworks", 0xf0d0, IM_COL32(255, 120, 200, 255), { "FIREWORK", "FW", "CONFETTI", "FANCYFW" } },
    { "fx.g.weather", 0xf76c, IM_COL32(150, 190, 255, 255), { "TORNADO", "STORM", "METEOR", "LIGHTNING", "DUSTDEVIL", "CYCLONE", "GASGIANT" } },
    { "fx.g.warp", 0xf7c0, IM_COL32(180, 140, 255, 255), { "WARP", "PORTAL", "TELEPORT", "ATLAS", "GATE", "RUNE", "NEXUS", "GALAXY", "ANOMALY", "CORE" } },
    { "fx.g.fire", 0xf06d, IM_COL32(255, 140, 60, 255), { "FIRE", "FLAME", "BURN", "LAVA", "INCIN", "HEAT", "FURNACE", "MAGMA", "VOL" } },
    { "fx.g.toxic", 0xf780, IM_COL32(150, 230, 90, 255), { "TOXIC", "TOX", "RADIO", "ACID", "SPORE", "GOO", "SLIME", "BIO", "CORRUP", "CORR" } },
    { "fx.g.water", 0xf043, IM_COL32(90, 180, 255, 255), { "WATER", "SPLASH", "BUBBLE", "UNDER", "SWIM", "RIPPLE", "DRIP", "FLOAT", "UW_" } },
    { "fx.g.sparks", 0xf0e7, IM_COL32(255, 220, 90, 255), { "SPARK", "ELEC", "ARC", "PLASMA", "ENERGY", "GLOW", "LIGHT", "FLASH", "LASER" } },
    { "fx.g.smoke", 0xf0c2, IM_COL32(190, 190, 200, 255), { "SMOKE", "STEAM", "VENT", "MIST", "FOG", "CLOUD", "HAZE", "DUST", "ASH", "EXHAUST" } },
    { "fx.g.explosions", 0xf1e2, IM_COL32(255, 90, 70, 255), { "EXPL", "EXP", "DESTR", "BURST", "BLAST", "IMPACT", "IMP", "HIT", "DEBRIS", "DMG", "DAMAGE" } },
    { "fx.g.creatures", 0xf1b0, IM_COL32(230, 170, 120, 255), { "CREATURE", "BLOOD", "FIEND", "QUEEN", "WORM", "JELLY", "PET", "EGG", "BUG", "SQUID", "FISH", "GOLEM" } },
    { "fx.g.trails", 0xf135, IM_COL32(120, 230, 230, 255), { "JET", "TRAIL", "ENGINE", "THRUST", "HOVER", "MUZ", "WHEEL", "ROVER", "NOMAD", "MECH" } },
};
constexpr int kGroupCount = (int)(sizeof(kGroups) / sizeof(kGroups[0]));

int GroupOf(const std::string& id)
{
    for (int g = 0; g < kGroupCount; ++g)
        for (const char* w : kGroups[g].words)
            if (id.find(w) != std::string::npos) return g;
    return kGroupCount; // other
}

// "MAINT_SPARKS" -> "Maint Sparks"
std::string Pretty(const std::string& id)
{
    std::string o;
    bool start = true;
    for (char c : id) {
        if (c == '_') { o += ' '; start = true; continue; }
        o += start ? c : (char)tolower((unsigned char)c);
        start = false;
    }
    return o;
}

// ---- the page (UI thread) ----

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    uint64_t version = 0;
    float ahead = 6, up = 0, seconds = 8, every = 3;
    bool loop = true;
    bool built = false;
    int tab = 0;    // 0 browse, 1 custom weather
    int preset = 0; // picked preset
    char presetName[48] = {};
} g;

void Rebuild()
{
    std::set<std::string> favs;
    {
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
        favs = g_favs;
    }
    g.view.clear();
    static const char* kTables[] = { "Planet", "Space", "Explosion", "Player", "Ship", "Pet battle" };
    for (auto& r : kIds) {
        ui::BrowserEntry e;
        e.key = r.id;
        e.title = Pretty(r.id);
        int grp = GroupOf(r.id);
        e.chip = grp + 1;
        e.group = grp < kGroupCount ? L::T(kGroups[grp].key) : L::T("fx.g.other");
        e.glyph = grp < kGroupCount ? kGroups[grp].glyph : icon::Star;
        e.tint = grp < kGroupCount ? kGroups[grp].tint : 0;
        e.subtitle = e.group + "   /   " + L::T(("fx.t." + std::string(r.table)).c_str()) + "   /   " + r.id;
        for (int t = 0; t < 6; ++t)
            if (!strcmp(r.table, kTables[t])) e.value = 6 - t; // game table order
        e.rare = favs.count(r.id) != 0;
        e.rarity = e.rare ? 1 : 0; // favourites first when sorted by rarity
        g.view.push_back(std::move(e));
    }
    g.browser.Invalidate();
    g.built = true;
}

bool Live() { return game::FrameHookReady() && game::PlayerState() && g_add && SystemNow(); }

void Bar(ImDrawList* dl, float x, float& y, float w, const char* label, const char* id, float* v, float lo, float hi, const char* fmt);

// The real weather underneath comes from the World tab's Force Weather and Force Storms, so the
// two pages always agree. Reads and writes those features (UI thread).
void ReadBase(Sky& k)
{
    auto& r = Registry::Get();
    if (Feature* f = r.Find("world.weather")) {
        k.base = f->on && !f->values.empty() ? std::clamp((int)std::lround(f->values[0]), 0, 16) : -1;
        k.intensity = f->values.size() > 1 ? std::clamp((int)std::lround(f->values[1]), 0, 1) : 0;
    }
    if (Feature* f = r.Find("world.storm_force")) k.storms = f->on;
}

void ApplyBase(const Sky& k)
{
    auto& r = Registry::Get();
    if (Feature* f = r.Find("world.weather")) {
        if (k.base >= 0) {
            r.SetSlider(*f, 0, (float)k.base);
            r.SetSlider(*f, 1, (float)k.intensity);
        }
        r.SetOn(*f, k.base >= 0);
    }
    if (Feature* f = r.Find("world.storm_force")) r.SetOn(*f, k.storms);
    Settings::Get().MarkDirty();
}

// The custom weather view: on/off, the effects in it, how they spread, and saved presets.
void DrawSky()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = std::min(ImGui::GetContentRegionAvail().x - S(8), S(620));
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = p.x, y = p.y;
    Sky sky;
    bool on;
    std::vector<std::string> presets;
    {
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
        sky = g_sky;
        on = g_skyOn;
        for (auto& k : g_presets) presets.push_back(k.name);
    }
    bool changed = false;

    dl->AddText(ui::theme::Body(), S(16), ImVec2(x, y + S(4)), P().text, L::T("fx.sky.on"));
    ImGui::SetCursorScreenPos(ImVec2(x + w - S(70), y));
    bool want = on;
    if (ui::Switch("##skyon", &want, !sky.set.empty() || on)) changed = true;
    y += S(36);
    const char* hint = L::T(sky.set.empty() ? "fx.sky.empty" : "fx.sky.hint");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, hint, nullptr, w);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, w, hint).y + S(10);

    // the real weather underneath (World > Weather)
    ReadBase(sky);
    {
        static std::vector<std::string> types, strengths;
        if (types.empty()) {
            types.push_back(L::T("fx.sky.base.off"));
            for (int i = 0; i <= 16; ++i) types.push_back(L::T(("weather.t." + std::to_string(i)).c_str()));
            strengths = { L::T("weather.i.0"), L::T("weather.i.1") };
        }
        float lw = S(150), dw = w - lw;
        int pick = sky.base + 1, strength = sky.intensity;
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(8)), P().textDim, L::T("fx.sky.base"));
        ImGui::SetCursorScreenPos(ImVec2(x + lw, y));
        bool baseChanged = ui::Dropdown("##skybase", types, &pick, dw);
        y += S(42);
        if (pick > 0) {
            dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(8)), P().textDim, L::T("fx.sky.intensity"));
            ImGui::SetCursorScreenPos(ImVec2(x + lw, y));
            baseChanged |= ui::Dropdown("##skyintensity", strengths, &strength, dw);
            y += S(42);
        }
        bool storms = sky.storms;
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(4)), P().textDim, L::T("fx.sky.storms"));
        ImGui::SetCursorScreenPos(ImVec2(x + w - S(70), y));
        baseChanged |= ui::Switch("##skystorms", &storms);
        y += S(40);
        if (baseChanged) {
            sky.base = pick - 1;
            sky.intensity = strength;
            sky.storms = storms;
            ApplyBase(sky);
            changed = true;
        }
    }
    dl->AddLine(ImVec2(x, y + S(2)), ImVec2(x + w, y + S(2)), P().line, S(1));
    y += S(12);

    // the effects in this weather
    std::string drop;
    for (auto& id : sky.set) {
        ImGui::PushID(id.c_str());
        int grp = GroupOf(id);
        std::string gl = ui::Utf8(grp < kGroupCount ? kGroups[grp].glyph : icon::Star);
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(5)), grp < kGroupCount ? kGroups[grp].tint : P().textDim, gl.c_str());
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(26), y + S(5)), P().text, Pretty(id).c_str());
        ImGui::SetCursorScreenPos(ImVec2(x + w - S(90), y));
        if (ui::Button("##skyrm", L::T("fx.remove"), S(90), ui::Tone::Red, true)) drop = id;
        ImGui::PopID();
        y += S(34);
    }
    if (!drop.empty()) { std::erase(sky.set, drop); changed = true; }
    y += S(8);

    Sky before = sky;
    Bar(dl, x, y, w, "fx.sky.radius", "##skyradius", &sky.radius, 5, 1000, "%.0f m");
    Bar(dl, x, y, w, "fx.sky.rate", "##skyrate", &sky.rate, 0.1f, 60, "%.1f / s");
    Bar(dl, x, y, w, "fx.sky.height", "##skyheight", &sky.height, -10, 500, "%.0f m");
    Bar(dl, x, y, w, "fx.sky.spread", "##skyspread", &sky.spread, 0, 300, "%.0f m");
    Bar(dl, x, y, w, "fx.sky.life", "##skylife", &sky.life, 1, 30, "%.0f s");
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(4)), P().textDim, L::T("fx.sky.turn"));
    ImGui::SetCursorScreenPos(ImVec2(x + w - S(70), y));
    ui::Switch("##skyturn", &sky.turn);
    y += S(40);
    if (sky.radius != before.radius || sky.rate != before.rate || sky.height != before.height || sky.spread != before.spread ||
        sky.life != before.life || sky.turn != before.turn) changed = true;

    // a surprise: up to 15 random effects and random settings, saved as a preset
    bool random = false;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##skyrandom", L::T("fx.sky.random"), w, ui::Tone::Teal, true)) random = true;
    y += S(48);

    // presets
    dl->AddLine(ImVec2(x, y + S(2)), ImVec2(x + w, y + S(2)), P().line, S(1));
    y += S(12);
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("fx.sky.presets"));
    y += S(24);
    int load = -1, del = -1;
    bool save = false;
    if (!presets.empty()) {
        g.preset = std::clamp(g.preset, 0, (int)presets.size() - 1);
        float pw = S(100);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::Dropdown("##skypreset", presets, &g.preset, w - pw * 2 - S(12));
        ImGui::SetCursorScreenPos(ImVec2(x + w - pw * 2 - S(6), y));
        if (ui::Button("##skyload", L::T("fx.sky.load"), pw, ui::Tone::Teal, true)) load = g.preset;
        ImGui::SetCursorScreenPos(ImVec2(x + w - pw, y));
        if (ui::Button("##skydel", L::T("fx.remove"), pw, ui::Tone::Red, true)) del = g.preset;
        y += S(46);
    }
    float sw = S(130);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool enter = ui::TextField("##skyname", g.presetName, sizeof g.presetName, w - sw - S(6), L::T("fx.sky.namehint"));
    ImGui::SetCursorScreenPos(ImVec2(x + w - sw, y));
    if ((ui::Button("##skysave", L::T("fx.sky.save"), sw, ui::Tone::Green, !sky.set.empty()) || enter) && !sky.set.empty()) save = true;
    y += S(46);

    if (changed || load >= 0 || del >= 0 || save || random) {
        std::lock_guard lk(g_mx);
        if (load >= 0 && load < (int)g_presets.size()) sky = g_presets[load];
        if (random) {
            static std::mt19937 rng{ std::random_device{}() };
            std::uniform_real_distribution<float> u(0.f, 1.f);
            std::vector<const char*> pool;
            for (auto& r : kIds)
                if (strcmp(r.table, "Pet battle") && strcmp(r.table, "Ship")) pool.push_back(r.id); // those need a creature or ship
            std::shuffle(pool.begin(), pool.end(), rng);
            sky = Sky{};
            int n = 3 + (int)(rng() % 13); // 3 to 15
            for (int k = 0; k < n && k < (int)pool.size(); ++k) sky.set.push_back(pool[k]);
            sky.radius = 30 + u(rng) * 370;
            sky.rate = 1 + u(rng) * 19;
            sky.height = u(rng) * 80;
            sky.spread = u(rng) * 60;
            sky.life = 4 + u(rng) * 8;
            sky.turn = u(rng) < 0.5f;
            sky.base = u(rng) < 0.6f ? (int)(rng() % 17) : -1;
            sky.intensity = u(rng) < 0.3f ? 1 : 0;
            sky.storms = u(rng) < 0.4f;
            int num = 1;
            for (auto& k : g_presets)
                if (k.name.rfind(L::T("fx.sky.randomname"), 0) == 0) ++num;
            sky.name = std::string(L::T("fx.sky.randomname")) + " " + std::to_string(num);
            g_presets.push_back(sky);
            g.preset = (int)g_presets.size() - 1;
            want = true;
        }
        if (load >= 0 || random) ApplyBase(sky);
        g_sky = sky;
        g_skyOn = want && !g_sky.set.empty();
        if (del >= 0 && del < (int)g_presets.size()) g_presets.erase(g_presets.begin() + del);
        if (save) {
            Sky k = g_sky;
            k.name = g.presetName[0] ? std::string(g.presetName) : L::Tf("fx.sky.unnamed", std::to_string(g_presets.size() + 1));
            std::erase_if(g_presets, [&](const Sky& o) { return o.name == k.name; }); // same name replaces
            g_presets.push_back(std::move(k));
            g.preset = (int)g_presets.size() - 1;
            g.presetName[0] = 0;
        }
        Save();
    }
    ImGui::Dummy(ImVec2(w, y - p.y + S(10)));
}

void DrawList(const char* query)
{
    if (!g.built || g.version != g_version) {
        g.version = g_version;
        Rebuild();
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(8), bw = (w - S(8)) / 2;
    size_t n;
    bool on;
    {
        std::lock_guard lk(g_mx);
        n = g_sky.set.size();
        on = g_skyOn;
    }
    std::string sky = std::string(L::T("fx.sky"));
    if (n) sky += "  (" + std::to_string(n) + (on ? ", " + std::string(L::T("fx.sky.isOn")) : std::string()) + ")";
    ImGui::SetCursorScreenPos(p);
    if (ui::Button("##fxtabbrowse", L::T("fx.browse"), bw, g.tab == 0 ? ui::Tone::Teal : ui::Tone::Neutral)) g.tab = 0;
    ImGui::SetCursorScreenPos(ImVec2(p.x + bw + S(8), p.y));
    if (ui::Button("##fxtabsky", sky.c_str(), bw, g.tab == 1 ? ui::Tone::Teal : ui::Tone::Neutral)) g.tab = 1;
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(46)));
    if (g.tab == 1) { DrawSky(); return; }

    // what is placed here, at the top; pick one to move or turn it
    uint64_t sys = SystemNow();
    struct Row { int key; std::string id; bool live; };
    std::vector<Row> here;
    {
        std::lock_guard lk(g_mx);
        for (auto& pl : g_placed)
            if (pl.system == sys) here.push_back({ pl.key, pl.id, pl.live });
    }
    if (!here.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 q = ImGui::GetCursorScreenPos();
        float y = q.y;
        std::string head = L::Tf("fx.placedhere", std::to_string(here.size()));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(q.x, y), P().textDim, head.c_str());
        y += S(24);
        for (auto& r : here) {
            ImGui::SetCursorScreenPos(ImVec2(q.x, y));
            ImGui::PushID(r.key);
            if (ImGui::InvisibleButton("##placedrow", ImVec2(w, S(30)))) {
                g_sel = g_sel == r.key ? 0 : r.key;
                g.browser.ClearFocus();
            }
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = g_sel == r.key;
            dl->AddRectFilled(ImVec2(q.x, y), ImVec2(q.x + w, y + S(30)), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 90), S(4));
            int grp = GroupOf(r.id);
            std::string gl = ui::Utf8(grp < kGroupCount ? kGroups[grp].glyph : icon::Star);
            dl->AddText(ui::theme::Body(), S(15), ImVec2(q.x + S(10), y + S(6)), grp < kGroupCount ? kGroups[grp].tint : P().textDim, gl.c_str());
            std::string name = Pretty(r.id) + (r.live ? "" : "   " + std::string(L::T("fx.notlive")));
            dl->AddText(ui::theme::Body(), S(15), ImVec2(q.x + S(36), y + S(6)), on || hov ? P().text : P().textDim, name.c_str());
            y += S(34);
        }
        ImGui::SetCursorScreenPos(ImVec2(q.x, y + S(6)));
    }
    g.browser.Draw(g.view, query);
}

void Fact(ImDrawList* dl, float x, float& y, float w, const std::string& k, const std::string& v)
{
    if (v.empty()) return;
    y += ui::RichText(dl, ui::theme::Body(), S(15), ImVec2(x, y), w, P().textBody, "<TECHNOLOGY>" + k + "<> " + v) + S(4);
}

void Bar(ImDrawList* dl, float x, float& y, float w, const char* label, const char* id, float* v, float lo, float hi, const char* fmt)
{
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T(label));
    y += S(22);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::EnergyBar(id, v, lo, hi, fmt, w);
    y += S(36);
}

float DrawEffect(ImDrawList* dl, ImVec2 a, float w, const ui::BrowserEntry& f)
{
    float y = ui::DrawEntryHeader(dl, a, w, f);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    std::string id = f.key;
    Fact(dl, x, y, bw, L::T("fx.f.group"), f.group);
    Fact(dl, x, y, bw, L::T("fx.f.id"), id);
    y += S(6);
    Bar(dl, x, y, bw, "fx.ahead", "##fxahead", &g.ahead, 1, 40, "%.0f m");
    Bar(dl, x, y, bw, "fx.up", "##fxup", &g.up, -5, 20, "%.1f m");

    // loop: again as soon as one ends; off: again every n seconds
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(4)), P().textDim, L::T("fx.loop"));
    ImGui::SetCursorScreenPos(ImVec2(x + bw - S(70), y));
    ui::Switch("##fxloop", &g.loop);
    y += S(34);
    if (!g.loop) Bar(dl, x, y, bw, "fx.every", "##fxevery", &g.every, 0.5f, 30, "%.1f s");
    Bar(dl, x, y, bw, "fx.seconds", "##fxsec", &g.seconds, 2, 30, "%.0f s");
    bool live = Live();

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##fxtest", L::T(live ? "fx.test" : "fx.ingame"), bw, ui::Tone::Teal, live)) {
        float ahead = g.ahead, up = g.up, every = g.every;
        bool loop = g.loop;
        int64_t ms = (int64_t)(g.seconds * 1000);
        ui::SetOpen(false); // out of the way so the effect can be seen
        game::Post([=] {
            Test t;
            bool ok = PlayerMat(ahead, up, t.m) && Spawn(id, t.m, t.inst);
            log::Info("Effects", "Test {} -> {}", id, ok ? "playing" : "failed");
            int64_t now = NowMs();
            t.id = id;
            t.loop = loop;
            t.every = every;
            t.next = now + (int64_t)(every * 1000);
            t.on = true;
            t.end = now + (ok ? ms : 300);
            std::lock_guard lk(g_mx);
            g_test = std::move(t);
            if (!ok) Registry::Get().Post([id] { ui::Toast(L::T("fx.failed"), id, icon::Warning); });
        });
    }
    y += S(46);

    int count = 0, limit = 0;
    {
        std::lock_guard lk(g_mx);
        count = LiveCount();
        limit = g_limit;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool room = count < limit;
    if (ui::Button("##fxplace", L::T(room ? "fx.place" : "fx.full"), bw, ui::Tone::Green, live && room)) {
        float ahead = g.ahead, up = g.up, every = g.every;
        bool loop = g.loop;
        std::string title = f.title;
        game::Post([=] {
            Placed p;
            uint64_t sys = SystemNow();
            if (!sys || !PlayerMat(ahead, up, p.m)) return;
            p.id = id;
            p.system = sys;
            p.loop = loop;
            p.every = every;
            bool ok = Spawn(id, p.m, p.inst);
            {
                std::lock_guard lk(g_mx);
                if (!g_loaded) Load();
                p.key = g_nextKey++;
                p.live = ok;
                p.next = NowMs() + (int64_t)(every * 1000);
                if (ok) { g_placed.push_back(p); Save(); }
            }
            ++g_version;
            log::Info("Effects", "Placed {} ({}) -> {}", id, loop ? "loop" : "timer", ok ? "live" : "failed");
            Registry::Get().Post([ok, title] { ui::Toast(L::T(ok ? "fx.placed" : "fx.failed"), title, ok ? icon::Check : icon::Warning); });
        });
    }
    y += S(46);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##fxfav", L::T(f.rare ? "fx.unfav" : "fx.fav"), bw, ui::Tone::Neutral, true)) {
        std::lock_guard lk(g_mx);
        if (f.rare) g_favs.erase(id);
        else g_favs.insert(id);
        Save();
        ++g_version;
    }
    y += S(46);

    bool inSky;
    {
        std::lock_guard lk(g_mx);
        inSky = std::find(g_sky.set.begin(), g_sky.set.end(), id) != g_sky.set.end();
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##fxsky", L::T(inSky ? "fx.sky.drop" : "fx.sky.add"), bw, ui::Tone::Neutral, true)) {
        std::lock_guard lk(g_mx);
        if (inSky) std::erase(g_sky.set, id);
        else g_sky.set.push_back(id);
        if (g_sky.set.empty()) g_skyOn = false;
        Save();
    }
    y += S(46);
    return y;
}

float DrawPlaced(ImDrawList* dl, ImVec2 a, float w)
{
    ui::BrowserEntry head;
    head.title = L::T("tab.effects");
    head.subtitle = L::T("fx.subtitle");
    head.body = L::T("fx.help");
    head.glyph = icon::Wand;
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    uint64_t sys = SystemNow();

    struct Row { int key; std::string id; bool live, loop; float every; };
    std::vector<Row> here;
    int live = 0, total = 0;
    float limit = 60;
    {
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
        for (auto& p : g_placed) {
            ++total;
            live += p.live;
            if (p.system == sys) here.push_back({ p.key, p.id, p.live, p.loop, p.every });
        }
        limit = (float)g_limit;
    }
    Fact(dl, x, y, bw, L::T("fx.f.here"), std::to_string(here.size()));
    Fact(dl, x, y, bw, L::T("fx.f.live"), std::to_string(live) + " / " + std::to_string((int)limit));
    Fact(dl, x, y, bw, L::T("fx.f.saved"), std::to_string(total));
    y += S(6);

    // the player's own cap; the game has room for 512 effects in total
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), P().textDim, L::T("fx.limit"));
    y += S(22);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::EnergyBar("##fxlimit", &limit, 1, 400, "%.0f", bw)) {
        std::lock_guard lk(g_mx);
        g_limit = (int)limit;
        Save();
    }
    y += S(36);
    if (limit > kWarnAbove) {
        const char* warn = L::T("fx.limitwarn");
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().gold, warn, nullptr, bw);
        y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, warn).y + S(8);
    }

    bool on = Live();
    if (!here.empty()) {
        dl->AddLine(ImVec2(x, y + S(2)), ImVec2(x + bw, y + S(2)), P().line, S(1));
        y += S(12);
        int remove = 0;
        for (auto& r : here) {
            ImGui::PushID(r.key);
            char secs[24];
            snprintf(secs, sizeof secs, "%.1f s", r.every);
            std::string name = Pretty(r.id) + "   " + (r.loop ? std::string(L::T("fx.looping")) : L::Tf("fx.everyshort", secs)) + (r.live ? "" : "   " + std::string(L::T("fx.notlive")));
            dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y + S(5)), r.live ? P().text : P().textFaint, name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(x + bw - S(90), y));
            if (ui::Button("##fxrm", L::T("fx.remove"), S(90), ui::Tone::Red, on)) remove = r.key;
            ImGui::PopID();
            y += S(34);
        }
        if (remove) {
            game::Post([remove] {
                std::lock_guard lk(g_mx);
                for (auto& p : g_placed)
                    if (p.key == remove) Kill(p.inst);
                std::erase_if(g_placed, [remove](const Placed& p) { return p.key == remove; });
                Save();
                ++g_version;
            });
        }
        y += S(6);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##fxrespawn", L::T("fx.respawn"), bw, ui::Tone::Teal, on)) {
            game::Post([] {
                uint64_t s = SystemNow();
                std::lock_guard lk(g_mx);
                for (auto& p : g_placed)
                    if (p.system == s) { Kill(p.inst); p.live = false; }
                StartHere(s);
                ++g_version;
            });
        }
        y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::HoldButton("##fxclear", L::T("fx.clear"), bw) && on) {
            game::Post([] {
                uint64_t s = SystemNow();
                std::lock_guard lk(g_mx);
                for (auto& p : g_placed)
                    if (p.system == s) Kill(p.inst);
                std::erase_if(g_placed, [s](const Placed& p) { return p.system == s; });
                Save();
                ++g_version;
            });
        }
        y += S(46);
    }
    const char* note = L::T("fx.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
    return y;
}

// Move and turn buttons for the picked placed effect, one compact row each. Each is also a hotkey.
float DrawEdit(ImDrawList* dl, ImVec2 a, float w, const std::string& id, int key)
{
    ui::BrowserEntry head;
    head.title = Pretty(id);
    head.subtitle = L::T("fx.edit.sub");
    int grp = GroupOf(id);
    head.glyph = grp < kGroupCount ? kGroups[grp].glyph : icon::Star;
    head.tint = grp < kGroupCount ? kGroups[grp].tint : 0;
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    bool live = Live();
    float half = (bw - S(8)) / 2;

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##fxeditrm", L::T("fx.remove"), half, ui::Tone::Red, live)) {
        g_sel = 0;
        game::Post([key] {
            std::lock_guard lk(g_mx);
            for (auto& p : g_placed)
                if (p.key == key) Kill(p.inst);
            std::erase_if(g_placed, [key](const Placed& p) { return p.key == key; });
            Save();
            ++g_version;
        });
    }
    ImGui::SetCursorScreenPos(ImVec2(x + half + S(8), y));
    if (ui::Button("##fxeditdone", L::T("fx.edit.done"), half, ui::Tone::Neutral, true)) g_sel = 0;
    y += S(44);

    float lw = S(96), cw = (bw - lw - S(6)) / 2;
    auto pair = [&](const char* label, int a1, int a2) {
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y + S(7)), P().textDim, L::T(label));
        for (int k = 0; k < 2; ++k) {
            int op = k ? a2 : a1;
            ImGui::SetCursorScreenPos(ImVec2(x + lw + k * (cw + S(6)), y));
            ImGui::PushID(op);
            if (ui::Button("##op", L::T((std::string(kOpIds[op]) + ".short").c_str()), cw, ui::Tone::Neutral, live))
                game::Post([op] { Adjust(op); });
            ImGui::PopID();
        }
        y += S(38);
    };
    auto step = [&](const char* label, const char* sid, std::atomic<float>& v, float lo, float hi, const char* fmt) {
        dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y + S(5)), P().textDim, L::T(label));
        ImGui::SetCursorScreenPos(ImVec2(x + lw, y));
        float f = v;
        ui::EnergyBar(sid, &f, lo, hi, fmt, bw - lw);
        v = f;
        y += S(34);
    };
    pair("fx.edit.sideways", MoveLeft, MoveRight);
    pair("fx.edit.forward", MoveFwd, MoveBack);
    pair("fx.edit.height", MoveUp, MoveDown);
    step("fx.edit.step", "##fxstep", g_moveStep, 0.05f, 5, "%.2f m");
    pair("fx.edit.turn", TurnLeft, TurnRight);
    pair("fx.edit.tilt", TiltUp, TiltDown);
    pair("fx.edit.roll", RollLeft, RollRight);
    step("fx.edit.angle", "##fxangle", g_turnStep, 1, 90, "%.0f deg");
    y += S(4);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##fxtome", L::T("fx.to_me.short"), bw, ui::Tone::Teal, live)) game::Post([] { Adjust(ToMe); });
    y += S(44);
    const char* note = L::T("fx.edit.help");
    dl->AddText(ui::theme::Body(), S(13), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(13), FLT_MAX, bw, note).y + S(12);
    return y;
}

float DrawInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (const ui::BrowserEntry* f = g.browser.Focused(g.view)) return DrawEffect(dl, a, w, *f);
    if (int key = g_sel) {
        std::string id;
        {
            uint64_t sys = SystemNow();
            std::lock_guard lk(g_mx);
            for (auto& p : g_placed)
                if (p.key == key && p.system == sys) id = p.id;
        }
        if (!id.empty()) return DrawEdit(dl, a, w, id, key);
        g_sel = 0;
    }
    return DrawPlaced(dl, a, w);
}

}

void InstallEffects()
{
    if (const Target& t = Loc().Resolve("Effects.Add"); t.ok) g_add = (AddFn)Abs(t.func);
    else log::Warn("Effects", "Add missing: {}", t.reason);
    if (const Target& t = Loc().Resolve("Effects.IsActive"); t.ok) g_active = (ActiveFn)Abs(t.func);
    else log::Warn("Effects", "IsActive missing, loop falls back to a 1 s timer: {}", t.reason);
    if (const Target& t = Loc().Resolve("Effects.Retire"); t.ok) g_retire = (RetireFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Effects.Manager"); t.ok && t.offset) g_mgr = (uint32_t)*t.offset;
    if (const Target& t = Loc().Resolve("Effects.DefaultHandle"); t.ok && t.offset) g_handle = (uint32_t)*t.offset;
    if (const Target& t = Loc().Resolve("Player.InstanceOffset"); t.ok && t.offset) g_playerOff = *t.offset;
    log::Info("Effects", "{} effects; manager +0x{:X}, handle +0x{:X}, spawn {}, loop {}", std::size(kIds), g_mgr, g_handle,
              g_add ? "ready" : "unavailable", g_active ? "ready" : "timer");
    game::EveryFrame(Tick);

    // hotkeys: edit the picked placed effect, and switch custom weather
    for (int op = 0; op < OpCount; ++op) {
        if (!g_add) { Fail(kOpIds[op], "effects unavailable"); continue; }
        Bound b;
        b.id = kOpIds[op];
        b.action = [op] { if (game::FrameHookReady()) game::Post([op] { Adjust(op); }); };
        Ok(std::move(b));
    }
    if (g_add) {
        Bound sky;
        sky.id = "fx.sky_toggle";
        sky.action = [] {
            bool on;
            {
                std::lock_guard lk(g_mx);
                if (!g_loaded) Load();
                g_skyOn = !g_skyOn && !g_sky.set.empty();
                on = g_skyOn;
                Save();
            }
            ui::Toast(L::T(on ? "fx.sky.toast.on" : "fx.sky.toast.off"), "", icon::Wand);
        };
        Ok(std::move(sky));
    }
    else Fail("fx.sky_toggle", "effects unavailable");

    Registry::Get().Post([] {
        std::vector<std::string> chips{ L::T("fx.g.all") };
        for (auto& gr : kGroups) chips.push_back(L::T(gr.key));
        chips.push_back(L::T("fx.g.other"));
        g.browser.chips = std::move(chips);
        g.browser.searchBox = true;
        g.browser.sortable = true;
        g.browser.sortOptions = { { ui::SortBy::Name, L::T("fx.sort.name") }, { ui::SortBy::Type, L::T("fx.sort.group") },
                                  { ui::SortBy::Value, L::T("fx.sort.table") }, { ui::SortBy::Rarity, L::T("fx.sort.favs") } };
        g.browser.captions = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { return "(" + std::to_string(std::size(kIds)) + ")"; };
        ui::RegisterPage("effects", std::move(page));
    });
}

}
