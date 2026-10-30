#include "features/WorldFx.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <shellapi.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

// Bases tab. In-game: the featured bases the game downloads, plus other players' bases
// seen in systems you visit (kept per save slot). Portal Repository: search portalrepository.com's public
// address catalogue (main build only). Everything warps through the Teleport tab's warp.
namespace os::features {

namespace {

using json = nlohmann::json;
using ui::theme::P;
using ui::theme::S;
namespace meta = hooks::meta;

// cGcBaseBuildingDownloadsManager featured bases: query result +0xE8 (5 = done),
// list count +0xFC and data +0x100, 0x150 per cGcBaseBuildingDownloadsManager::BaseData.
uintptr_t g_dl = 0;
struct { uint32_t state = 0xE8, count = 0xFC, data = 0x100, stride = 0x150, name = 0x40, user = 0xC0, platform = 0x100; } F;
constexpr int32_t kDone = 5;
// cGcUniverseAddressData / cGcGalacticAddressData, from the reflection data
struct { uint32_t galaxy = 0, ga = 4, x = 0, y = 4, z = 8, system = 0xC, planet = 0x10; } U;

enum Source { Featured = 1, Seen = 2, Repo = 3, Mine = 4 };

struct Entry
{
    std::string key, name, owner, platform, tags, link, system, planet, type;
    uint64_t ua = 0;
    int source = 0;
    int64_t when = 0;
    bool hasPos = false;
    double pos[3] = {}; // in its system
};

std::mutex g_mx; // everything below except the UI struct
std::vector<Entry> g_featured, g_seen, g_repo, g_mine;
struct { bool ok = false; uint64_t ua = 0; double pos[3] = {}; } g_me; // where you are, for distances
uint64_t g_version = 1;
std::wstring g_slotKey;     // "<account>_slot<N>" from the save file the game opened last
std::wstring g_seenKey;     // the key g_seen was loaded for
bool g_featuredLoaded = false;

std::filesystem::path Folder() { return std::filesystem::path(paths::Root()) / L"OpenSkies.bases"; }

bool MainBuild() { return std::string(L::T("ui.credit.logo")) == "1"; }

// ---- which save is loaded: the last save*.hg the game opened ----

safetyhook::InlineHook g_cfw;

void NoteFile(const wchar_t* path)
{
    if (!path) return;
    size_t n = wcslen(path);
    if (n < 8 || _wcsicmp(path + n - 3, L".hg") != 0) return;
    const wchar_t* base = path + n;
    while (base > path && base[-1] != L'\\' && base[-1] != L'/') --base;
    if (_wcsnicmp(base, L"save", 4) != 0) return;
    int idx = 1;
    const wchar_t* d = base + 4;
    if (*d != L'.') {
        idx = 0;
        for (; *d >= L'0' && *d <= L'9'; ++d) idx = idx * 10 + (*d - L'0');
        if (*d != L'.' || idx < 1) return;
    }
    std::wstring dir(path, base > path ? base - 1 : base);
    size_t cut = dir.find_last_of(L"\\/");
    std::wstring account = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
    std::wstring key = account + L"_slot" + std::to_wstring((idx + 1) / 2);
    std::lock_guard lk(g_mx);
    if (key == g_slotKey) return;
    g_slotKey = key;
    log::Debug("Bases", "Save in use: {} ({})", paths::Utf8(key), paths::Utf8(base));
}

HANDLE WINAPI CreateFileWHk(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    NoteFile(name);
    return g_cfw.stdcall<HANDLE>(name, access, share, sa, disp, flags, tmpl);
}

// ---- files ----

json ToJ(const Entry& e)
{
    json j = { { "name", e.name }, { "owner", e.owner }, { "platform", e.platform }, { "ua", e.ua }, { "when", e.when },
               { "system", e.system }, { "planet", e.planet } };
    if (e.hasPos) j["pos"] = json::array({ e.pos[0], e.pos[1], e.pos[2] });
    return j;
}

Entry FromJ(const json& j, int source)
{
    Entry e;
    e.name = j.value("name", "");
    e.owner = j.value("owner", "");
    e.platform = j.value("platform", "");
    e.ua = j.value("ua", (uint64_t)0);
    e.when = j.value("when", (int64_t)0);
    e.system = j.value("system", "");
    e.planet = j.value("planet", "");
    if (auto it = j.find("pos"); it != j.end() && it->is_array() && it->size() == 3) {
        for (int k = 0; k < 3; ++k) e.pos[k] = (*it)[k].get<double>();
        e.hasPos = true;
    }
    e.source = source;
    return e;
}

std::vector<Entry> ReadList(const std::filesystem::path& f, int source)
{
    std::vector<Entry> out;
    std::ifstream in(f);
    if (!in) return out;
    try {
        json all = json::parse(in);
        bool old = all.value("version", 1) < 2; // 0.9.6 test builds stored owner and platform the wrong way round
        for (auto& j : all.value("bases", json::array())) {
            Entry e = FromJ(j, source);
            if (old) std::swap(e.owner, e.platform);
            if (e.ua) out.push_back(std::move(e));
        }
    }
    catch (...) { log::Warn("Bases", "Could not read {}", paths::Utf8(f.filename().wstring())); }
    return out;
}

void WriteList(const std::filesystem::path& f, const std::vector<Entry>& list)
{
    json a = json::array();
    for (auto& e : list) a.push_back(ToJ(e));
    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    std::ofstream(f) << json{ { "version", 2 }, { "bases", std::move(a) } }.dump(1);
}

std::filesystem::path SeenFile(const std::wstring& key) { return Folder() / (L"seen_" + (key.empty() ? std::wstring(L"unknown") : key) + L".json"); }
std::filesystem::path FeaturedFile() { return Folder() / L"featured.json"; }

// Seen bases go by owner and name, so a later sighting can correct the address.
std::string KeyOf(const Entry& e)
{
    if (e.source == Seen) return std::to_string(e.source) + "|" + e.owner + "|" + e.name;
    char b[24];
    snprintf(b, sizeof b, "%013llX", (unsigned long long)(e.ua & 0x000FFFFFFFFFFFFFull));
    return std::to_string(e.source) + "|" + b + "|" + e.owner + "|" + e.name;
}

// ---- game data (game thread) ----

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

bool ReadUA(uintptr_t a, uint64_t& ua)
{
    int32_t g = 0, x = 0, y = 0, z = 0, s = 0, p = 0;
    uintptr_t ga = a + U.ga;
    if (!Rd(a + U.galaxy, g) || !Rd(ga + U.x, x) || !Rd(ga + U.y, y) || !Rd(ga + U.z, z) || !Rd(ga + U.system, s) || !Rd(ga + U.planet, p))
        return false;
    if (g < 0 || g > 255 || s < 0 || s > 0xFFF || p < 0 || p > 15) return false;
    ua = (uint64_t)(x & 0xFFF) | (uint64_t)(z & 0xFFF) << 12 | (uint64_t)(y & 0xFF) << 24 | (uint64_t)(g & 0xFF) << 32 |
         (uint64_t)(s & 0xFFF) << 40 | (uint64_t)(p & 0xF) << 52;
    return true;
}

// The game asks for featured bases only from a menu page this version no longer shows. The tab runs the game's
// own request block (Bases.FeaturedQuery) through a thunk that builds that block's parent frame.
using QueryFn = void (*)(uintptr_t mgr);
QueryFn g_query = nullptr;
int g_queryTries = 0;
uint64_t g_queryFrame = 0;
constexpr int kQueryTries = 5;

bool MakeQueryThunk(uintptr_t parent, uintptr_t block)
{
    // the parent starts push rbx; sub rsp,0x60; mov rax,[rip+x]; mov rbx,rcx
    static const uint8_t want[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60 };
    uint8_t have[sizeof want] = {};
    for (size_t i = 0; i < sizeof want; ++i) Rd(parent + i, have[i]);
    if (memcmp(have, want, sizeof want) != 0) return false;
    uint8_t code[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    memcpy(code + 15, &block, 8);
    void* m = VirtualAlloc(nullptr, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) return false;
    memcpy(m, code, sizeof code);
    DWORD old = 0;
    VirtualProtect(m, sizeof code, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), m, sizeof code);
    g_query = (QueryFn)m;
    return true;
}

bool CallQuery()
{
    __try { g_query(g_dl); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Game thread. Asks when nothing is loaded or waiting, a few times at most, 30 s apart.
void AskFeatured()
{
    int32_t state = 0;
    if (!g_query || !g_dl || g_queryTries >= kQueryTries || !Rd(g_dl + F.state, state) || state == kDone || state == 3) return;
    if (g_queryFrame && game::Frame() - g_queryFrame < 30 * 60) return;
    g_queryFrame = game::Frame();
    ++g_queryTries;
    bool ok = CallQuery();
    Rd(g_dl + F.state, state);
    log::Info("Bases", "Asked for featured bases (try {}): {}, state {}", g_queryTries, ok ? "sent" : "failed", state);
    if (!ok) g_queryTries = kQueryTries; // never again this session
}

void ReadFeatured()
{
    int32_t state = 0;
    uint32_t n = 0;
    uintptr_t data = 0;
    if (!g_dl || !Rd(g_dl + F.state, state) || state != kDone || !Rd(g_dl + F.count, n) || !Rd(g_dl + F.data, data) || !data || !n || n > 1000)
        return;
    std::vector<Entry> list;
    for (uint32_t i = 0; i < n; ++i) {
        uintptr_t b = data + (uintptr_t)i * F.stride;
        Entry e;
        e.source = Featured;
        if (!ReadUA(b, e.ua) || !e.ua) continue;
        e.name = Text(b + F.name, 64);
        e.owner = Text(b + F.user, 64);
        e.platform = Text(b + F.platform, 64);
        list.push_back(std::move(e));
    }
    if (list.empty()) return;
    std::lock_guard lk(g_mx);
    bool same = list.size() == g_featured.size();
    for (size_t i = 0; same && i < list.size(); ++i) same = KeyOf(list[i]) == KeyOf(g_featured[i]);
    if (same) return;
    int64_t now = (int64_t)std::time(nullptr);
    for (auto& e : list) e.when = now;
    g_featured = std::move(list);
    g_featuredLoaded = true;
    WriteList(FeaturedFile(), g_featured);
    ++g_version;
    log::Info("Bases", "{} featured bases", g_featured.size());
}

// System and planet names, known for the system you are in.
std::string g_sysName;
std::vector<std::string> g_planetNames;
uint64_t g_namesUa = 0;

void Name(Entry& e, uint64_t here)
{
    if ((e.ua & 0x000FFFFFFFFFFFFFull & ~(0xFull << 52)) != (here & 0x000FFFFFFFFFFFFFull & ~(0xFull << 52))) return;
    if (g_namesUa != here) {
        uint64_t ua = 0;
        g_sysName.clear();
        g_planetNames.clear();
        if (CurrentSystemNames(ua, g_sysName, g_planetNames)) g_namesUa = here;
    }
    e.system = g_sysName;
    size_t pi = (size_t)((e.ua >> 52) & 0xF);
    if (pi < g_planetNames.size()) e.planet = g_planetNames[pi];
}

void ReadMine(uint64_t here)
{
    std::vector<Entry> list;
    for (auto& b : MyBases()) {
        Entry e;
        e.source = Mine;
        e.name = b.name;
        e.type = b.type;
        e.ua = b.ua;
        e.hasPos = true;
        for (int k = 0; k < 3; ++k) e.pos[k] = b.pos[k];
        Name(e, here);
        list.push_back(std::move(e));
    }
    std::lock_guard lk(g_mx);
    // names learned on an earlier visit stay
    for (auto& e : list)
        for (auto& o : g_mine)
            if (KeyOf(o) == KeyOf(e)) {
                if (e.system.empty()) e.system = o.system;
                if (e.planet.empty()) e.planet = o.planet;
            }
    bool same = list.size() == g_mine.size();
    for (size_t i = 0; same && i < list.size(); ++i) same = KeyOf(list[i]) == KeyOf(g_mine[i]) && list[i].system == g_mine[i].system;
    g_mine = std::move(list);
    if (!same) ++g_version;
}

void ReadSeen()
{
    uint64_t here = 0;
    if (!game::PlayerState() || !CurrentAddress(here)) return;
    auto bases = OtherBasesHere();
    std::lock_guard lk(g_mx);
    if (g_seenKey != g_slotKey || g_seenKey.empty() && g_seen.empty()) { // another save, or the first look
        g_seen = ReadList(SeenFile(g_slotKey), Seen);
        g_seenKey = g_slotKey;
        ++g_version;
    }
    if (bases.empty()) return;
    int64_t now = (int64_t)std::time(nullptr);
    int added = 0;
    bool changed = false;
    for (auto& b : bases) {
        Entry e;
        e.source = Seen;
        e.name = b.name;
        e.owner = b.owner;
        e.platform = b.platform;
        e.ua = b.ua ? b.ua : here; // each base carries its own full address
        e.when = now;
        e.hasPos = true;
        for (int k = 0; k < 3; ++k) e.pos[k] = b.pos[k];
        Name(e, here);
        std::string k = KeyOf(e);
        auto it = std::find_if(g_seen.begin(), g_seen.end(), [&](const Entry& o) { return KeyOf(o) == k; });
        if (it != g_seen.end()) {
            // a later sighting corrects the address and fills in names; only real changes are saved
            changed |= it->ua != e.ua || it->platform != e.platform || (!e.system.empty() && it->system != e.system) || !it->hasPos;
            if (!e.system.empty() || it->ua != e.ua) { it->system = e.system; it->planet = e.planet; }
            it->ua = e.ua;
            it->when = now;
            it->platform = e.platform;
            it->hasPos = true;
            for (int c = 0; c < 3; ++c) it->pos[c] = e.pos[c];
            continue;
        }
        g_seen.push_back(std::move(e));
        ++added;
    }
    if (added || changed) {
        WriteList(SeenFile(g_seenKey), g_seen);
        ++g_version;
    }
    if (added) log::Info("Bases", "{} new bases seen here, {} kept for this save", added, g_seen.size());
}

void Tick()
{
    if (game::Frame() % 30) return;
    {
        uint64_t ua = 0;
        double pos[3] = {};
        bool ok = game::PlayerState() && PlayerSpot(ua, pos);
        std::lock_guard lk(g_mx);
        g_me.ok = ok;
        g_me.ua = ua;
        for (int k = 0; k < 3; ++k) g_me.pos[k] = pos[k];
    }
    if (game::Frame() % 120) return;
    if (game::PlayerState()) AskFeatured();
    ReadFeatured();
    ReadSeen();
    uint64_t here = 0;
    if (game::PlayerState() && CurrentAddress(here) && here) ReadMine(here);
}

// ---- Portal Repository (worker thread) ----

std::string HttpGet(const std::wstring& path, int& status)
{
    status = 0;
    std::string body;
    HINTERNET s = WinHttpOpen(L"OpenSkies", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 20000);
    HINTERNET c = WinHttpConnect(s, L"portalrepository.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
        status = (int)code;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 8u << 20) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
            body.append(chunk.data(), got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return body;
}

std::wstring UrlPart(const std::string& s)
{
    std::wstring o;
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') o += (wchar_t)ch;
        else {
            wchar_t b[4];
            swprintf(b, 4, L"%%%02X", ch);
            o += b;
        }
    }
    return o;
}

// WordPress gives titles with HTML entities: &#8217; &amp; and the like.
std::string Unescape(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 8) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        unsigned cp = 0;
        if (!ent.empty() && ent[0] == '#') cp = (unsigned)strtoul(ent.c_str() + 1 + (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')),
                                                                  nullptr, ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? 16 : 10);
        else if (ent == "amp") cp = '&';
        else if (ent == "quot") cp = '"';
        else if (ent == "lt") cp = '<';
        else if (ent == "gt") cp = '>';
        else if (ent == "nbsp") cp = ' ';
        if (!cp) { o += s[i]; continue; }
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 0x3F)); }
        else { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
        i = e;
    }
    return o;
}

struct Cat { int galaxy = -1; std::string galaxyName, platform; };
std::map<int, Cat> g_cats;          // category id -> galaxy and platform
std::map<int, std::string> g_tags;  // tag id -> name
std::vector<std::pair<int, std::string>> g_galaxies; // galaxy index, name, for the picker
constexpr int kBaseTag = 252;       // "Base"
std::atomic<bool> g_fetching{ false };
std::string g_repoStatus;
int g_repoPage = 0, g_repoPages = 0;

bool LoadNames() // worker thread, under nothing; fills g_cats / g_tags
{
    std::filesystem::path f = Folder() / L"portalrepository.names.json";
    json j;
    {
        std::ifstream in(f);
        if (in) try { j = json::parse(in); } catch (...) {}
    }
    int64_t now = (int64_t)std::time(nullptr);
    if (!j.is_object() || now - j.value("when", (int64_t)0) > 7 * 86400) {
        json cats = json::array(), tags = json::array();
        for (int page = 1; page <= 5; ++page) {
            int st = 0;
            std::string b = HttpGet(L"/wp-json/wp/v2/categories?per_page=100&_fields=id,name&page=" + std::to_wstring(page), st);
            if (st != 200) break;
            json a = json::parse(b, nullptr, false);
            if (!a.is_array() || a.empty()) break;
            for (auto& c : a) cats.push_back(c);
        }
        for (int page = 1; page <= 5; ++page) {
            int st = 0;
            std::string b = HttpGet(L"/wp-json/wp/v2/tags?per_page=100&_fields=id,name&page=" + std::to_wstring(page), st);
            if (st != 200) break;
            json a = json::parse(b, nullptr, false);
            if (!a.is_array() || a.empty()) break;
            for (auto& c : a) tags.push_back(c);
        }
        if (cats.empty()) return !g_cats.empty();
        j = { { "when", now }, { "categories", cats }, { "tags", tags } };
        std::error_code ec;
        std::filesystem::create_directories(Folder(), ec);
        std::ofstream(f) << j.dump();
    }
    std::lock_guard lk(g_mx);
    g_cats.clear();
    g_tags.clear();
    std::map<int, std::string> gal;
    static const std::regex re(R"(^\s*(\d{1,3})\s+(.*?)\s*\((.*)\)\s*$)");
    for (auto& c : j.value("categories", json::array())) {
        std::string name = Unescape(c.value("name", ""));
        std::smatch m;
        Cat cat;
        if (std::regex_match(name, m, re)) {
            int n = std::stoi(m[1].str());
            if (n >= 1 && n <= 256) {
                cat.galaxy = n - 1;
                cat.galaxyName = m[2].str();
                cat.platform = m[3].str();
                gal[cat.galaxy] = std::to_string(n) + " " + cat.galaxyName;
            }
        }
        g_cats[c.value("id", 0)] = cat;
    }
    for (auto& t : j.value("tags", json::array())) g_tags[t.value("id", 0)] = Unescape(t.value("name", ""));
    g_galaxies.assign(gal.begin(), gal.end());
    return !g_cats.empty();
}

void Search(std::string query, bool basesOnly, int galaxy, int page)
{
    if (g_fetching.exchange(true)) return;
    {
        std::lock_guard lk(g_mx);
        g_repoStatus = L::T("bases.repo.searching");
    }
    std::thread([=] {
        std::string status;
        std::vector<Entry> found;
        int pages = 0;
        if (!LoadNames()) status = L::T("bases.repo.offline");
        else {
            std::wstring path = L"/wp-json/wp/v2/posts?per_page=50&_fields=id,title,link,content,categories,tags,date&page=" + std::to_wstring(page);
            if (!query.empty()) path += L"&search=" + UrlPart(query);
            if (basesOnly) path += L"&tags=" + std::to_wstring(kBaseTag);
            if (galaxy >= 0) {
                std::wstring ids;
                std::lock_guard lk(g_mx);
                for (auto& [id, c] : g_cats)
                    if (c.galaxy == galaxy) ids += (ids.empty() ? L"" : L",") + std::to_wstring(id);
                if (!ids.empty()) path += L"&categories=" + ids;
            }
            int st = 0;
            std::string body = HttpGet(path, st);
            json a = json::parse(body, nullptr, false);
            if (st == 400 && page > 1) status = L::T("bases.repo.end");
            else if (st != 200 || !a.is_array()) status = L::Tf("bases.repo.failed", std::to_string(st));
            else {
                // the address sits in a <pre> block; galleries before it can hold other hex runs
                static const std::regex pre(R"(<pre>\s*([0-9A-Fa-f]{12})\s*</pre>)"), glyph(R"(\b([0-9A-Fa-f]{12})\b)");
                std::lock_guard lk(g_mx);
                for (auto& p : a) {
                    std::string content = p["content"].value("rendered", ""), g;
                    std::smatch m;
                    if (std::regex_search(content, m, pre) || std::regex_search(content, m, glyph)) g = m[1].str();
                    if (g.empty()) continue;
                    Entry e;
                    e.source = Repo;
                    e.name = Unescape(p["title"].value("rendered", ""));
                    e.link = p.value("link", "");
                    int gal = 0;
                    for (auto& c : p.value("categories", json::array())) {
                        auto it = g_cats.find(c.get<int>());
                        if (it != g_cats.end() && it->second.galaxy >= 0) { gal = it->second.galaxy; e.platform = it->second.platform; }
                    }
                    for (auto& t : p.value("tags", json::array())) {
                        auto it = g_tags.find(t.get<int>());
                        if (it == g_tags.end() || it->second.empty() || it->second.starts_with("Version") || it->second.starts_with("Group")) continue;
                        if (!e.tags.empty()) e.tags += ", ";
                        std::string tg = it->second;
                        while (!tg.empty() && (tg[0] == '*' || tg[0] == '+')) tg.erase(0, 1);
                        e.tags += tg;
                    }
                    if (!GlyphsToAddress(g, gal, e.ua)) continue;
                    std::tm tm{};
                    std::string date = p.value("date", "");
                    if (sscanf_s(date.c_str(), "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
                        tm.tm_year -= 1900;
                        tm.tm_mon -= 1;
                        e.when = (int64_t)_mkgmtime(&tm);
                    }
                    found.push_back(std::move(e));
                }
                pages = 1;
                status = found.empty() && page == 1 ? std::string(L::T("bases.repo.none")) : std::string();
            }
        }
        {
            std::lock_guard lk(g_mx);
            if (page == 1) g_repo.clear();
            for (auto& e : found) g_repo.push_back(std::move(e));
            g_repoPage = page;
            g_repoPages = pages ? page + (found.size() >= 1 ? 1 : 0) : g_repoPages;
            g_repoStatus = status;
            ++g_version;
        }
        log::Info("Bases", "Portal Repository page {}: {} addresses", page, found.size());
        g_fetching = false;
    }).detach();
}

// ---- UI ----

struct
{
    ui::Browser game, repo, mine;
    std::vector<ui::BrowserEntry> gameEntries, repoEntries, mineEntries;
    std::map<std::string, Entry> byKey;
    uint64_t seen = 0;
    int view = 0; // 0 featured and seen, 1 your bases, 2 Portal Repository
    char query[96] = {};
    bool basesOnly = true;
    int galaxy = 0; // picker index, 0 = all
} g;

// "Planet, System" when known, otherwise the galaxy, system and planet numbers.
std::string Place(const Entry& e)
{
    if (e.system.empty()) return AddressWhere(e.ua);
    std::string g = L::Tf("bases.galaxy", std::to_string(((e.ua >> 32) & 0xFF) + 1));
    return (e.planet.empty() ? "" : e.planet + ", ") + e.system + "  (" + g + ")";
}

int Signed(uint64_t v, int bits) { int s = (int)(v & ((1ull << bits) - 1)); return s >= (1 << (bits - 1)) ? s - (1 << bits) : s; }

// Metres in the same system when the spot is known, light years between systems.
std::string Distance(const Entry& e)
{
    bool ok;
    uint64_t me;
    double pos[3];
    {
        std::lock_guard lk(g_mx);
        ok = g_me.ok;
        me = g_me.ua;
        for (int k = 0; k < 3; ++k) pos[k] = g_me.pos[k];
    }
    if (!ok || !me) return {};
    if (((e.ua >> 32) & 0xFF) != ((me >> 32) & 0xFF)) return L::T("bases.dist.galaxy");
    uint64_t mask = 0x000FFFFFFFFFFFFFull & ~(0xFull << 52) & ~(0xFFull << 32);
    if ((e.ua & mask) == (me & mask)) {
        if (!e.hasPos) return L::T("bases.dist.system");
        double dx = e.pos[0] - pos[0], dy = e.pos[1] - pos[1], dz = e.pos[2] - pos[2], d = std::sqrt(dx * dx + dy * dy + dz * dz);
        char b[32];
        if (d < 1000) snprintf(b, sizeof b, "%.0f m", d);
        else snprintf(b, sizeof b, "%.1f km", d / 1000);
        return b;
    }
    double vx = Signed(e.ua, 12) - Signed(me, 12), vy = Signed(e.ua >> 24, 8) - Signed(me >> 24, 8), vz = Signed(e.ua >> 12, 12) - Signed(me >> 12, 12);
    double ly = std::sqrt(vx * vx + vy * vy + vz * vz) * 400;
    std::string n = std::to_string((long long)std::llround(ly));
    for (int i = (int)n.size() - 3; i > 0; i -= 3) n.insert(i, ",");
    return L::Tf("bases.dist.ly", ly < 400 ? std::string(L::T("bases.dist.near")) : n);
}

std::string Date(int64_t t)
{
    if (!t) return {};
    char b[32] = {};
    time_t tt = (time_t)t;
    tm lt{};
    if (!localtime_s(&lt, &tt)) strftime(b, sizeof b, "%Y-%m-%d", &lt);
    return b;
}

void Rebuild()
{
    g.gameEntries.clear();
    g.repoEntries.clear();
    g.mineEntries.clear();
    g.byKey.clear();
    std::lock_guard lk(g_mx);
    auto add = [](const Entry& e, std::vector<ui::BrowserEntry>& out) {
        ui::BrowserEntry b;
        b.key = KeyOf(e);
        b.title = e.name.empty() ? AddressGlyphs(e.ua) : e.name;
        std::string who = e.owner.empty() ? e.platform : e.owner + (e.platform.empty() ? "" : "  (" + e.platform + ")");
        b.subtitle = (who.empty() ? "" : who + "   /   ") + Place(e);
        b.glyph = e.source == Featured ? icon::Star : e.source == Seen ? icon::Binoculars : e.source == Mine ? icon::Pin : icon::Planet;
        b.chip = e.source == Featured ? 1 : e.source == Seen ? 2 : 0;
        b.rare = e.source == Featured;
        b.group = e.owner;
        b.value = (double)e.when;
        Entry copy = e;
        copy.key = b.key;
        g.byKey[b.key] = std::move(copy);
        out.push_back(std::move(b));
    };
    for (auto& e : g_featured) add(e, g.gameEntries);
    for (auto& e : g_seen) add(e, g.gameEntries);
    for (auto& e : g_repo) add(e, g.repoEntries);
    for (auto& e : g_mine) add(e, g.mineEntries);
    g.seen = g_version;
    g.game.Invalidate();
    g.repo.Invalidate();
    g.mine.Invalidate();
}

void Note(ImDrawList* dl, float x, float& y, float w, const std::string& s, ImU32 col)
{
    if (s.empty()) return;
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x, y), col, s.c_str(), nullptr, w);
    y += ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, w, s.c_str()).y + S(8);
}

