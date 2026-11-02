#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/Settings.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstring>
#include <map>
#include <mutex>

namespace os::features {

namespace {

using ui::theme::P;
using ui::theme::S;

struct alignas(16) V4 { float x = 0, y = 0, z = 0, w = 0; };
struct alignas(16) BigPos { V4 local, offset; };

using CamMatrixFn = const V4* (*)(uintptr_t cam);
using ScreenPosFn = V4* (*)(uintptr_t cam, V4* out, const BigPos* pos);

struct
{
    int64_t cam = -1;
    uintptr_t camVt = 0;
    ScreenPosFn screenPos = nullptr;
    // Game POIs: the planet the game counts you on and its building list (target Planet.Buildings)
    int64_t sysPtr = -1, planets = -1, stride = -1, buildings = -1, current = -1, count = -1;
    uint32_t classOff = 0xD3;
} F;

// POI groups by cGcBuildingClassification name; bit i of Settings poiGroups. -1 = never shown.
enum PoiGroup { Buildings, Crashes, Ancient, Settlements, Sentinels };
std::vector<int> g_classGroup;        // by class value
std::vector<std::string> g_className; // display names by class value

int GroupOf(const std::string& n)
{
    static const std::pair<const char*, int> kGroups[] = {
        { "Shelter", Buildings }, { "Abandoned", Buildings }, { "WaterAbandoned", Buildings }, { "Shop", Buildings },
        { "Outpost", Buildings }, { "RadioTower", Buildings }, { "Observatory", Buildings }, { "Depot", Buildings },
        { "Factory", Buildings }, { "Harvester", Buildings }, { "MissionTower", Buildings }, { "LargeBuilding", Buildings },
        { "Beacon", Buildings },
        { "CrashedFreighter", Crashes }, { "WaterCrashedFreighter", Crashes }, { "Crashed_Swarm", Crashes },
        { "DistressSignal", Crashes }, { "WaterDistressSignal", Crashes }, { "NPCDistressSignal", Crashes },
        { "Debris", Crashes }, { "NPCDebris", Crashes }, { "DamagedMachine", Crashes }, { "CargoDrop", Crashes },
        { "Monolith", Ancient }, { "Plaque", Ancient }, { "Ruin", Ancient }, { "TreasureRuins", Ancient },
        { "WaterTreasureRuins", Ancient }, { "DigSite", Ancient }, { "AncientGuardian", Ancient }, { "Portal", Ancient },
        { "GraveInCave", Ancient }, { "StoryGlitch", Ancient }, { "RobotHead", Ancient },
        { "DroneHive", Sentinels }, { "AbandonedRobotCamp", Sentinels }, { "ScrapYard", Sentinels },
        { "SentinelDistressSignal", Sentinels },
    };
    for (auto& [k, g] : kGroups) if (n == k) return g;
    return n.rfind("Settlement", 0) == 0 ? Settlements : -1;
}

// "Settlement_SheriffsOffice" -> "Settlement Sheriffs Office"
std::string Pretty(const std::string& n)
{
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c == '_') { out += ' '; continue; }
        if (c >= 'A' && c <= 'Z' && i && n[i - 1] >= 'a' && n[i - 1] <= 'z') out += ' ';
        out += c;
    }
    return out;
}

void LoadClasses()
{
    for (auto& [v, n] : hooks::meta::EnumNames("cGcBuildingSpawnData", "Classification")) {
        if (v < 0 || v > 255) continue;
        if ((size_t)v >= g_classGroup.size()) { g_classGroup.resize(v + 1, -1); g_className.resize(v + 1); }
        g_classGroup[v] = GroupOf(n);
        g_className[v] = Pretty(n);
    }
    if (auto m = hooks::meta::Path("cGcBuildingSpawnData", "Classification")) F.classOff = m->offset;
}

// ---- game POIs ----
// The buildings of the planet the game counts you on. Their spawn position (+0x30) is in the same frame as the camera:
// the game's own markers split it into planet position plus the rest.
constexpr uint32_t kPos = 0x30;

bool RdV(uintptr_t a, V4& v);

