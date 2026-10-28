#include "features/Expeditions.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
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
#include <nlohmann/json.hpp>
#include <zstd.h>
#include <windows.h>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>

// Expeditions tab: run past expeditions offline from season JSON files, unlock their rewards.
namespace os::features {

namespace {

namespace fs = std::filesystem;
namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

struct Fns
{
    void (*apply)(void*, const void*) = nullptr;
    void (*copy)(void*, const void*) = nullptr;
    void (*ctor)(void*) = nullptr;
    void (*dtor)(void*) = nullptr;
    int (*fromJson)(void*, void*, int) = nullptr;
    void (*jsonCtor)(void*) = nullptr;
    bool (*jsonParse)(void*, const char*) = nullptr;
    void (*loadCache)(void*) = nullptr;
    void (*resetSeason)(void*) = nullptr;
    void (*unlockEntry)(const void*) = nullptr;
    int64_t mgr = -1, history = -1, endTime = 0x6748;
    uint32_t seasonSize = 0x6BA0, startTime = 0x6758;
    safetyhook::MidHook* swap = nullptr;
} F;

// SeasonMgr flags after its two seasons: 1 while an expedition save is loaded, season cache is valid.
uint32_t InSeasonSave() { return 2 * F.seasonSize + 0x53; }
uint32_t CacheValid() { return 2 * F.seasonSize + 0x54; }

struct FileInfo { std::string path, title, label, icon, finalReward; int season = -1, remix = 0, display = 0; };

struct Exp
{
    std::string key, name, desc, icon, finalReward, file;
    int season = -1, remix = 0, display = 0;
};

std::mutex g_mx;
std::vector<Exp> g_list;           // published to the UI
std::atomic<bool> g_loading{ false };
std::atomic<uintptr_t> g_armed{ 0 }; // season buffer in use by the swap hook
std::string g_armedKey;

struct UiState
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    size_t built = SIZE_MAX;
} g;

fs::path Folder() { return fs::path(paths::Root()) / L"OpenSkies.expeditions"; }

std::string ReadFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

#include "EmbeddedSeasons.inc"

// Writes the season files that ship inside the DLL, once per session, skipping ones already there.
void ExtractEmbedded()
{
    static bool done = false;
    if (done) return;
    done = true;
    uint32_t raw = 0;
    memcpy(&raw, kSeasonPack, 4);
    std::vector<uint8_t> buf(raw);
    size_t got = ZSTD_decompress(buf.data(), raw, kSeasonPack + 4, sizeof(kSeasonPack) - 4);
    if (ZSTD_isError(got) || got != raw || raw < 8 || memcmp(buf.data(), "OSXP", 4) != 0) {
        log::Warn("Expeditions", "Built-in season data could not be read");
        return;
    }
    std::error_code ec;
    fs::create_directories(Folder(), ec);
    uint32_t n = 0, written = 0;
    memcpy(&n, buf.data() + 4, 4);
    size_t at = 8;
    for (uint32_t i = 0; i < n && at + 2 <= raw; ++i) {
        uint16_t nl = 0;
        memcpy(&nl, buf.data() + at, 2);
        at += 2;
        if (at + nl + 4 > raw) break;
        std::string name((const char*)buf.data() + at, nl);
        at += nl;
        uint32_t sz = 0;
        memcpy(&sz, buf.data() + at, 4);
        at += 4;
        if (at + sz > raw) break;
        fs::path p = Folder() / name;
        if (fs::file_size(p, ec) != sz) {
            std::ofstream(p, std::ios::binary).write((const char*)buf.data() + at, sz);
            ++written;
        }
        at += sz;
    }
    if (written) log::Info("Expeditions", "Wrote {} built-in season files", written);
}

std::vector<FileInfo> ScanFiles()
{
    std::vector<FileInfo> out;
    std::error_code ec;
    ExtractEmbedded();
    fs::create_directories(Folder(), ec);
    for (auto& e : fs::directory_iterator(Folder(), ec)) {
        if (e.path().extension() != ".json" && e.path().extension() != ".JSON") continue;
        auto j = nlohmann::json::parse(ReadFile(e.path()), nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        FileInfo fi;
        fi.path = e.path().string();
        fi.season = j.value("SeasonNumber", j.value("SeasonId", -1));
        fi.remix = j.value("RemixNumber", 0);
        fi.display = j.value("DisplayNumber", 0);
        fi.finalReward = j.value("FinalReward", std::string());
        if (!fi.finalReward.empty() && fi.finalReward[0] == '^') fi.finalReward.erase(0, 1);
        fi.title = j.value("Title", std::string());
        fi.label = j.value("Label", std::string());
        if (j.contains("MainIcon") && j["MainIcon"].is_object()) fi.icon = j["MainIcon"].value("Filename", std::string());
        out.push_back(std::move(fi));
    }
    return out;
}

std::string Str(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return s;
}

std::string VarStr(uintptr_t at)
{
    uintptr_t p = 0;
    return Rd(at, p) && p ? Str(p, 260) : std::string();
}

// Game thread: the past expedition table plus the local files.
void Load()
{
    auto files = ScanFiles();
    std::vector<Exp> list;
    uintptr_t d = game::Data();
    uintptr_t table = 0, data = 0;
    int32_t n = 0;
    if (d && F.history > 0 && Rd(d + F.history, table) && table && Rd(table, data) && Rd(table + 8, n) && data && n > 0 && n < 500) {
        uint32_t size = 0xB8;
        if (auto* ci = meta::Get("cGcHistoricalSeasonData")) size = ci->size;
        auto off = [](const char* f, uint32_t fb) { auto m = meta::Path("cGcHistoricalSeasonData", f); return m ? m->offset : fb; };
        uint32_t oName = off("SeasonName", 0), oIcon = off("MainIcon", 0x60), oFinal = off("FinalReward", 0x78), oDesc = off("Description", 0x94);
        uint32_t oSeason = off("SeasonNumber", 0x90), oRemix = off("RemixNumber", 0x8C), oDisp = off("DisplayNumber", 0x88);
        auto file = meta::Path("cTkTextureResource", "Filename");
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t r = data + (uintptr_t)i * size;
            Exp e;
            e.name = game::catalog::Translate(Str(r + oName, 32));
            e.desc = game::catalog::Translate(Str(r + oDesc, 32));
            e.icon = VarStr(r + oIcon + (file ? file->offset : 0));
            e.finalReward = Str(r + oFinal, 16);
            Rd(r + oSeason, e.season);
            Rd(r + oRemix, e.remix);
            Rd(r + oDisp, e.display);
            e.key = "H:" + std::to_string(e.season) + ":" + std::to_string(e.remix);
            // Best file for this row: same season and remix, then the same display number or final reward.
            int best = 0;
            for (auto& f : files) {
                int score = f.season == e.season && f.remix == e.remix ? 3
                          : (f.display > 0 && f.display == e.display) || (!f.finalReward.empty() && f.finalReward == e.finalReward) ? 2
                          : 0;
                if (score > best) {
                    best = score;
                    e.file = f.path;
                    if (e.icon.empty()) e.icon = f.icon;
                }
            }
            list.push_back(std::move(e));
        }
    }
    // files that match no table row still get an entry
    for (auto& f : files) {
        bool used = false;
        for (auto& e : list) used |= e.file == f.path;
        if (used) continue;
        Exp e;
        e.key = "F:" + f.path;
        e.name = !f.label.empty() ? f.label : f.title.empty() ? fs::path(f.path).stem().string() : game::catalog::Translate(f.title);
        e.season = f.season;
        e.remix = f.remix;
        e.display = f.display;
        e.finalReward = f.finalReward;
        e.file = f.path;
        e.icon = f.icon;
        list.push_back(std::move(e));
    }
    {
        std::lock_guard lk(g_mx);
        g_list = std::move(list);
    }
    g_loading = false;
}

void Reload()
{
    if (g_loading.exchange(true)) return;
    game::Post(Load);
}

void SwapCb(Context& c)
{
    uintptr_t buf = g_armed;
    if (!buf || !F.copy) return;
    uintptr_t cached = c.rcx + F.seasonSize;
    if (c.rdx == cached) F.copy((void*)cached, (const void*)buf);
}

void Arm(std::string key, std::string path, std::string name)
{
    game::Post([key, path, name] {
        uintptr_t d = game::Data();
        if (!d || F.mgr < 0 || !F.apply || !F.copy || !F.ctor || !F.fromJson || !F.jsonCtor || !F.jsonParse) return;
        std::string text = ReadFile(path);
        if (text.empty()) return;
        size_t size = std::max<size_t>(F.seasonSize, 0x6BA0) + 0x1000;
        auto* buf = (uint8_t*)_aligned_malloc(size, 16);
        if (!buf) return;
        memset(buf, 0, size);
        F.ctor(buf);

        alignas(16) uint8_t reader[0x300] = {};
        F.jsonCtor(reader);
        bool parsed = F.jsonParse(reader, text.c_str());
        int rc = parsed ? F.fromJson(buf, reader, -1) : -1;
        uintptr_t vt = 0, rdtor = 0;
        if (Rd((uintptr_t)reader, vt) && vt && Rd(vt, rdtor) && rdtor)
            ((void (*)(void*, int))rdtor)(reader, 0);

        if (rc != 9) {
            log::Warn("Expeditions", "{} did not parse (code {})", path, rc);
            if (F.dtor) F.dtor(buf);
            _aligned_free(buf);
            return;
        }
        uint64_t now = (uint64_t)_time64(nullptr);
        Wr<uint64_t>((uintptr_t)buf + F.endTime, now + 400ull * 86400);
        Wr<uint64_t>((uintptr_t)buf + F.startTime, now - 86400);
        uintptr_t old = g_armed.exchange((uintptr_t)buf); // the previous buffer is leaked on purpose: the game may still hold a copy
        (void)old;
        g_armedKey = key;
        if (F.swap) (void)F.swap->enable();
        uintptr_t mgr = d + F.mgr;
        uintptr_t cached = mgr + F.seasonSize;
        F.copy((void*)cached, buf);
        Wr<uint8_t>(mgr + CacheValid(), 1);
        uint8_t inSeasonSave = 0;
        Rd(mgr + InSeasonSave(), inSeasonSave);
        if (!inSeasonSave) F.apply((void*)mgr, (const void*)cached);
        log::Info("Expeditions", "Armed {}", path);
        Registry::Get().Post([name] {
            ui::Toast(L::Tf("exp.armedtoast", name), L::T("exp.next"), icon::Flag);
        });
    });
}

void Disarm()
{
    g_armed = 0;
    g_armedKey.clear();
    if (F.swap) (void)F.swap->disable();
    game::Post([] {
        uintptr_t d = game::Data();
        if (!d || F.mgr < 0) return;
        uintptr_t mgr = d + F.mgr;
        Wr<uint8_t>(mgr + CacheValid(), 0);
        if (F.loadCache) F.loadCache((void*)mgr);
        uint8_t inSeasonSave = 0;
        Rd(mgr + InSeasonSave(), inSeasonSave);
        if (!inSeasonSave && F.resetSeason) F.resetSeason((void*)mgr);
    });
}

void DrawList(const char* query)
{
    if (!game::Data()) return;
    if (g.built == SIZE_MAX && !g_loading) Reload();
    std::vector<Exp> list;
    {
        std::lock_guard lk(g_mx);
        list = g_list;
    }
    if (list.size() != g.built) {
        g.built = list.size();
        g.view.clear();
        for (auto& e : list) {
            ui::BrowserEntry b;
            b.key = e.key;
            b.title = e.name;
            b.subtitle = L::Tf("exp.number", std::to_string(e.display ? e.display : e.season)) + (e.file.empty() ? "" : "   /   " + std::string(L::T("exp.ready")));
            b.body = e.desc;
            b.icon = e.icon;
            b.glyph = icon::Flag;
            b.chip = e.file.empty() ? 2 : 1;
            b.marked = !e.file.empty();
            g.view.push_back(std::move(b));
        }
        g.browser.Invalidate();
    }
    g.browser.Draw(g.view, query);
}

float DrawInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = g.browser.Focused(g.view);
    Exp e;
    if (f) {
        std::lock_guard lk(g_mx);
        for (auto& x : g_list) if (x.key == f->key) e = x;
    }
    ui::BrowserEntry head;
    if (f) head = *f;
    else {
        head.title = L::T("tab.expeditions");
        head.subtitle = L::T("exp.subtitle");
        head.body = L::Tf("exp.help", Folder().string());
        head.glyph = icon::Flag;
    }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2;
    bool live = game::FrameHookReady();