void StartSearch(int page)
{
    int galaxy = -1;
    {
        std::lock_guard lk(g_mx);
        if (g.galaxy > 0 && g.galaxy <= (int)g_galaxies.size()) galaxy = g_galaxies[g.galaxy - 1].first;
    }
    Search(g.query, g.basesOnly, galaxy, page);
}

void DrawRepo(ImDrawList* dl, float x, float& y, float w)
{
    float bw = S(110);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool enter = ui::TextField("##basesq", g.query, sizeof g.query, w - bw - S(8), L::T("bases.repo.hint"));
    ImGui::SetCursorScreenPos(ImVec2(x + w - bw, y));
    if ((ui::Button("##basesgo", L::T("bases.repo.search"), bw, ui::Tone::Green, !g_fetching) || enter) && !g_fetching) StartSearch(1);
    y += S(44);
    std::vector<std::string> gals{ L::T("bases.repo.allgalaxies") };
    std::string status;
    int page, pages;
    {
        std::lock_guard lk(g_mx);
        for (auto& [i, n] : g_galaxies) gals.push_back(n);
        status = g_repoStatus;
        page = g_repoPage;
        pages = g_repoPages;
    }
    if (g.galaxy >= (int)gals.size()) g.galaxy = 0;
    float half = (w - S(8)) / 2;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::Dropdown("##basesgal", gals, &g.galaxy, half);
    dl->AddText(ui::theme::Body(), S(15), ImVec2(x + half + S(16), y + S(6)), P().textDim, L::T("bases.repo.basesonly"));
    ImGui::SetCursorScreenPos(ImVec2(x + w - S(70), y));
    ui::Switch("##basesonly", &g.basesOnly);
    y += S(44);
    Note(dl, x, y, w, status, P().textBody);
    if (page && pages > page && !g_fetching) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##basesmore", L::T("bases.repo.more"), w, ui::Tone::Neutral, true)) StartSearch(page + 1);
        y += S(46);
    }
    Note(dl, x, y, w, L::T("bases.repo.credit"), P().textFaint);
}

