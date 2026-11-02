#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

// .nmsship files are zips; stb_image's inflate reads their deflated entries.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>

namespace os::features {

namespace {

using ui::theme::P;
using ui::theme::S;
using json = nlohmann::json;
namespace meta = hooks::meta;

// One library corvette. Files are .nmsbase-style JSON (Objects, BaseType PlayerShipBase), so other base tools can open them;
// Open Skies keeps its extras under "OpenSkies".
struct Ship
{
    std::string file, name, from, owner;
    int64_t made = 0;
    int cls = -1;
    std::vector<CorvettePart> parts;
    json inv = json::object(); // general, tech, cargo stacks when the source had them
    json ownership;            // a .nmsship so.json, kept for applying later
    float lo[3] = {}, hi[3] = {};
};

std::mutex g_mx; // g_list, g_status, g_focusKey, g_head
std::vector<Ship> g_list;
bool g_loaded = false;
uint64_t g_version = 1;
std::string g_status, g_focusKey;

// Header facts, refreshed on the game thread.
struct Head
{
    bool live = false, hasNear = false, mine = false;
    std::string nearName;
    float nearDist = 0;
    size_t nearParts = 0, loaded = 0;
    int slotsUsed = 0, slotsTotal = 0, corvettes = 0;
    int32_t limit = -1;
    std::vector<std::pair<int, std::string>> yours; // slot and name of each corvette you own that is loaded
} g_head;

// The game's in-place corvette base rebuild, read from TakeOwnership (target Corvette.RebuildBaseSite).
using ClearFn = void (*)(uintptr_t mgr, const uint16_t* base);
using ObjFn = void (*)(uintptr_t obj);
using PopulateFn = void (*)(uintptr_t buffer, uintptr_t data);
using InitFn = void (*)(uintptr_t buffer, uintptr_t data, int32_t type);
using AddNodesFn = void (*)(uintptr_t mgr, const uint16_t* base, bool);
using PopulateShipFn = void (*)(uintptr_t ownership, int32_t slot);
using ReassembleFn = void (*)(void* component, int32_t slot);
struct
{
    uintptr_t mgr = 0;
    ClearFn clear = nullptr;
    ObjFn ownerDefaults = nullptr, baseDefaults = nullptr, reposition = nullptr, link = nullptr;
    PopulateFn populate = nullptr;
    InitFn init = nullptr;
    AddNodesFn addNodes = nullptr;
    PopulateShipFn populateShip = nullptr; // Corvette.PopulateShipFromBase, on Data + ownership (Ship.OwnershipOffset)
    ReassembleFn reassemble = nullptr;     // Corvette.ReassembleTech: tech from the base's parts, as the workshop does
    int64_t ownership = -1;
    int32_t type = 0x14;
    // cGcPersistentBase and cGcPersistentBaseEntry, from the game's metadata when it has them
    uint32_t size = 0x2B0, objects = 0x40, owner = 0x68, entry = 0x90;
    uint32_t at = 0, pos = 0x10, up = 0x20, id = 0x30, time = 0x40, user = 0x48;
    bool Ready() const { return mgr && clear && ownerDefaults && baseDefaults && populate && reposition && init && link && addNodes; }
} RB;

// A corvette into a new slot, the way the game transfers ships from saved data (target Corvette.RedeemShipSite):
// fill the purchase item from a ship record, then open the game's own ship purchase page.
using SetupShipFn = void (*)(uintptr_t item, const void* ship, const void* look, const void* base, bool, int32_t cost, int32_t currency, bool);
using PageFn = void (*)(uintptr_t frontend, int32_t page, bool);
using ContainerFn = void (*)(uintptr_t store, void* container);
using TechStoresFn = void (*)(uintptr_t buffer, uintptr_t tech, uintptr_t general);
struct
{
    SetupShipFn setup = nullptr;
    PageFn page = nullptr;
    ContainerFn container = nullptr; // Inventory.PopulateContainer
    TechStoresFn techStores = nullptr; // Corvette.ReassembleTechStores: tech from parts, into any two stores
    int64_t itemTech = -1, itemGeneral = -1; // the purchase item's own stores (Purchase.TechStore / GeneralStore)
    int64_t item = -1, frontend = -1;
    int32_t purchasePage = 0x26;
    // cGcPlayerOwnershipData, cGcCharacterCustomisationSaveData and cGcInventoryContainer, from the metadata when it has them
    uint32_t size = 0x4E0, look = 0x68, inv = 0x20, cargo = 0x180, tech = 0x2E0, res = 0x440, name = 0x4B8, nameLen = 0x20, slots = 0x10;
    bool Ready() const { return setup && page && container && item > 0 && frontend > 0; }
} NS;

uintptr_t g_shipGlobals = 0;
uint32_t g_limitOff = 0x468; // cGcBuildableShipGlobals.ComplexityLimitWarning

std::filesystem::path Folder() { return std::filesystem::path(paths::Root()) / L"OpenSkies.corvettes"; }

void SetStatus(std::string s)
{
    std::lock_guard lk(g_mx);
    g_status = std::move(s);
}

// ---- reading files ----

std::string Lower(std::string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void Vec(const json& a, float out[3])
{
    if (a.is_array() && a.size() >= 3)
        for (int k = 0; k < 3; ++k) out[k] = a[k].is_number() ? a[k].get<float>() : 0.f;
}

bool PartOf(const json& o, CorvettePart& p)
{
    if (!o.is_object()) return false;
    p.id = o.value("ObjectID", "");
    if (!p.id.empty() && p.id[0] == '^') p.id.erase(0, 1);
    if (p.id.empty() || p.id.size() >= 16) return false;
    auto num = [&](const char* k) -> uint64_t {
        auto it = o.find(k);
        if (it == o.end() || !it->is_number()) return 0;
        return it->is_number_unsigned() ? it->get<uint64_t>() : it->is_number_integer() ? (uint64_t)it->get<int64_t>() : (uint64_t)it->get<double>();
    };
    p.user = num("UserData");
    p.time = num("Timestamp");
    if (!p.time) p.time = (uint64_t)std::time(nullptr);
    Vec(o.value("Position", json()), p.p);
    Vec(o.value("Up", json()), p.up);
    Vec(o.value("At", json()), p.at);
    return true;
}

void Bounds(Ship& s)
{
    for (int k = 0; k < 3; ++k) s.lo[k] = s.hi[k] = s.parts.empty() ? 0.f : s.parts[0].p[k];
    for (auto& p : s.parts)
        for (int k = 0; k < 3; ++k) { s.lo[k] = std::min(s.lo[k], p.p[k]); s.hi[k] = std::max(s.hi[k], p.p[k]); }
}

// Objects from any of the shapes people share: a full base entry, a Base Builder prefab, or a bare objects array.
bool FromJson(const json& j, Ship& s)
{
    const json* objs = nullptr;
    if (j.is_array()) objs = &j;
    else if (j.is_object() && j.contains("Objects") && j["Objects"].is_array()) objs = &j["Objects"];
    else if (j.is_object() && j.contains("Prefab") && j["Prefab"].is_array()) objs = &j["Prefab"];
    if (!objs) return false;
    for (auto& o : *objs) {
        CorvettePart p;
        if (PartOf(o, p)) s.parts.push_back(std::move(p));
    }
    if (j.is_object()) {
        std::string n = j.value("Name", "");
        if (!n.empty() && Lower(n) != "default") s.name = n;
        if (j.contains("OpenSkies") && j["OpenSkies"].is_object()) {
            const json& x = j["OpenSkies"];
            s.from = x.value("from", "");
            s.owner = x.value("owner", "");
            s.made = x.value("made", (int64_t)0);
            s.cls = x.value("class", -1);
            if (x.contains("inventories")) s.inv = x["inventories"];
            if (x.contains("ownership")) s.ownership = x["ownership"];
        }
    }
    return !s.parts.empty();
}

// Minimal zip reader: central directory, stored or deflated entries.
bool ZipEntry(const std::string& zip, const std::string& want, std::string& out)
{
    auto u16 = [&](size_t o) { return o + 2 <= zip.size() ? (uint32_t)(uint8_t)zip[o] | (uint32_t)(uint8_t)zip[o + 1] << 8 : 0u; };
    auto u32 = [&](size_t o) { return o + 4 <= zip.size() ? u16(o) | u16(o + 2) << 16 : 0u; };
    if (zip.size() < 22) return false;
    size_t eocd = std::string::npos;
    for (size_t i = zip.size() - 22 + 1; i-- > 0 && zip.size() - i < 22 + 65536;)
        if (u32(i) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) return false;
    uint32_t n = u16(eocd + 10), cd = u32(eocd + 16);
    for (uint32_t k = 0, o = cd; k < n && u32(o) == 0x02014b50; ++k) {
        uint32_t method = u16(o + 10), csize = u32(o + 20), usize = u32(o + 24), nl = u16(o + 28), el = u16(o + 30), cl = u16(o + 32), lh = u32(o + 42);
        std::string name = o + 46 + nl <= zip.size() ? zip.substr(o + 46, nl) : std::string();
        o += 46 + nl + el + cl;
        if (Lower(name) != want || u32(lh) != 0x04034b50 || usize > (64u << 20)) continue;
        size_t data = lh + 30 + u16(lh + 26) + u16(lh + 28);
        if (data + csize > zip.size()) return false;
        if (method == 0) { out = zip.substr(data, csize); return true; }
        if (method != 8) return false;
        out.assign(usize, '\0');
        int got = stbi_zlib_decode_noheader_buffer(out.data(), (int)usize, zip.data() + data, (int)csize);
        if (got < 0) return false;
        out.resize(got);
        return true;
    }
    return false;
}

bool ReadFile(const std::filesystem::path& f, Ship& s)
{
    std::ifstream in(f, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string ext = Lower(paths::Utf8(f.extension().wstring()));
    s.file = paths::Utf8(f.filename().wstring());
    s.name = paths::Utf8(f.stem().wstring());
    std::error_code ec;
    auto when = std::filesystem::last_write_time(f, ec);
    if (!ec) s.made = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::clock_cast<std::chrono::system_clock>(when).time_since_epoch()).count();
    try {
        if (ext == ".nmsship") {
            std::string objs, so;
            if (!ZipEntry(text, "objects.json", objs)) return false;
            if (ZipEntry(text, "so.json", so)) {
                s.ownership = json::parse(so, nullptr, false);
                if (s.ownership.is_discarded()) s.ownership = json();
                else if (s.ownership.is_object()) {
                    std::string n = s.ownership.value("Name", "");
                    if (!n.empty() && Lower(n) != "default") s.name = n;
                }
            }
            std::string name = s.name;
            bool ok = FromJson(json::parse(objs), s);
            s.name = name;
            return ok;
        }
        return FromJson(json::parse(text), s);
    }
    catch (...) { return false; }
}

void Load() // under g_mx
{
    g_loaded = true;
    g_list.clear();
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    for (auto& f : std::filesystem::directory_iterator(Folder(), ec)) {
        std::string ext = Lower(paths::Utf8(f.path().extension().wstring()));
        if (ext != ".json" && ext != ".txt" && ext != ".nmsbase" && ext != ".nmsprefab" && ext != ".nmsship") continue;
        Ship s;
        if (!ReadFile(f.path(), s)) {
            log::Warn("Corvettes", "Could not read {}", paths::Utf8(f.path().filename().wstring()));
            continue;
        }
        Bounds(s);
        g_list.push_back(std::move(s));
    }
    std::sort(g_list.begin(), g_list.end(), [](auto& a, auto& b) { return a.made > b.made; });
    ++g_version;
}

// ---- writing ----

json J3(const float v[3]) { return json::array({ v[0], v[1], v[2] }); }

json StacksJson(const std::vector<ShipStack>& st)
{
    json a = json::array();
    for (auto& s : st) a.push_back({ { "id", s.id }, { "type", s.type }, { "x", s.x }, { "y", s.y }, { "amount", s.amount }, { "max", s.max } });
    return a;
}

bool Write(const Ship& s)
{
    json objs = json::array();
    for (auto& p : s.parts)
        objs.push_back({ { "Timestamp", p.time }, { "ObjectID", "^" + p.id }, { "UserData", p.user },
                         { "Position", J3(p.p) }, { "Up", J3(p.up) }, { "At", J3(p.at) } });
    json x = { { "from", s.from }, { "owner", s.owner }, { "made", s.made }, { "class", s.cls }, { "inventories", s.inv } };
    if (!s.ownership.is_null()) x["ownership"] = s.ownership;
    json j = { { "Name", s.name }, { "BaseVersion", 8 }, { "OriginalBaseVersion", 8 }, { "UserData", 0 },
               { "LastUpdateTimestamp", s.made }, { "Objects", std::move(objs) },
               { "BaseType", { { "PersistentBaseTypes", "PlayerShipBase" } } }, { "OpenSkies", std::move(x) } };
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    std::ofstream out(Folder() / paths::Wide(s.file));
    if (!out) return false;
    out << j.dump(1);
    return (bool)out;
}

std::string FileFor(const std::string& name)
{
    std::string s;
    for (char c : name) s += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : c == ' ' ? '_' : '\0';
    std::erase(s, '\0');
    if (s.empty()) s = "corvette";
    s = s.substr(0, 40);
    std::string f = s + ".json";
    for (int n = 2; std::filesystem::exists(Folder() / paths::Wide(f)); ++n) f = s + "_" + std::to_string(n) + ".json";
    return f;
}

// ---- copy (game thread) ----

std::string ShownName(const CorvetteSeen& c)
{
    std::string shown = c.mine && c.slot >= 0 ? ShipName(c.slot) : std::string();
    if (shown.empty()) shown = c.name;
    if (shown.empty() && !c.owner.empty()) shown = L::Tf("corv.othersname", c.owner);
    return shown;
}

// Two builds are the same when every part matches in order (ID, colour, place).
uint64_t Sig(const std::vector<CorvettePart>& parts)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ ((const uint8_t*)p)[i]) * 1099511628211ull;
    };
    for (auto& p : parts) {
        mix(p.id.data(), p.id.size());
        mix(&p.user, 8);
        for (int k = 0; k < 3; ++k) {
            int32_t q[3] = { (int32_t)std::lround(p.p[k] * 1000), (int32_t)std::lround(p.up[k] * 1000), (int32_t)std::lround(p.at[k] * 1000) };
            mix(q, sizeof q);
        }
    }
    return h;
}