    if (f && !e.file.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        bool armed = g_armed && g_armedKey == e.key;
        if (ui::Button("##arm", L::T(armed ? "exp.armed" : "exp.arm"), bw, ui::Tone::Green, live && F.apply && !armed)) {
            Arm(e.key, e.file, e.name);
        }
        y += S(46);
    }
    if (f && !e.finalReward.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::HoldButton("##rewards", L::T("exp.rewards"), bw)) {
            std::string id = e.finalReward;
            game::BackupSaveOnce();
            game::Post([id] {
                char buf[16] = {};
                memcpy(buf, id.data(), std::min<size_t>(id.size(), 16));
                if (F.unlockEntry) F.unlockEntry(buf);
            });
            ui::Toast(L::Tf("exp.rewardsdone", e.name), "", icon::Check);
        }
        y += S(46);
    }
    if (g_armed) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##disarm", L::T("exp.disarm"), bw, ui::Tone::Red, true)) Disarm();
        y += S(46);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    if (ui::Button("##reload", L::T("exp.reload"), bw, ui::Tone::Neutral, live)) { g.built = SIZE_MAX - 1; Reload(); }
    y += S(46);
    const char* note = L::T("exp.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(a.x + pad, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
    return y;
}

template <class Fn> void Bind(const char* name, Fn& fn)
{
    const Target& t = Loc().Resolve(name);
    if (t.ok) fn = (Fn)Abs(t.func);
    else log::Warn("Expeditions", "{} missing: {}", name, t.reason);
}