void DrawList(const char* query)
{
    if (g.seen != g_version) Rebuild();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(8), x = p.x, y = p.y;
    bool repo = MainBuild();
    if (!repo && g.view == 2) g.view = 0;
    {
        int n = repo ? 3 : 2;
        float bw3 = (w - S(8) * (n - 1)) / n;
        const char* keys[] = { "bases.view.game", "bases.view.mine", "bases.view.repo" };
        for (int i = 0; i < n; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(x + (bw3 + S(8)) * i, y));
            ImGui::PushID(i);
            if (ui::Button("##basesview", L::T(keys[i]), bw3, g.view == i ? ui::Tone::Teal : ui::Tone::Neutral)) g.view = i;
            ImGui::PopID();
        }
        y += S(46);
    }
    if (g.view == 2) DrawRepo(dl, x, y, w);
    else if (g.view == 1) {
        size_t n;
        {
            std::lock_guard lk(g_mx);
            n = g_mine.size();
        }
        Note(dl, x, y, w, L::Tf("bases.mine.count", std::to_string(n)), P().textDim);
    }
    else {
        std::wstring slot;
        size_t nf, ns;
        {
            std::lock_guard lk(g_mx);
            slot = g_seenKey.empty() ? g_slotKey : g_seenKey;
            nf = g_featured.size();
            ns = g_seen.size();
        }
        std::string s = L::Tf("bases.counts", std::to_string(nf), std::to_string(ns));
        if (!slot.empty()) s += "   /   " + L::Tf("bases.slot", paths::Utf8(slot.substr(slot.rfind(L"_slot") + 5)));
        Note(dl, x, y, w, s, P().textDim);
        if (!nf) Note(dl, x, y, w, L::T("bases.featured.hint"), P().textDim);
        if (!ns) Note(dl, x, y, w, L::T("bases.empty"), P().textDim);
    }
    dl->AddLine(ImVec2(x, y), ImVec2(x + w, y), P().line, S(1));
    y += S(12);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (g.view == 2) g.repo.Draw(g.repoEntries, query);
    else if (g.view == 1) g.mine.Draw(g.mineEntries, query);
    else g.game.Draw(g.gameEntries, query);
}