// The same parts in any place: a claimed corvette's base may be re-centred by the game.
uint64_t IdSig(const std::vector<CorvettePart>& parts)
{
    std::vector<std::string> ids;
    for (auto& p : parts) ids.push_back(p.id);
    std::sort(ids.begin(), ids.end());
    uint64_t h = 1469598103934665603ull;
    for (auto& id : ids)
        for (char c : id + '|') h = (h ^ (uint8_t)c) * 1099511628211ull;
    return h;
}

// The library file that already holds this exact build, or empty.
std::string AlreadySaved(const std::vector<CorvettePart>& parts)
{
    uint64_t sig = Sig(parts);
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    for (auto& s : g_list)
        if (s.parts.size() == parts.size() && Sig(s.parts) == sig) return s.file;
    return {};
}

// Saves a loaded corvette to the library; returns the file, or empty when writing failed.
std::string Save(CorvetteSeen c, std::string name)
{
    Ship s;
    std::string shown = ShownName(c);
    s.name = !name.empty() ? name : !shown.empty() ? shown : std::string("Corvette");
    s.from = c.mine ? L::T("corv.from.mine") : shown;
    s.owner = c.owner;
    s.made = (int64_t)std::time(nullptr);
    s.parts = std::move(c.parts);
    // your own corvettes also bring their class, tech and cargo; other players' only arrive as parts
    if (c.mine && c.slot >= 0) {
        s.cls = ShipClass(c.slot);
        const char* keys[] = { "general", "tech", "cargo" };
        for (int w = 0; w < 3; ++w) {
            std::vector<ShipStack> st;
            if (ShipStacks(c.slot, w, st)) s.inv[keys[w]] = StacksJson(st);
        }
    }
    Bounds(s);
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    s.file = FileFor(s.name);
    if (!Write(s)) return {};
    log::Info("Corvettes", "Copied {} ({} parts, {}) into {}", shown, s.parts.size(), c.mine ? "yours" : "another player's", s.file);
    std::string file = s.file;
    g_list.insert(g_list.begin(), std::move(s));
    ++g_version;
    return file;
}