int64_t Off(const char* name)
{
    const Target& t = Loc().Resolve(name);
    return t.ok && t.offset ? *t.offset : -1;
}

}

void InstallExpeditions()
{
    Bind("Expedition.ApplySeason", F.apply);
    Bind("Expedition.SeasonCopy", F.copy);
    Bind("Expedition.SeasonCtor", F.ctor);
    Bind("Expedition.SeasonDtor", F.dtor);
    Bind("Expedition.SeasonFromJson", F.fromJson);
    Bind("Expedition.JsonReaderCtor", F.jsonCtor);
    Bind("Expedition.JsonReaderParse", F.jsonParse);
    Bind("Expedition.LoadCache", F.loadCache);
    Bind("Expedition.ResetSeason", F.resetSeason);
    Bind("Reward.UnlockTableEntry", F.unlockEntry);
    F.mgr = Off("Expedition.SeasonMgr");
    F.history = Off("Expedition.HistoryTable");
    if (int64_t e = Off("Expedition.TimeLeft"); e > 0) F.endTime = e;
    if (auto* ci = meta::Get("cGcSeasonalGameModeData")) F.seasonSize = ci->size;
    if (auto m = meta::Path("cGcSeasonalGameModeData", "StartTimeUTC")) F.startTime = m->offset;
    if (auto m = meta::Path("cGcSeasonalGameModeData", "EndTimeUTC")) F.endTime = m->offset;
    if (const Target& t = Loc().Resolve("Expedition.ApplySeason"); t.ok) F.swap = Mid(t.func, SwapCb);
    log::Info("Expeditions", "Ready: apply {}, parse {}, reader {}, copy {}, manager +0x{:X}",
              F.apply != nullptr, F.fromJson != nullptr, F.jsonCtor != nullptr && F.jsonParse != nullptr, F.copy != nullptr, F.mgr);

    Registry::Get().Post([] {
        g.browser.chips = { L::T("exp.chip.all"), L::T("exp.chip.ready"), L::T("exp.chip.missing") };
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        ui::RegisterPage("expeditions", std::move(page));
    });
}

}