// Game thread, every half second.
void PoiSpots(uint64_t here, int groups, std::vector<MarkerSpot>& out)
{
    uintptr_t sys = 0, arr = 0;
    uint32_t count = 0;
    int32_t index = 0, planets = 0;
    if (F.buildings < 0 || g_classGroup.empty() || !Rd(game::Data() + F.sysPtr, sys) || !sys) return;
    // the game's own pick, as cGcBuilding::Scanned does it: current index clamped to the planet count
    if (!Rd(sys + F.current, index) || !Rd(sys + F.count, planets) || planets <= 0 || planets > 16) return;
    index = std::clamp(index, 0, planets - 1);
    uintptr_t planet = sys + F.planets + index * (uintptr_t)F.stride;
    if (!Rd(planet + F.buildings, arr) || !Rd(planet + F.buildings - 4, count) || !arr || count > 20000) return;
    static uintptr_t reportedFor = 0;
    bool report = reportedFor != planet && count;
    if (report) reportedFor = planet;
    std::map<std::string, int> kinds;
    int listed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uintptr_t b = 0, sd = 0;
        uint8_t cls = 0;
        V4 p;
        if (!Rd(arr + i * 8ull, b) || !b || !Rd(b + 0x78, sd) || !sd || !Rd(sd + F.classOff, cls) || cls >= g_classGroup.size()) continue;
        if (report) ++kinds[g_className[cls]];
        int group = g_classGroup[cls];
        if (group < 0 || !(groups >> group & 1) || !RdV(b + kPos, p) || !std::isfinite(p.x) || (p.x == 0 && p.y == 0 && p.z == 0)) continue;
        out.push_back({ g_className[cls], here, { p.x, p.y, p.z }, 2 + group });
        ++listed;
    }
    if (report) {
        std::string list;
        for (auto& [k, n] : kinds) list += (list.empty() ? "" : ", ") + k + " " + std::to_string(n);
        log::Info("Markers", "POIs planet {} (address says {}): {} buildings ({}); {} marked", index, (here >> 52) & 0xF, count, list, listed);
    }
}