void Fact(ImDrawList* dl, float x, float& y, float w, const std::string& k, const std::string& v)
{
    if (v.empty()) return;
    y += ui::RichText(dl, ui::theme::Body(), S(15), ImVec2(x, y), w, P().textBody, "<TECHNOLOGY>" + k + "<> " + v) + S(4);
}

float DrawInspector(ImVec2 a, float w)
{
    auto& br = g.view == 2 ? g.repo : g.view == 1 ? g.mine : g.game;
    auto& entries = g.view == 2 ? g.repoEntries : g.view == 1 ? g.mineEntries : g.gameEntries;
    const ui::BrowserEntry* be = entries.empty() ? nullptr : br.Focused(entries);
    if (!be) return 0; // default tab info
    auto it = g.byKey.find(be->key);
    if (it == g.byKey.end()) return 0;
    const Entry e = it->second;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ui::DrawEntryHeader(dl, a, w, *be);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    const char* kind = e.source == Featured ? "bases.kind.featured" : e.source == Seen ? "bases.kind.seen" : e.source == Mine ? "bases.kind.mine" : "bases.kind.repo";
    Fact(dl, x, y, bw, L::T("bases.f.kind"), L::T(kind));
    Fact(dl, x, y, bw, L::T("bases.f.owner"), e.owner);
    Fact(dl, x, y, bw, L::T("bases.f.platform"), e.platform);
    Fact(dl, x, y, bw, L::T("bases.f.type"), e.type);
    Fact(dl, x, y, bw, L::T("bases.f.where"), Place(e));
    if (!e.system.empty()) Fact(dl, x, y, bw, L::T("bases.f.address"), AddressWhere(e.ua));
    Fact(dl, x, y, bw, L::T("bases.f.distance"), Distance(e));
    Fact(dl, x, y, bw, L::T("bases.f.glyphs"), AddressGlyphs(e.ua));
    Fact(dl, x, y, bw, L::T("bases.f.tags"), e.tags);
    if (e.source != Mine) Fact(dl, x, y, bw, L::T(e.source == Repo ? "bases.f.posted" : "bases.f.when"), Date(e.when));
    y += S(8);
    bool live = game::FrameHookReady() && game::PlayerState();
    std::string name = e.name.empty() ? AddressGlyphs(e.ua) : e.name;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##baseswarp", L::T(live ? "bases.warp" : "bases.ingame"), bw, ui::Tone::Green, live)) {
        WarpToAddress(e.ua, name);
        ui::Toast(L::T("bases.warping"), name, icon::Rocket);
    }
    y += S(46);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##basesspot", L::T("bases.spot"), bw, ui::Tone::Teal, true)) {
        SaveSystemSpot(e.ua, name);
        ui::Toast(L::T("bases.spotsaved"), name, icon::Pin);
    }
    y += S(46);
    if (!e.link.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::Button("##basesopen", L::T("bases.open"), bw, ui::Tone::Neutral, true))
            ShellExecuteW(nullptr, L"open", paths::Wide(e.link).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        y += S(46);
    }
    if (e.source == Seen) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        if (ui::HoldButton("##basesforget", L::T("bases.forget"), bw)) {
            std::lock_guard lk(g_mx);
            std::erase_if(g_seen, [&](const Entry& o) { return KeyOf(o) == e.key; });
            WriteList(SeenFile(g_seenKey), g_seen);
            g.game.ClearFocus();
            ++g_version;
        }
        y += S(50);
    }
    return y;
}

}