void CopyNearest(std::string name)
{
    if (!game::PlayerState()) { SetStatus(L::T("corv.ingame")); return; }
    auto all = CorvettesLoaded();
    if (all.empty()) { SetStatus(L::T("corv.none")); return; }
    size_t n = all[0].parts.size();
    std::string file = Save(std::move(all[0]), name);
    std::lock_guard lk(g_mx);
    if (file.empty()) { g_status = L::T("corv.writefail"); return; }
    for (auto& s : g_list)
        if (s.file == file) g_status = L::Tf("corv.copied", s.name, std::to_string(n));
    g_focusKey = file;
}

// ---- replace a corvette's parts (game thread) ----

template <class F, class... A> bool Call(F fn, A... a)
{
    __try { fn(a...); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void PutParts(uint8_t* dst, const std::vector<CorvettePart>& parts)
{
    for (size_t i = 0; i < parts.size(); ++i) {
        uint8_t* e = dst + i * RB.entry;
        const CorvettePart& p = parts[i];
        float at[4] = { p.at[0], p.at[1], p.at[2], 0 }, pos[4] = { p.p[0], p.p[1], p.p[2], 0 }, up[4] = { p.up[0], p.up[1], p.up[2], 0 };
        memcpy(e + RB.at, at, 16);
        memcpy(e + RB.pos, pos, 16);
        memcpy(e + RB.up, up, 16);
        memcpy(e + RB.id, p.id.data(), std::min<size_t>(p.id.size(), 15));
        memcpy(e + RB.time, &p.time, 8);
        memcpy(e + RB.user, &p.user, 8);
    }
}

// Rebuilds a slot's corvette tech from its parts. The game only reads the component for a fallback base index (+0xB0).
bool RebuildTech(int slot, int base)
{
    if (!RB.reassemble) return false;
    alignas(16) uint8_t component[0x100] = {};
    *(uint16_t*)(component + 0xB0) = base >= 0 ? (uint16_t)base : 0xFFFF;
    bool ok = Call(RB.reassemble, (void*)component, (int32_t)slot);
    if (!ok) log::Warn("Corvettes", "Tech of slot {} was not rebuilt", slot);
    return ok;
}

// The steps TakeOwnership runs to rebuild a corvette base in place, with the saved parts swapped in for the base's
// own. The copy of the old parts the game allocates is left alone (a few KB per replace).
bool ReplaceParts(int slot, const std::vector<CorvettePart>& parts, std::string& why)
{
    CorvetteSeen target;
    for (auto& c : CorvettesLoaded())
        if (c.mine && c.slot == slot) target = c;
    uintptr_t buf = BaseBuffer(target.base);
    if (!buf) { why = L::T("corv.err.notloaded"); return false; }
    // back up what is there now, so a replace can always be undone by loading it back; once is enough
    std::string backup = AlreadySaved(target.parts);
    if (backup.empty()) backup = Save(target, ShownName(target) + " " + L::T("corv.backup"));
    if (backup.empty()) { why = L::T("corv.writefail"); return false; }

    uint16_t idx = (uint16_t)target.base;
    std::vector<uint8_t> tmpMem(RB.size + 16, 0), ours(parts.size() * RB.entry + 16, 0);
    uintptr_t tmp = ((uintptr_t)tmpMem.data() + 15) & ~(uintptr_t)15;
    uint8_t* arr = (uint8_t*)(((uintptr_t)ours.data() + 15) & ~(uintptr_t)15);
    PutParts(arr, parts);
    *(uint16_t*)(tmp + RB.objects + 0xC) = 0x100; // an empty array, as the game's own temp starts

    if (!Call(RB.clear, RB.mgr, (const uint16_t*)&idx) || !Call(RB.ownerDefaults, tmp + RB.owner) || !Call(RB.baseDefaults, tmp)
        || !Call(RB.populate, buf, tmp)) {
        why = L::T("corv.err.game");
        return false;
    }
    // swap our parts in; a set byte at +0xC tells the game this array is not its to free
    uintptr_t oldData = *(uintptr_t*)(tmp + RB.objects);
    int32_t oldCount = *(int32_t*)(tmp + RB.objects + 8);
    uint8_t oldFlag = *(uint8_t*)(tmp + RB.objects + 0xC);
    *(uintptr_t*)(tmp + RB.objects) = (uintptr_t)arr;
    *(int32_t*)(tmp + RB.objects + 8) = (int32_t)parts.size();
    *(uint8_t*)(tmp + RB.objects + 0xC) = 1;
    bool ok = Call(RB.reposition, tmp) && Call(RB.init, buf, tmp, RB.type) && Call(RB.link, buf)
              && Call(RB.addNodes, RB.mgr, (const uint16_t*)&idx, true);
    *(uintptr_t*)(tmp + RB.objects) = oldData;
    *(int32_t*)(tmp + RB.objects + 8) = oldCount;
    *(uint8_t*)(tmp + RB.objects + 0xC) = oldFlag;
    if (!ok) { why = L::T("corv.err.game"); return false; }
    bool tech = RebuildTech(slot, target.base);
    // rebuild the ship from its new base, as summoning it does, so it shows at once
    bool shown = RB.populateShip && RB.ownership > 0 && Call(RB.populateShip, game::Data() + (uintptr_t)RB.ownership, (int32_t)slot);
    if (!shown) log::Warn("Corvettes", "Slot {} rebuilt, but the ship was not refreshed: summon it again to see it", slot);
    log::Info("Corvettes", "Replaced the parts of slot {} (base {}): {} -> {} parts, tech {}, backup {}", slot, target.base,
              target.parts.size(), parts.size(), tech ? "rebuilt" : "unchanged", backup);
    return true;
}

// The game's tech rebuild reads only a buffer's type (+0x23C) and its part slots (+0x34 count, +0x38 data, 0xD0 each,
// the entry at +0x10, an empty Timestamp skips it), so a stand-in buffer holds the library parts.
bool OfferTech(uintptr_t templateBuf, const std::vector<CorvettePart>& parts, uintptr_t item)
{
    if (!NS.techStores || NS.itemTech <= 0 || NS.itemGeneral <= 0 || parts.empty()) return false;
    constexpr uint32_t kCap = 0x30, kCount = 0x34, kData = 0x38, kSlot = 0xD0, kEntry = 0x10, kType = 0x23C;
    std::vector<uint8_t> bufMem(0x400 + 16, 0), slotMem(parts.size() * kSlot + 16, 0), one(RB.entry + 16, 0);
    uint8_t* fake = (uint8_t*)(((uintptr_t)bufMem.data() + 15) & ~(uintptr_t)15);
    uint8_t* slots = (uint8_t*)(((uintptr_t)slotMem.data() + 15) & ~(uintptr_t)15);
    uint8_t* e = (uint8_t*)(((uintptr_t)one.data() + 15) & ~(uintptr_t)15);
    for (size_t i = 0; i < parts.size(); ++i) {
        CorvettePart p = parts[i];
        if (!p.time) p.time = 1;
        PutParts(e, { p });
        memcpy(slots + i * kSlot + kEntry, e, RB.entry);
    }
    int32_t type = 0;
    if (!Rd(templateBuf + kType, type)) return false;
    *(int32_t*)(fake + kType) = type;
    *(uint32_t*)(fake + kCap) = *(uint32_t*)(fake + kCount) = (uint32_t)parts.size();
    *(uintptr_t*)(fake + kData) = (uintptr_t)slots;
    bool ok = Call(NS.techStores, (uintptr_t)fake, item + (uintptr_t)NS.itemTech, item + (uintptr_t)NS.itemGeneral);
    if (!ok) log::Warn("Corvettes", "The offered ship's tech was not rebuilt");
    return ok;
}

// The template corvette gives the ship record (model, class, tech) and the base header; the library entry gives the
// parts and the name. Cargo and general items are left out so nothing is duplicated.
bool NewCorvette(int templateSlot, const std::vector<CorvettePart>& parts, const std::string& name, std::string& why)
{
    CorvetteSeen tpl;
    for (auto& c : CorvettesLoaded())
        if (c.mine && c.slot == templateSlot) tpl = c;
    uintptr_t buf = BaseBuffer(tpl.base), res = ShipResource(templateSlot);
    uintptr_t stores[3] = { ShipStore(templateSlot, 0), ShipStore(templateSlot, 1), ShipStore(templateSlot, 2) };
    if (!buf || !res || !stores[0] || !stores[1] || !stores[2]) { why = L::T("corv.err.notloaded"); return false; }

    std::vector<uint8_t> shipMem(NS.size + NS.look + 64, 0), tmpMem(RB.size + 16, 0), ours(parts.size() * RB.entry + 16, 0);
    uint8_t* ship = (uint8_t*)(((uintptr_t)shipMem.data() + 15) & ~(uintptr_t)15);
    uint8_t* look = ship + ((NS.size + 15) & ~15u);
    uintptr_t tmp = ((uintptr_t)tmpMem.data() + 15) & ~(uintptr_t)15;
    uint8_t* arr = (uint8_t*)(((uintptr_t)ours.data() + 15) & ~(uintptr_t)15);
    PutParts(arr, parts);

    // the ship record: the template's model, then its stores as save containers
    memcpy(ship + NS.res, (const void*)res, 0x48);
    if (!Call(NS.container, stores[0], (void*)(ship + NS.inv)) || !Call(NS.container, stores[1], (void*)(ship + NS.tech))
        || !Call(NS.container, stores[2], (void*)(ship + NS.cargo))) {
        why = L::T("corv.err.game");
        return false;
    }
    *(int32_t*)(ship + NS.inv + NS.slots + 8) = 0;   // no general items
    *(int32_t*)(ship + NS.cargo + NS.slots + 8) = 0; // no cargo
    memcpy(ship + NS.name, name.data(), std::min<size_t>(name.size(), NS.nameLen - 1));

    // the base: the template's header with the library parts swapped in, as replace does
    *(uint16_t*)(tmp + RB.objects + 0xC) = 0x100;
    if (!Call(RB.ownerDefaults, tmp + RB.owner) || !Call(RB.baseDefaults, tmp) || !Call(RB.populate, buf, tmp)) {
        why = L::T("corv.err.game");
        return false;
    }
    uintptr_t oldData = *(uintptr_t*)(tmp + RB.objects);
    int32_t oldCount = *(int32_t*)(tmp + RB.objects + 8);
    uint8_t oldFlag = *(uint8_t*)(tmp + RB.objects + 0xC);
    *(uintptr_t*)(tmp + RB.objects) = (uintptr_t)arr;
    *(int32_t*)(tmp + RB.objects + 8) = (int32_t)parts.size();
    *(uint8_t*)(tmp + RB.objects + 0xC) = 1;
    uintptr_t d = game::Data(), item = d + (uintptr_t)NS.item;
    // free: cost 0 in units; the purchase item copies everything it is given
    bool ok = Call(NS.setup, item, (const void*)ship, (const void*)look, (const void*)tmp, false, (int32_t)0, (int32_t)0, false);
    // the item's tech is the template's; rebuild it from the library parts, so the page and the claimed ship match
    bool tech = ok && OfferTech(buf, parts, item);
    ok = ok && Call(NS.page, d + (uintptr_t)NS.frontend, NS.purchasePage, true);
    *(uintptr_t*)(tmp + RB.objects) = oldData;
    *(int32_t*)(tmp + RB.objects + 8) = oldCount;
    *(uint8_t*)(tmp + RB.objects + 0xC) = oldFlag;
    if (!ok) { why = L::T("corv.err.game"); return false; }
    log::Info("Corvettes", "Offered {} ({} parts) on the ship purchase page, template slot {}, tech {}", name, parts.size(), templateSlot,
              tech ? "rebuilt from its parts" : "the template's");
    return true;
}

// A corvette offered on the purchase page: once claimed, its tech is rebuilt from its parts. The page keeps the
// template's part tech, which does not match the new build. Game thread only.
struct
{
    uint64_t sig = 0;
    size_t count = 0;
    std::vector<int> before; // slots that already had this build
    uint64_t until = 0;      // frame
} g_claim;

void WatchClaim()
{
    if (!g_claim.sig || !RB.reassemble) return;
    if (game::Frame() > g_claim.until) { g_claim.sig = 0; return; }
    for (auto& c : CorvettesLoaded()) {
        if (!c.mine || c.slot < 0 || c.parts.size() != g_claim.count || IdSig(c.parts) != g_claim.sig) continue;
        if (std::find(g_claim.before.begin(), g_claim.before.end(), c.slot) != g_claim.before.end()) continue;
        g_claim.sig = 0;
        bool tech = RebuildTech(c.slot, c.base);
        if (tech && RB.populateShip && RB.ownership > 0) Call(RB.populateShip, game::Data() + (uintptr_t)RB.ownership, (int32_t)c.slot);
        log::Info("Corvettes", "Claimed into slot {} ({} parts), tech {}", c.slot, c.parts.size(), tech ? "rebuilt from its parts" : "not rebuilt");
        return;
    }
}

void New(int templateSlot, std::string key)
{
    std::vector<CorvettePart> parts;
    std::string name;
    {
        std::lock_guard lk(g_mx);
        for (auto& s : g_list)
            if (s.file == key) { parts = s.parts; name = s.name; }
    }
    if (parts.empty()) return;
    std::string why = L::T("corv.err.missing");
    bool ok = NS.Ready() && RB.Ready() && NewCorvette(templateSlot, parts, name, why);
    if (ok) {
        g_claim.sig = IdSig(parts);
        g_claim.count = parts.size();
        g_claim.before.clear();
        for (auto& c : CorvettesLoaded())
            if (c.mine && c.slot >= 0 && c.parts.size() == parts.size() && IdSig(c.parts) == g_claim.sig) g_claim.before.push_back(c.slot);
        g_claim.until = game::Frame() + 60ull * 60 * 30; // about half an hour
    }
    else {
        SetStatus(L::Tf("corv.newfail", name, why));
        ui::Toast(L::T("corv.newfail.title"), why, icon::Warning);
    }
}

void Replace(int slot, std::string key)
{
    std::vector<CorvettePart> parts;
    std::string name;
    {
        std::lock_guard lk(g_mx);
        for (auto& s : g_list)
            if (s.file == key) { parts = s.parts; name = s.name; }
    }
    if (parts.empty()) return;
    std::string why = L::T("corv.err.missing"), ship = ShipName(slot);
    bool ok = RB.Ready() && ReplaceParts(slot, parts, why);
    SetStatus(ok ? L::Tf("corv.replaced", ship, name) : L::Tf("corv.replacefail", ship, why));
    ui::Toast(L::T(ok ? "corv.replaced.title" : "corv.replacefail.title"), ok ? name : why, ok ? icon::Ship : icon::Warning);
}

void Tick()
{
    if (game::Frame() % 30) return;
    WatchClaim();
    Head h;
    h.live = game::PlayerState() != 0;
    if (h.live) {
        auto all = CorvettesLoaded();
        h.loaded = all.size();
        for (auto& c : all)
            if (c.mine && c.slot >= 0) {
                ++h.corvettes;
                std::string n = ShipName(c.slot);
                h.yours.push_back({ c.slot, n.empty() ? c.name : n });
            }
        if (!all.empty()) {
            auto& c = all[0];
            h.hasNear = true;
            h.mine = c.mine;
            h.nearName = c.mine && c.slot >= 0 ? ShipName(c.slot) : std::string();
            if (h.nearName.empty()) h.nearName = c.name;
            if (!c.mine && !c.owner.empty()) h.nearName += " (" + c.owner + ")";
            h.nearDist = c.dist;
            h.nearParts = c.parts.size();
        }
        static size_t lastMine = SIZE_MAX, lastOthers = SIZE_MAX;
        size_t others = h.loaded - h.yours.size();
        if (h.yours.size() != lastMine || others != lastOthers) {
            lastMine = h.yours.size();
            lastOthers = others;
            log::Info("Corvettes", "Loaded corvettes: {} yours, {} other players'{}", lastMine, others,
                      h.hasNear ? " (nearest " + h.nearName + ", " + std::to_string((int)h.nearDist) + " m)" : std::string());
        }
        h.slotsUsed = OwnedShipSlots(h.slotsTotal);
        if (g_shipGlobals) Rd(g_shipGlobals + g_limitOff, h.limit);
    }
    std::lock_guard lk(g_mx);
    g_head = std::move(h);
}

// ---- UI ----

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> entries;
    uint64_t seen = 0;
    char name[64] = {};
    int target = 0; // which of your corvettes the inspector would replace
    int tpl = 0;    // which of your corvettes a new slot copies its ship record from
} g;

// L::Tf takes two values; these lines need more.
std::string Fmt(const char* key, std::initializer_list<std::string> vals)
{
    std::string s = L::T(key);
    int i = 0;
    for (auto& v : vals) {
        std::string tag = "{" + std::to_string(i++) + "}";
        if (size_t at = s.find(tag); at != std::string::npos) s.replace(at, tag.size(), v);
    }
    return s;
}

std::string Metres(float v)
{
    char b[24];
    snprintf(b, sizeof b, "%.1f", std::max(v, 0.1f));
    return b;
}

int Count(const Ship& s, const char* prefix)
{
    size_t n = strlen(prefix);
    return (int)std::count_if(s.parts.begin(), s.parts.end(), [&](const CorvettePart& p) { return p.id.compare(0, n, prefix) == 0; });
}

int Stacks(const Ship& s, const char* key) { return s.inv.contains(key) && s.inv[key].is_array() ? (int)s.inv[key].size() : -1; }

void Rebuild()
{
    g.entries.clear();
    std::lock_guard lk(g_mx);
    if (!g_loaded) Load();
    for (auto& s : g_list) {
        ui::BrowserEntry e;
        e.key = s.file;
        e.title = s.name;
        std::string who = s.owner.empty() ? s.from : s.owner;
        e.subtitle = L::Tf("corv.partsn", std::to_string(s.parts.size())) + (who.empty() ? "" : "   /   " + who);
        e.glyph = icon::Ship;
        e.count = std::to_string(s.parts.size());
        e.value = (double)s.parts.size();
        e.group = who;
        g.entries.push_back(std::move(e));
    }
    g.seen = g_version;
    g.browser.Invalidate();
}

void Note(ImDrawList* dl, float x, float& y, float w, const std::string& s, ImU32 col, float size = 15)
{
    if (s.empty()) return;
    dl->AddText(ui::theme::Body(), S(size), ImVec2(x, y), col, s.c_str(), nullptr, w);
    y += ui::theme::Body()->CalcTextSizeA(S(size), FLT_MAX, w, s.c_str()).y + S(8);
}

void DrawList(const char* query)
{
    Head h;
    std::string status;
    {
        std::lock_guard lk(g_mx);
        if (!g_loaded) Load();
        h = g_head;
        status = g_status;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(8), x = p.x, y = p.y;
    bool live = h.live && game::FrameHookReady();

    std::string where = !live ? std::string(L::T("corv.ingame"))
                       : h.hasNear ? Fmt(h.mine ? "corv.near.mine" : "corv.near", { h.nearName, Metres(h.nearDist), std::to_string(h.nearParts) })
                                : std::string(L::T("corv.none"));
    Note(dl, x, y, w, where, h.hasNear && live ? P().text : P().textDim, 16);
    if (live) {
        std::string facts = Fmt("corv.slots", { std::to_string(h.slotsUsed), std::to_string(h.slotsTotal), std::to_string(h.corvettes) });
        if (h.limit > 0) facts += "   /   " + L::Tf("corv.limit", std::to_string(h.limit));
        Note(dl, x, y, w, facts, P().textDim);
    }
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::TextField("##corvname", g.name, sizeof g.name, w, L::T("corv.name.hint"));
    y += S(40);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##corvcopy", L::T("corv.copy"), w, ui::Tone::Green, live && h.hasNear)) {
        std::string name = g.name;
        g.name[0] = 0;
        game::Post([=] { CopyNearest(name); });
    }
    y += S(46);
    Note(dl, x, y, w, status, P().textBody);
    // the same row as Crafting > Corvette and Base Limits: one setting, two places
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::DrawFeatureRow("build.corvette");
    y = ImGui::GetCursorScreenPos().y + S(6);
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), P().line, S(1));
    y += S(12);
    ImGui::SetCursorScreenPos(ImVec2(x, y));

    if (g.seen != g_version) Rebuild();
    if (g.entries.empty()) {
        Note(dl, x, y, w, L::T("corv.empty"), P().textDim);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::Dummy(ImVec2(w, S(10)));
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

float DrawInspector(ImVec2 a, float w)
{
    const ui::BrowserEntry* e = g.entries.empty() ? nullptr : g.browser.Focused(g.entries);
    if (!e) return 0; // default tab info
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ui::DrawEntryHeader(dl, a, w, *e);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    std::string key = e->key;
    Ship s;
    {
        std::lock_guard lk(g_mx);
        g_focusKey = key;
        for (auto& b : g_list)
            if (b.file == key) { s = b; break; }
    }
    char made[32] = {};
    time_t tt = (time_t)s.made;
    tm lt{};
    if (s.made && !localtime_s(&lt, &tt)) strftime(made, sizeof made, "%Y-%m-%d %H:%M", &lt);
    static const char* kCls[] = { "C", "B", "A", "S" };

    Fact(dl, x, y, bw, L::T("corv.f.parts"), std::to_string(s.parts.size()));
    Fact(dl, x, y, bw, L::T("corv.f.size"), Metres(s.hi[0] - s.lo[0]) + " x " + Metres(s.hi[1] - s.lo[1]) + " x " + Metres(s.hi[2] - s.lo[2]));
    Fact(dl, x, y, bw, L::T("corv.f.from"), s.from);
    Fact(dl, x, y, bw, L::T("corv.f.owner"), s.owner);
    Fact(dl, x, y, bw, L::T("corv.f.made"), made);
    if (s.cls >= 0 && s.cls <= 3) Fact(dl, x, y, bw, L::T("corv.f.class"), kCls[s.cls]);
    int tech = Stacks(s, "tech"), cargo = Stacks(s, "cargo"), general = Stacks(s, "general");
    if (tech >= 0) Fact(dl, x, y, bw, L::T("corv.f.tech"), std::to_string(tech));
    if (general >= 0 || cargo >= 0) Fact(dl, x, y, bw, L::T("corv.f.cargo"), std::to_string(std::max(general, 0) + std::max(cargo, 0)));
    if (!s.ownership.is_null()) Fact(dl, x, y, bw, L::T("corv.f.ownership"), L::T("corv.f.ownership.yes"));
    Fact(dl, x, y, bw, L::T("corv.f.file"), key);
    y += S(6);

    // key parts, by the game's ID families
    std::string keys = Fmt("corv.keyparts", { std::to_string(Count(s, "B_COK_")), std::to_string(Count(s, "B_ALK_")),
                                              std::to_string(Count(s, "B_GEN_")), std::to_string(Count(s, "B_TRU_")),
                                              std::to_string(Count(s, "B_SHL_")), std::to_string(Count(s, "B_TUR_")) });
    Note(dl, x, y, bw, keys, P().textBody);
    if (!Count(s, "U_PARAGON")) Note(dl, x, y, bw, L::T("corv.warn.core"), P().gold, 14);
    if (!Count(s, "B_COK_")) Note(dl, x, y, bw, L::T("corv.warn.cockpit"), P().gold, 14);
    if (tech < 0) Note(dl, x, y, bw, L::T("corv.note.partsonly"), P().textDim, 14);
    y += S(4);

    // put it on one of your corvettes: replace only, with a warning and a backup first
    Head h;
    {
        std::lock_guard lk(g_mx);
        h = g_head;
    }
    bool live = h.live && game::FrameHookReady();
    dl->AddText(ui::theme::Title(), S(17), ImVec2(x, y), P().text, L::T("corv.use"));
    y += S(28);
    if (!live) Note(dl, x, y, bw, L::T("corv.ingame"), P().textDim);
    else if (h.yours.empty()) Note(dl, x, y, bw, L::T("corv.use.none"), P().textDim);
    else {
        std::vector<std::string> names;
        for (auto& [slot, n] : h.yours) names.push_back(n.empty() ? L::Tf("corv.slotn", std::to_string(slot + 1)) : n);
        if (g.target >= (int)names.size()) g.target = 0;
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::Dropdown("##corvtarget", names, &g.target, bw);
        y += S(44);
        Note(dl, x, y, bw, L::Tf("corv.warn.replace", names[g.target]), P().gold, 14);
        Note(dl, x, y, bw, L::T("corv.note.workshop"), P().textDim, 14);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::HoldButton("##corvreplace", L::T("corv.replace"), bw) && !s.parts.empty()) {
            int slot = h.yours[g.target].first;
            game::Post([slot, key] { Replace(slot, key); });
        }
        y += S(50);
    }
    y += S(6);
    dl->AddText(ui::theme::Title(), S(17), ImVec2(x, y), P().text, L::T("corv.new"));
    y += S(28);
    if (live && !h.yours.empty()) {
        std::vector<std::string> names;
        for (auto& [slot, n] : h.yours) names.push_back(n.empty() ? L::Tf("corv.slotn", std::to_string(slot + 1)) : n);
        if (g.tpl >= (int)names.size()) g.tpl = 0;
        Note(dl, x, y, bw, L::T("corv.new.template"), P().textDim, 14);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::Dropdown("##corvtpl", names, &g.tpl, bw);
        y += S(44);
        Note(dl, x, y, bw, L::Tf("corv.new.note", names[g.tpl]), P().textBody, 14);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##corvnew", L::T("corv.new.go"), bw, ui::Tone::Green, NS.Ready() && !s.parts.empty())) {
            int slot = h.yours[g.tpl].first;
            ui::SetOpen(false); // the game's purchase page opens behind the menu otherwise
            game::Post([slot, key] { New(slot, key); });
        }
        y += S(46);
    }
    else if (live) Note(dl, x, y, bw, L::T("corv.new.needone"), P().textDim, 14);

    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##corvfolder", L::T("corv.folder"), bw, ui::Tone::Neutral, true)) {
        std::error_code ec;
        std::filesystem::create_directories(Folder(), ec);
        ShellExecuteW(nullptr, L"open", Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    y += S(46);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##corvreload", L::T("corv.reload"), bw, ui::Tone::Teal, true)) {
        std::lock_guard lk(g_mx);
        Load();
    }
    y += S(46);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::HoldButton("##corvdelete", L::T("corv.delete"), bw)) {
        std::lock_guard lk(g_mx);
        std::error_code ec;
        std::filesystem::remove(Folder() / paths::Wide(key), ec);
        std::erase_if(g_list, [&](const Ship& b) { return b.file == key; });
        g_focusKey.clear();
        g.browser.ClearFocus();
        ++g_version;
    }
    y += S(50);
    return y;
}

}