bool RdV(uintptr_t a, V4& v)
{
    __try { memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool Project(uintptr_t cam, const BigPos& p, V4& out)
{
    __try { return F.screenPos(cam, &out, &p) != nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Shown { std::string name; float x, y, dist; int kind; };

std::mutex g_mx;
std::vector<Shown> g_shown;     // screen spots for the render thread, x and y 0..1
std::vector<MarkerSpot> g_spots; // game thread
uint64_t g_lastFetch = 0;

constexpr uint64_t kSystem = 0x000FFFFFFFFFFFFFull; // galaxy, region and system; the planet sits above

// Game thread, every frame: project the spots of this system.
void Tick()
{
    auto& d = Settings::Get().Data();
    std::vector<Shown> out;
    uintptr_t data = game::Data(), vt = 0, fn = 0;
    uint64_t here = 0;
    bool live = d.markers && F.screenPos && F.cam >= 0 && data && game::PlayerState() && CurrentAddress(here);
    const V4* m = nullptr;
    if (live && Rd(data + F.cam, vt) && vt == F.camVt && Rd(vt + 0x48, fn) && fn) m = reinterpret_cast<CamMatrixFn>(fn)(data + F.cam);
    V4 at, local, offset;
    if (m && RdV(uintptr_t(m + 2), at) && RdV(uintptr_t(m + 3), local) && RdV(uintptr_t(m + 4), offset)) {
        if (game::Frame() - g_lastFetch >= 30) {
            g_lastFetch = game::Frame();
            g_spots.clear();
            if (d.markTeleports) g_spots = TeleportMarkers();
            if (d.markBases) for (auto& s : BaseMarkers()) g_spots.push_back(std::move(s));
            if (d.markPois && d.poiGroups) PoiSpots(here, d.poiGroups, g_spots);
        }
        double cx = (double)local.x + offset.x, cy = (double)local.y + offset.y, cz = (double)local.z + offset.z;
        double range = d.markerRange > 0 ? d.markerRange * 1000.0 : 1e30;
        for (auto& s : g_spots) {
            if ((s.ua & kSystem) != (here & kSystem)) continue;
            double dx = s.pos[0] - cx, dy = s.pos[1] - cy, dz = s.pos[2] - cz;
            double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > range) continue;
            if (dx * -at.x + dy * -at.y + dz * -at.z <= 0) continue; // behind the camera (at points backwards)
            BigPos p;
            p.offset = offset; // keep the big part exact, the projector adds the two
            p.local = { (float)(s.pos[0] - offset.x), (float)(s.pos[1] - offset.y), (float)(s.pos[2] - offset.z), 0 };
            V4 sp;
            if (!Project(data + F.cam, p, sp) || !std::isfinite(sp.x) || !std::isfinite(sp.y)) continue;
            if (sp.x < -0.1f || sp.x > 1.1f || sp.y < -0.1f || sp.y > 1.1f) continue;
            out.push_back({ s.name, sp.x, sp.y, (float)dist, s.kind });
        }
    }
    std::lock_guard lk(g_mx);
    g_shown = std::move(out);
}

std::string DistText(float m)
{
    char b[32];
    if (m < 1000) snprintf(b, sizeof b, "%.0f m", m);
    else snprintf(b, sizeof b, "%.1f km", m / 1000);
    return b;
}

}

void DrawMarkers()
{
    std::vector<Shown> list;
    {
        std::lock_guard lk(g_mx);
        list = g_shown;
    }
    if (list.empty()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont* font = ui::theme::Body();
    float fs = S(15), r = S(7);
    const auto& styles = Settings::Get().Data().markerStyles;
    for (auto& s : list) {
        ImVec2 c(s.x * size.x, s.y * size.y);
        int kind = std::clamp(s.kind, 0, kMarkerKinds - 1);
        const MarkerStyle& st = styles[kind];
        ImU32 col = st.icon ? (ImU32)st.icon : ui::MarkerColour(kind);
        // fade the far ones a little so close spots stand out
        float a = s.dist < 200 ? 1.f : std::max(0.45f, 1.f - (s.dist - 200) / 8000.f);
        ImVec2 d[4] = { { c.x, c.y - r }, { c.x + r, c.y }, { c.x, c.y + r }, { c.x - r, c.y } };
        dl->AddConvexPolyFilled(d, 4, ui::Alpha(col, a));
        dl->AddPolyline(d, 4, ui::Alpha(IM_COL32(0, 0, 0, 255), 0.6f * a), S(1.5f), ImDrawFlags_Closed);
        std::string dist = DistText(s.dist);
        ImVec2 tn = font->CalcTextSizeA(fs, FLT_MAX, 0, s.name.c_str()), td = font->CalcTextSizeA(fs, FLT_MAX, 0, dist.c_str());
        float y = c.y + r + S(4);
        auto text = [&](const std::string& t, ImVec2 ts, ImU32 tc) {
            ImVec2 at(c.x - ts.x / 2, y);
            dl->AddText(font, fs, ImVec2(at.x + 1, at.y + 1), ui::Alpha(IM_COL32(0, 0, 0, 255), 0.7f * a), t.c_str());
            dl->AddText(font, fs, at, ui::Alpha(tc, a), t.c_str());
            y += ts.y;
        };
        if (st.showName && !s.name.empty()) text(s.name, tn, st.name ? (ImU32)st.name : P().text);
        if (st.showDist) text(dist, td, st.dist ? (ImU32)st.dist : P().textDim);
    }
}

void InstallMarkers()
{
    if (const Target& t = Loc().Resolve("Camera.Manager"); t.ok && t.offset) {
        auto vts = Img().Vtables("cGcCameraManager");
        if (vts.size() == 1) { F.cam = *t.offset; F.camVt = Abs(vts[0]); }
    }
    if (const Target& t = Loc().Resolve("Camera.ScreenPos"); t.ok) F.screenPos = (ScreenPosFn)Abs(t.func);
    if (F.cam < 0 || !F.screenPos) {
        log::Warn("Markers", "Camera or screen projection not found, HUD markers are off");
        return;
    }
    const Target& sp = Loc().Resolve("Data.SolarSystem");
    const Target& pl = Loc().Resolve("SolarSystem.Planets");
    const Target& st = Loc().Resolve("SolarSystem.PlanetStride");
    const Target& bl = Loc().Resolve("Planet.Buildings");
    const Target& cu = Loc().Resolve("SolarSystem.CurrentPlanet");
    const Target& pc = Loc().Resolve("SolarSystem.PlanetCount");
    if (sp.ok && sp.offset && pl.ok && pl.offset && st.ok && st.offset && bl.ok && bl.offset && cu.ok && cu.offset && pc.ok && pc.offset) {
        F.sysPtr = *sp.offset; F.planets = *pl.offset; F.stride = *st.offset; F.buildings = *bl.offset;
        F.current = *cu.offset; F.count = *pc.offset;
    }
    LoadClasses();
    game::EveryFrame(Tick);
    log::Info("Markers", "Ready: camera +0x{:X}, POIs {} ({} classes)", F.cam, F.buildings >= 0 ? "on" : "off", g_className.size());
}

}