void InstallBases()
{
    if (const Target& t = Loc().Resolve("Bases.DownloadsManager"); t.ok) g_dl = t.global;
    if (const Target& t = Loc().Resolve("Bases.FeaturedQuery"); t.ok && !MakeQueryThunk(Abs(t.func), Abs(t.site)))
        log::Warn("Bases", "Featured query block found, but its function start changed");
    auto off = [](const char* cls, const char* f, uint32_t& out) { if (auto m = meta::Path(cls, f)) out = m->offset; };
    off("cGcUniverseAddressData", "RealityIndex", U.galaxy);
    off("cGcUniverseAddressData", "GalacticAddress", U.ga);
    off("cGcGalacticAddressData", "VoxelX", U.x);
    off("cGcGalacticAddressData", "VoxelY", U.y);
    off("cGcGalacticAddressData", "VoxelZ", U.z);
    off("cGcGalacticAddressData", "SolarSystemIndex", U.system);
    off("cGcGalacticAddressData", "PlanetIndex", U.planet);
    if (HMODULE kb = GetModuleHandleW(L"kernelbase.dll"))
        if (void* f = (void*)GetProcAddress(kb, "CreateFileW"))
            if (auto h = safetyhook::InlineHook::create(f, reinterpret_cast<void*>(&CreateFileWHk))) g_cfw = std::move(*h);
    {
        std::lock_guard lk(g_mx);
        g_featured = ReadList(FeaturedFile(), Featured);
        g_featuredLoaded = !g_featured.empty();
    }
    log::Info("Bases", "Featured list {} (request {}), save watch {}, Portal Repository {}", g_dl ? "ready" : "missing", g_query ? "ready" : "missing",
              g_cfw ? "on" : "off",
              MainBuild() ? "on" : "off (forum build)");
    game::EveryFrame(Tick);

    Registry::Get().Post([] {
        g.game.chips = { L::T("bases.chip.all"), L::T("bases.chip.featured"), L::T("bases.chip.seen") };
        g.game.searchBox = true;
        g.game.sortable = true;
        g.game.sortOptions = { { ui::SortBy::Default, L::T("bases.sort.default") }, { ui::SortBy::Name, L::T("bases.sort.name") },
                               { ui::SortBy::Type, L::T("bases.sort.owner") }, { ui::SortBy::Value, L::T("bases.sort.recent") } };
        g.game.captions = true;
        g.repo.sortable = true;
        g.repo.sortOptions = { { ui::SortBy::Default, L::T("bases.sort.default") }, { ui::SortBy::Name, L::T("bases.sort.name") },
                               { ui::SortBy::Value, L::T("bases.sort.recent") } };
        g.repo.captions = true;
        g.mine.searchBox = true;
        g.mine.sortable = true;
        g.mine.sortOptions = { { ui::SortBy::Default, L::T("bases.sort.default") }, { ui::SortBy::Name, L::T("bases.sort.name") } };
        g.mine.captions = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] {
            std::lock_guard lk(g_mx);
            return "(" + std::to_string(g_featured.size() + g_seen.size() + g_mine.size()) + ")";
        };
        ui::RegisterPage("bases", std::move(page));
    });
}

}