void InstallCorvettes()
{
    g_shipGlobals = Loc().GlobalsInstance("cGcBuildableShipGlobals");
    if (auto m = meta::Path("cGcBuildableShipGlobals", "ComplexityLimitWarning")) g_limitOff = m->offset;
    log::Info("Corvettes", "Library {}, complexity limit {}", paths::Utf8(Folder().wstring()), g_shipGlobals ? "readable" : "unknown");

    // The rebuild steps, in TakeOwnership's order after the clear: owner defaults, base defaults, populate,
    // reposition, init (r8d = base type), link grid, add nodes.
    if (const Target& t = Loc().Resolve("Corvette.RebuildBaseSite"); t.ok) {
        auto in = Dec(t.site);
        if (in && in->ripTarget) RB.mgr = Abs(*in->ripTarget);
        std::vector<uint32_t> calls;
        int32_t type = -1;
        for (int i = 0; in && i < 160 && calls.size() < 8; ++i, in = Next(*in)) {
            if (in->mnemonic == "mov" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm && calls.size() == 5)
                type = (int32_t)in->ops[1].imm; // the last immediate before the init call
            if (in->isCall && in->branchTarget) calls.push_back(*in->branchTarget);
        }
        if (calls.size() == 8 && type > 0) {
            RB.clear = (ClearFn)Abs(calls[0]);
            RB.ownerDefaults = (ObjFn)Abs(calls[1]);
            RB.baseDefaults = (ObjFn)Abs(calls[2]);
            RB.populate = (PopulateFn)Abs(calls[3]);
            RB.reposition = (ObjFn)Abs(calls[4]);
            RB.init = (InitFn)Abs(calls[5]);
            RB.link = (ObjFn)Abs(calls[6]);
            RB.addNodes = (AddNodesFn)Abs(calls[7]);
            RB.type = type;
        }
        else log::Warn("Corvettes", "Rebuild site found, but {} calls (type {}) instead of 8", calls.size(), type);
    }
    if (const Target& t = Loc().Resolve("Corvette.PopulateShipFromBase"); t.ok) RB.populateShip = (PopulateShipFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Corvette.ReassembleTech"); t.ok) RB.reassemble = (ReassembleFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Ship.OwnershipOffset"); t.ok && t.offset) RB.ownership = *t.offset;
    auto off = [](const char* cls, const char* f, uint32_t& out) { if (auto m = meta::Path(cls, f)) out = m->offset; };
    // new slot: the transfer path's calls, in RedeemShip's order: can redeem, cost, setup ship, open page
    if (const Target& t = Loc().Resolve("Corvette.RedeemShipSite"); t.ok) {
        std::vector<uint32_t> calls;
        std::vector<int64_t> adds; // add rcx, imm before the setup and page calls
        int32_t page = -1;
        for (auto in = Dec(t.func); in && calls.size() < 4; in = Next(*in)) {
            if (in->mnemonic == "add" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm && in->ops[1].imm > 0x10000) adds.push_back(in->ops[1].imm);
            if (in->mnemonic == "mov" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm && calls.size() == 3) page = (int32_t)in->ops[1].imm;
            if (in->isCall && in->branchTarget) calls.push_back(*in->branchTarget);
            if (in->mnemonic == "ret") break;
        }
        if (calls.size() == 4 && adds.size() == 2 && page > 0) {
            NS.setup = (SetupShipFn)Abs(calls[2]);
            NS.page = (PageFn)Abs(calls[3]);
            NS.item = adds[0];
            NS.frontend = adds[1];
            NS.purchasePage = page;
        }
        else log::Warn("Corvettes", "Ship transfer site found, but {} calls, {} offsets, page {}", calls.size(), adds.size(), page);
    }
    if (const Target& t = Loc().Resolve("Inventory.PopulateContainer"); t.ok) NS.container = (ContainerFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Corvette.ReassembleTechStores"); t.ok) NS.techStores = (TechStoresFn)Abs(t.func);
    if (const Target& t = Loc().Resolve("Purchase.TechStore"); t.ok && t.offset) NS.itemTech = *t.offset;
    if (const Target& t = Loc().Resolve("Purchase.GeneralStore"); t.ok && t.offset) NS.itemGeneral = *t.offset;
    if (auto* ci = meta::Get("cGcPlayerOwnershipData"); ci && ci->size >= 0x200 && ci->size <= 0x2000) NS.size = ci->size;
    if (auto* ci = meta::Get("cGcCharacterCustomisationSaveData"); ci && ci->size >= 0x10 && ci->size <= 0x1000) NS.look = ci->size;
    off("cGcPlayerOwnershipData", "Inventory", NS.inv);
    off("cGcPlayerOwnershipData", "Inventory_Cargo", NS.cargo);
    off("cGcPlayerOwnershipData", "Inventory_TechOnly", NS.tech);
    off("cGcPlayerOwnershipData", "Resource", NS.res);
    off("cGcPlayerOwnershipData", "Name", NS.name);
    if (auto m = meta::Path("cGcPlayerOwnershipData", "Name"); m && m->size >= 8 && m->size <= 0x200) NS.nameLen = m->size;
    off("cGcInventoryContainer", "Slots", NS.slots);
    log::Info("Corvettes", "New slot {} (item +0x{:X}, page 0x{:X}, ship record 0x{:X} bytes), offered tech rebuild {}", NS.Ready() ? "ready" : "missing",
              NS.item, NS.purchasePage, NS.size, NS.techStores && NS.itemTech > 0 && NS.itemGeneral > 0 ? "ready" : "missing");
    if (auto* ci = meta::Get("cGcPersistentBase"); ci && ci->size >= 0x100 && ci->size <= 0x1000) RB.size = ci->size;
    if (auto* ci = meta::Get("cGcPersistentBaseEntry"); ci && ci->size >= 0x60 && ci->size <= 0x200) RB.entry = ci->size;
    off("cGcPersistentBase", "Objects", RB.objects);
    off("cGcPersistentBase", "Owner", RB.owner);
    off("cGcPersistentBaseEntry", "At", RB.at);
    off("cGcPersistentBaseEntry", "Position", RB.pos);
    off("cGcPersistentBaseEntry", "Up", RB.up);
    off("cGcPersistentBaseEntry", "ObjectID", RB.id);
    off("cGcPersistentBaseEntry", "Timestamp", RB.time);
    off("cGcPersistentBaseEntry", "UserData", RB.user);
    log::Info("Corvettes", "Replace parts {} (base type {}, base 0x{:X} bytes, objects +0x{:X}, entry 0x{:X}), ship refresh {}, tech rebuild {}",
              RB.Ready() ? "ready" : "missing", RB.type, RB.size, RB.objects, RB.entry, RB.populateShip && RB.ownership > 0 ? "ready" : "missing",
              RB.reassemble ? "ready" : "missing");
    game::EveryFrame(Tick);

    Bound b;
    b.id = "corv.copy";
    b.action = [] { if (game::PlayerState()) game::Post([] { CopyNearest(""); }); };
    Ok(std::move(b));

    Registry::Get().Post([] {
        g.browser.chips = { L::T("corv.chip.all") };
        g.browser.searchBox = true;
        g.browser.sortable = true;
        g.browser.sortOptions = { { ui::SortBy::Default, L::T("corv.sort.newest") }, { ui::SortBy::Name, L::T("corv.sort.name") },
                                  { ui::SortBy::Value, L::T("corv.sort.parts") }, { ui::SortBy::Type, L::T("corv.sort.from") } };
        g.browser.captions = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] {
            std::lock_guard lk(g_mx);
            return g_loaded ? "(" + std::to_string(g_list.size()) + ")" : std::string();
        };
        ui::RegisterPage("corvettes", std::move(page));
    });
}

}
