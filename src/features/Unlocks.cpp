#include "features/Unlocks.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "features/ItemsPage.h"
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
#include <set>
#include <unordered_map>
#include <unordered_set>

// Unlocks tab: learn tech, blueprints, specials, titles, words, glyphs and rewards.
namespace os::features {

namespace {

namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

enum Cat { All, Tech, Products, BaseParts, Specials, Titles, Words, Glyphs, Season, Twitch, Platform, Exclusive, CatCount };
const char* kCatKeys[CatCount] = { "unl.chip.all", "unl.chip.tech", "unl.chip.products", "unl.chip.baseparts", "unl.chip.specials",
                                   "unl.chip.titles", "unl.chip.words", "unl.chip.glyphs", "unl.chip.season", "unl.chip.twitch", "unl.chip.platform",
                                   "unl.chip.exclusive" };

// Expedition and pre-order exclusives (the set from Nexus mod 2589): tech blueprints and the Utopia stations.
const std::unordered_set<std::string> kExclusiveTech = { "SHIPJUMP_SPEC", "HYPERDRIVE_SPEC", "LAUNCHER_SPEC", "PHOTONIX_CORE",
                                                         "BOLT_SM", "LASER_XO", "WORMTECH", "WORMREADER" };
const std::unordered_set<std::string> kExclusiveParts = { "S9_SHIPTREE", "S9_WEAPONTREE", "S9_BUILDERTREE", "S9_SUITTREE", "S9_EXOCRAFTTREE" };
// Core ship engines and their exclusive upgrades; clearing Core lets them be removed and swapped.
const char* kEngines[] = { "SHIPJUMP1", "HYPERDRIVE", "LAUNCHER", "SHIPJUMP_SPEC", "HYPERDRIVE_SPEC", "LAUNCHER_SPEC" };

struct Entry
{
    Cat cat = Tech;
    std::string key, id, title, subtitle, body, icon;
    std::string arg;        // product id for rewards, word group for words
    int race = 0;
    uintptr_t record = 0;   // tech record for LearnTech
    bool known = false;
    std::string item;       // product handed out on unlock (ship, Multi-Tool and other rewards)
    std::string reward;     // the item's GiveRewardOnSpecialPurchase: what the Quicksilver claim runs instead (a ship)
};

// Game functions and offsets.
struct Fns
{
    bool (*learnTech)(void*, const void*, bool, bool) = nullptr;
    bool (*learnProduct)(void*, const void*, bool) = nullptr;
    bool (*learnSpecial)(void*, const void*) = nullptr;
    bool (*unlockTitle)(void*, const void*, bool, bool) = nullptr;
    bool (*learnWord)(void*, const void*, int) = nullptr;
    bool (*seasonReward)(void*, const void*) = nullptr;
    void (*saveSettings)(void*) = nullptr;
    bool (*giveReward)(void* mgr, const void* id, const void* mission, const void* seed, bool peek, bool forceMsg, int* count, bool silent,
                       int invOverride, bool mining) = nullptr;
    void* rewardMgr = nullptr;
    int64_t knownTech = -1, knownProducts = -1, knownSpecials = -1, redeemedSeason = -1, wordGroups = -1;
    int64_t userSettings = -1, usTitles = -1, usDirty = -1, titleTable = -1, runes = -1;
} F;

std::mutex g_mx;
std::shared_ptr<const std::vector<Entry>> g_entries;
std::atomic<bool> g_building{ false };

struct UiState
{
    ui::Browser browser;
    std::shared_ptr<const std::vector<Entry>> snap;
    std::vector<ui::BrowserEntry> view;
    bool requested = false;
    bool again = false; // hand out reward items even when already owned
} g;

template <class Fn> void Bind(const char* name, Fn& fn)
{
    const Target& t = Loc().Resolve(name);
    if (t.ok) fn = (Fn)Abs(t.func);
    else log::Warn("Unlocks", "{} missing: {}", name, t.reason);
}

int64_t Off(const char* name)
{
    const Target& t = Loc().Resolve(name);
    return t.ok && t.offset ? *t.offset : -1;
}

std::string Id16(uintptr_t at)
{
    char b[17] = {};
    for (int i = 0; i < 16 && Rd(at + i, b[i]) && b[i]; ++i) {}
    return b;
}

std::string Str(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return s;
}

bool Printable(const std::string& s)
{
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7E) return false;
    return true;
}

// Vector of 16 byte ids: { u32 capacity; u32 count; T* data; }
std::unordered_set<std::string> VecIds(uintptr_t v)
{
    std::unordered_set<std::string> out;
    uint32_t n = 0;
    uintptr_t data = 0;
    if (!Rd(v + 4, n) || !Rd(v + 8, data) || !data || n > 100000) return out;
    for (uint32_t i = 0; i < n; ++i) out.insert(Id16(data + i * 16));
    return out;
}

// Flat hash set: keys at +0x10, info bytes at +0x18 (keys end where info starts).
std::unordered_set<std::string> SetIds(uintptr_t s, size_t keySize)
{
    std::unordered_set<std::string> out;
    uintptr_t keys = 0, info = 0;
    if (!Rd(s + 0x10, keys) || !Rd(s + 0x18, info) || !keys || info <= keys) return out;
    size_t slots = (info - keys) / keySize;
    if (slots > 200000) return out;
    for (size_t i = 0; i < slots; ++i) {
        uint8_t b = 0;
        if (Rd(info + i, b) && b) out.insert(keySize == 16 ? Id16(keys + i * keySize) : Str(keys + i * keySize, keySize));
    }
    return out;
}

// A table: slot holds a pointer to an object starting with { T* data; i32 count; }.
std::pair<uintptr_t, int32_t> Array(uintptr_t slot, uint32_t idOff, bool idIsTkId = true)
{
    uintptr_t obj = 0, data = 0;
    int32_t n = 0;
    if (Rd(slot, obj) && obj && Rd(obj, data) && Rd(obj + 8, n) && data && n > 0 && n < 100000) {
        std::string first = idIsTkId ? Id16(data + idOff) : Str(data + idOff, 32);
        if (Printable(first)) return { data, n };
    }
    // some tables are inline arrays rather than pointers
    if (Rd(slot, data) && Rd(slot + 8, n) && data && n > 0 && n < 100000) {
        std::string first = idIsTkId ? Id16(data + idOff) : Str(data + idOff, 32);
        if (Printable(first)) return { data, n };
    }
    return { 0, 0 };
}

uint32_t Field(const char* cls, const char* f, uint32_t fallback)
{
    auto m = meta::Path(cls, f);
    return m ? m->offset : fallback;
}

uint32_t Size(const char* cls, uint32_t fallback)
{
    auto* ci = meta::Get(cls);
    return ci && ci->size ? ci->size : fallback;
}

void FromItem(Entry& e, const std::string& id)
{
    if (auto* it = game::catalog::Find(id)) {
        e.title = it->name;
        e.body = it->desc;
        e.icon = it->icon;
    }
    if (e.title.empty()) e.title = id;
}

void Build()
{
    uintptr_t d = game::Data(), ps = game::PlayerState(), rm = game::Reality();
    if (!d || !ps || !rm) { g_building = false; return; }
    auto out = std::make_shared<std::vector<Entry>>();

    // Tech: skip procedural, repair-only and obsolete records (LearnTech refuses them).
    {
        auto known = F.knownTech > 0 ? VecIds(ps + F.knownTech) : std::unordered_set<std::string>{};
        uint32_t sz = Size("cGcTechnology", 0x2E0), id = Field("cGcTechnology", "ID", 0x108);
        uint32_t proc = Field("cGcTechnology", "Procedural", 0x2CC), rep = Field("cGcTechnology", "RepairTech", 0x2CD);
        auto [arr, n] = Array(rm + 0x10, id);
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t r = arr + (uintptr_t)i * sz;
            uint8_t p = 0, rt = 0;
            Rd(r + proc, p);
            Rd(r + rep, rt);
            Entry e;
            e.id = Id16(r + id);
            if (p || rt || e.id == "OBSOLETE" || !Printable(e.id)) continue;
            e.cat = kExclusiveTech.count(e.id) ? Exclusive : Tech;
            e.key = "T:" + e.id;
            e.record = r;
            e.known = known.count(e.id) != 0;
            FromItem(e, e.id);
            out->push_back(std::move(e));
        }
    }

    // Product blueprints from the three product tables; base parts are their own chip.
    {
        auto known = F.knownProducts > 0 ? VecIds(ps + F.knownProducts) : std::unordered_set<std::string>{};
        uint32_t sz = Size("cGcProductData", 0x300), id = Field("cGcProductData", "ID", 0x150);
        uint32_t craft = Field("cGcProductData", "IsCraftable", 0x2F5), type = Field("cGcProductData", "Type", 0x1E8);
        for (uint32_t slot : { 0x18u, 0x20u, 0x28u }) {
            auto [arr, n] = Array(rm + slot, id);
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t r = arr + (uintptr_t)i * sz;
                uint8_t c = 0;
                int32_t t = -1;
                Rd(r + craft, c);
                Rd(r + type, t);
                Entry e;
                e.id = Id16(r + id);
                if (!Printable(e.id)) continue;
                if (!c && t != 7 && !kExclusiveParts.count(e.id)) continue; // exclusives are often not craftable
                e.cat = kExclusiveParts.count(e.id) ? Exclusive : t == 4 ? BaseParts : Products;
                e.key = "P:" + e.id;
                e.known = known.count(e.id) != 0;
                FromItem(e, e.id);
                out->push_back(std::move(e));
            }
        }
    }

    // Purchaseable specials (Quicksilver shop, customisation, and retired/exclusive SpecialShop products).
    std::unordered_set<std::string> listedSpecials;
    auto knownSpecials = F.knownSpecials > 0 ? SetIds(ps + F.knownSpecials, 16) : std::unordered_set<std::string>{};
    {
        auto [arr, n] = Array(rm + 0x210, 0);
        uint32_t sz = Size("cGcPurchaseableSpecial", 0x20);
        for (int32_t i = 0; i < n; ++i) {
            Entry e;
            e.id = Id16(arr + (uintptr_t)i * sz);
            if (!Printable(e.id)) continue;
            listedSpecials.insert(e.id);
            e.cat = Specials;
            e.key = "S:" + e.id;
            e.known = knownSpecials.count(e.id) != 0;
            FromItem(e, e.id);
            out->push_back(std::move(e));
        }
    }

    // Season, twitch and platform rewards: the redeemed sets sit at +0, +0x40, +0x80 from the season set.
    if (F.redeemedSeason > 0) {
        struct R { Cat cat; uint32_t slot, size, keyOff, productOff; int64_t set; const char* prefix; };
        R rs[] = {
            { Season, 0x220, Size("cGcUnlockableSeasonReward", 0x78), Field("cGcUnlockableSeasonReward", "ID", 0x40),
              Field("cGcUnlockableSeasonReward", "ID", 0x40), F.redeemedSeason, "R:" },
            { Twitch, 0x228, Size("cGcUnlockableTwitchReward", 0x30), Field("cGcUnlockableTwitchReward", "TwitchId", 0x20),
              Field("cGcUnlockableTwitchReward", "ProductId", 0x10), F.redeemedSeason + 0x40, "W:" },
            { Platform, 0x230, Size("cGcUnlockablePlatformReward", 0x20), Field("cGcUnlockablePlatformReward", "RewardId", 0x10),
              Field("cGcUnlockablePlatformReward", "ProductId", 0x00), F.redeemedSeason + 0x80, "F:" },
        };
        for (auto& r : rs) {
            auto known = SetIds(ps + r.set, 16);
            auto [arr, n] = Array(rm + r.slot, r.keyOff);
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t rec = arr + (uintptr_t)i * r.size;
                Entry e;
                e.cat = r.cat;
                e.id = Id16(rec + r.keyOff);
                e.arg = Id16(rec + r.productOff);
                if (!Printable(e.id)) continue;
                listedSpecials.insert(e.id);
                if (Printable(e.arg)) listedSpecials.insert(e.arg);
                e.key = r.prefix + e.id;
                e.known = known.count(e.id) != 0 || (Printable(e.arg) && knownSpecials.count(e.arg) != 0);
                e.item = r.cat == Season ? e.id : Printable(e.arg) ? e.arg : "";
                FromItem(e, Printable(e.arg) ? e.arg : e.id);
                out->push_back(std::move(e));
            }
        }
    }

    // Exclusive / retired SpecialShop products (TradeCategory == 8) not listed in PurchaseableSpecials or reward tables.
    {
        uint32_t sz = Size("cGcProductData", 0x300), id = Field("cGcProductData", "ID", 0x150);
        uint32_t trade = Field("cGcProductData", "TradeCategory", 0x1E4);
        for (uint32_t slot : { 0x18u, 0x20u, 0x28u }) {
            auto [arr, n] = Array(rm + slot, id);
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t r = arr + (uintptr_t)i * sz;
                int32_t tc = -1;
                Rd(r + trade, tc);
                if (tc != 8) continue;
                std::string pid = Id16(r + id);
                if (!Printable(pid) || listedSpecials.count(pid)) continue;
                listedSpecials.insert(pid);
                Entry e;
                e.cat = Specials;
                e.id = pid;
                e.key = "S:" + e.id;
                e.known = knownSpecials.count(e.id) != 0;
                e.item = e.id; // retired shop products: the flag alone gives nothing to use
                FromItem(e, e.id);
                out->push_back(std::move(e));
            }
        }
    }

    // Titles (account wide).
    if (F.titleTable > 0 && F.userSettings > 0 && F.usTitles > 0) {
        auto known = SetIds(d + F.userSettings + F.usTitles, 16);
        uint32_t sz = Size("cGcPlayerTitle", 0xF8), id = Field("cGcPlayerTitle", "ID", 0x70);
        uint32_t title = Field("cGcPlayerTitle", "Title", 0x20), desc = Field("cGcPlayerTitle", "UnlockDescription", 0x40);
        auto [arr, n] = Array(d + F.titleTable, id);
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t r = arr + (uintptr_t)i * sz;
            Entry e;
            e.cat = Titles;
            e.id = Id16(r + id);
            if (!Printable(e.id)) continue;
            e.key = "N:" + e.id;
            e.title = game::catalog::Translate(Str(r + title, 32));
            e.body = game::catalog::Translate(Str(r + desc, 32));
            e.known = known.count(e.id) != 0;
            out->push_back(std::move(e));
        }
    }

    // Words: one entry per group and race.
    if (F.wordGroups > 0) {
        std::set<std::pair<std::string, int>> known;
        uint32_t n = 0;
        uintptr_t data = 0;
        if (Rd(ps + F.wordGroups + 4, n) && Rd(ps + F.wordGroups + 8, data) && data && n < 100000)
            for (uint32_t i = 0; i < n; ++i) {
                std::string grp = Str(data + i * 0x30, 32);
                for (int race = 0; race < 9; ++race) {
                    uint8_t f = 0;
                    if (Rd(data + i * 0x30 + 0x20 + race, f) && f) known.insert({ grp, race });
                }
            }
        uint32_t sz = Size("cGcAlienSpeechEntry", 0x68), grp = Field("cGcAlienSpeechEntry", "Group", 0x00);
        uint32_t text = Field("cGcAlienSpeechEntry", "Text", 0x20), race = Field("cGcAlienSpeechEntry", "Race", 0x5C);
        auto raceNames = meta::EnumNames("cGcAlienSpeechEntry", "Race");
        auto [arr, cnt] = Array(rm + 0xC68, grp, false);
        for (int32_t i = 0; i < cnt; ++i) {
            uintptr_t r = arr + (uintptr_t)i * sz;
            Entry e;
            e.cat = Words;
            e.arg = Str(r + grp, 32);
            Rd(r + race, e.race);
            if (!Printable(e.arg)) continue;
            e.key = "D:" + e.arg + "#" + std::to_string(e.race);
            e.title = game::catalog::Translate(Str(r + text, 32));
            for (auto& [v, nm] : raceNames) if (v == e.race) e.subtitle = nm;
            e.known = known.count({ e.arg, e.race }) != 0;
            out->push_back(std::move(e));
        }
    }

    // Portal glyphs: one bitmask.
    if (F.runes > 0) {
        int32_t bits = 0;
        Rd(d + F.runes, bits);
        Entry e;
        e.cat = Glyphs;
        e.key = "G:ALL";
        e.title = L::T("unl.glyphs");
        e.body = L::T("unl.glyphs.desc");
        e.known = (bits & 0xFFFF) == 0xFFFF;
        out->push_back(std::move(e));
    }

    // Reward items whose claim runs a reward instead.
    {
        std::unordered_map<std::string, std::string> rewards;
        uint32_t sz = Size("cGcProductData", 0x300), id = Field("cGcProductData", "ID", 0x150);
        uint32_t give = Field("cGcProductData", "GiveRewardOnSpecialPurchase", 0x130);
        for (uint32_t slot : { 0x18u, 0x20u, 0x28u }) {
            auto [arr, n] = Array(rm + slot, id);
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t r = arr + (uintptr_t)i * sz;
                std::string g = Id16(r + give);
                if (Printable(g)) rewards[Id16(r + id)] = g;
            }
        }
        for (auto& e : *out)
            if (!e.item.empty())
                if (auto it = rewards.find(e.item); it != rewards.end()) e.reward = it->second;
    }

    {
        std::lock_guard lk(g_mx);
        g_entries = std::move(out);
    }
    g_building = false;
}

void Request()
{
    if (g_building.exchange(true)) return;
    game::Post(Build);
}

// Game thread. GiveGenericReward with the arguments the game's own callers pass.
bool CallReward(const char* id)
{
    char mission[16] = {}, seed[16] = {};
    int count = 0;
    __try { return F.giveReward(F.rewardMgr, id, mission, seed, false, false, &count, false, -1, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool GiveReward(const std::string& reward)
{
    char id[16] = {};
    memcpy(id, reward.data(), std::min<size_t>(reward.size(), 16));
    bool ok = CallReward(id);
    log::Info("Unlocks", "Reward {}: {}", reward, ok ? "given" : "failed");
    return ok;
}

void Grant(std::vector<Entry> list, bool again)
{
    game::BackupSaveOnce();
    game::Post([list = std::move(list), again] {
        uintptr_t d = game::Data(), ps = game::PlayerState();
        if (!d || !ps) return;
        void* us = F.userSettings > 0 ? (void*)(d + F.userSettings) : nullptr;
        int done = 0;
        for (auto& e : list) {
            char id[16] = {};
            memcpy(id, e.id.data(), std::min<size_t>(e.id.size(), 16));
            char arg[32] = {};
            memcpy(arg, e.arg.data(), std::min<size_t>(e.arg.size(), e.cat == Words ? 32 : 16));
            bool ok = false;
            switch (e.cat) {
            case Tech: ok = F.learnTech && F.learnTech((void*)ps, (void*)e.record, false, true); break;
            case Products:
            case BaseParts: ok = F.learnProduct && F.learnProduct((void*)ps, id, true); break;
            case Exclusive:
                if (e.record) ok = F.learnTech && F.learnTech((void*)ps, (void*)e.record, false, true);
                else ok = F.learnProduct && F.learnProduct((void*)ps, id, true);
                break;
            case Specials: ok = F.learnSpecial && F.learnSpecial((void*)ps, id); break;
            case Titles: ok = F.unlockTitle && F.unlockTitle((void*)ps, id, false, false); break;
            case Words: ok = F.learnWord && F.learnWord((void*)ps, arg, e.race); break;
            case Season:
                if (F.seasonReward && us) F.seasonReward(us, id);
                ok = F.learnSpecial && F.learnSpecial((void*)ps, id);
                break;
            case Twitch:
            case Platform: // LearnSpecial also marks the reward redeemed
                ok = F.learnSpecial && F.learnSpecial((void*)ps, arg[0] ? arg : id);
                break;
            case Glyphs: {
                int32_t bits = 0;
                ok = Rd(d + F.runes, bits) && Wr<int32_t>(d + F.runes, bits | 0xFFFF);
                break;
            }
            default: break;
            }
            // The flag alone does not hand out ships or Multi-Tools. Their items do nothing in the inventory:
            // the Quicksilver claim runs the item's reward instead, so do the same.
            if (!e.item.empty() && (!e.known || again)) {
                if (!e.reward.empty() && F.giveReward && F.rewardMgr) ok = GiveReward(e.reward) || ok;
                else ok = GiveProduct(e.item) || ok;
            }
            done += ok;
        }
        // Flush the account sets the same way the game's save paths do.
        uint8_t dirty = 0;
        if (us && F.usDirty > 0 && F.saveSettings && Rd((uintptr_t)us + F.usDirty, dirty) && dirty) {
            Wr<uint8_t>((uintptr_t)us + F.usDirty, 0);
            F.saveSettings(us);
        }
        log::Info("Unlocks", "Unlocked {} of {}", done, list.size());
        Request();
    });
}

ui::BrowserEntry ToView(const Entry& e)
{
    ui::BrowserEntry b;
    b.key = e.key;
    b.title = e.title.empty() ? e.id : e.title;
    b.subtitle = std::string(L::T(kCatKeys[e.cat])) + (e.subtitle.empty() ? "" : "   /   " + e.subtitle);
    b.body = e.body;
    b.icon = e.icon;
    b.glyph = e.cat == Exclusive ? icon::Rocket : e.cat == Titles ? icon::Check : e.cat == Words ? icon::Info : e.cat == Glyphs ? icon::Planet : icon::Cubes;
    b.chip = e.cat;
    b.marked = e.known;
    Describe(!e.item.empty() ? e.item : !e.arg.empty() && e.cat != Words ? e.arg : e.id, b);
    return b;
}

void DrawList(const char* query)
{
    if (!game::PlayerState()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T("items.ingame"));
        return;
    }
    if (!game::catalog::Ready()) { game::catalog::Request(); }
    std::shared_ptr<const std::vector<Entry>> snap;
    {
        std::lock_guard lk(g_mx);
        snap = g_entries;
    }
    if (!snap) {
        if (!g.requested && game::catalog::Ready()) { Request(); g.requested = true; }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T("items.loading"));
        return;
    }
    static uint64_t prices = 0;
    if (snap != g.snap || prices != game::prices::Version()) {
        g.snap = snap;
        prices = game::prices::Version();
        g.view.clear();
        for (auto& e : *snap) g.view.push_back(ToView(e));
        g.browser.Invalidate();
    }
    g.browser.Draw(g.view, query);
}

float DrawInspector(ImVec2 a, float w)
{
    if (!g.snap) return 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ui::BrowserEntry* f = g.browser.Focused(g.view);
    ui::BrowserEntry head;
    if (f) head = *f;
    else {
        head.title = L::T("tab.unlocks");
        head.subtitle = L::T("unl.subtitle");
        head.body = L::T("unl.help");
        head.glyph = icon::Lock;
    }
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2;

    // Known entries can still be granted when they hand out an item and the switch is on.
    auto grantable = [](const Entry& e) { return !e.known || (g.again && !e.item.empty()); };
    const Entry* fe = nullptr;
    if (f) for (auto& e : *g.snap) if (e.key == f->key) { fe = &e; break; }

    // counts for the buttons
    size_t newSel = 0;
    std::unordered_set<std::string> sel(g.browser.Selected().begin(), g.browser.Selected().end());
    for (auto& e : *g.snap) if (grantable(e) && sel.count(e.key)) ++newSel;

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    bool hide = g.browser.hideMarked;
    if (ui::Switch("##hide", &hide)) { g.browser.hideMarked = hide; g.browser.Invalidate(); }
    dl->AddText(ui::theme::Body(), S(16), ImVec2(a.x + pad + S(64), y + S(3)), P().textBody, L::T("unl.hideknown"));
    y += S(40);
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    ui::Switch("##again", &g.again);
    dl->AddText(ui::theme::Body(), S(16), ImVec2(a.x + pad + S(64), y + S(3)), P().textBody, L::T("unl.again"));
    y += S(40);

    bool live = game::FrameHookReady();
    auto pick = [&](bool selectedOnly) {
        std::vector<Entry> list;
        for (auto& e : *g.snap) {
            if (!grantable(e)) continue;
            if (selectedOnly ? sel.count(e.key) != 0 : (f && e.key == f->key)) list.push_back(e);
        }
        return list;
    };
    if (fe && grantable(*fe)) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##one", L::T("unl.unlock"), bw, ui::Tone::Green, live)) { Grant(pick(false), g.again); ui::Toast(L::Tf("unl.done", "1"), "", icon::Check); }
        y += S(46);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    if (ui::Button("##selall", L::T("unl.selectall"), bw, ui::Tone::Neutral, true)) g.browser.SelectVisible();
    y += S(46);
    if (newSel) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        std::string lbl = L::Tf("unl.unlocksel", std::to_string(newSel));
        if (ui::HoldButton("##many", lbl.c_str(), bw)) {
            auto list = pick(true);
            Grant(list, g.again);
            g.browser.ClearSelection();
            ui::Toast(L::Tf("unl.done", std::to_string(list.size())), "", icon::Check);
        }
        y += S(46);
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##clear", L::T("unl.clear"), bw, ui::Tone::Neutral, true)) g.browser.ClearSelection();
        y += S(46);
    }
    dl->AddText(ui::theme::Body(), S(14), ImVec2(a.x + pad, y), P().textFaint, L::T("unl.note"), nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, L::T("unl.note")).y + pad;
    return y;
}

// Game thread: sets or clears Core on the engine records in the live tech table.
void SetEngineCore(bool core)
{
    uintptr_t rm = game::Reality();
    if (!rm) return;
    uint32_t sz = Size("cGcTechnology", 0x2E0), id = Field("cGcTechnology", "ID", 0x108), flag = Field("cGcTechnology", "Core", 0x2C7);
    auto [arr, n] = Array(rm + 0x10, id);
    int done = 0;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t r = arr + (uintptr_t)i * sz;
        std::string tid = Id16(r + id);
        for (const char* e : kEngines) if (tid == e) done += Wr<uint8_t>(r + flag, core ? 1 : 0);
    }
    log::Info("Unlocks", "Engine core flag {} on {} records", core, done);
}

}

void InstallUnlocks()
{
    Bind("PlayerState.LearnTech", F.learnTech);
    Bind("PlayerState.LearnProduct", F.learnProduct);
    Bind("PlayerState.LearnSpecial", F.learnSpecial);
    Bind("PlayerState.UnlockTitle", F.unlockTitle);
    Bind("PlayerState.LearnWordGroup", F.learnWord);
    Bind("UserSettings.UnlockSeasonReward", F.seasonReward);
    Bind("UserSettings.Save", F.saveSettings);
    Bind("Reward.GiveGenericReward", F.giveReward);
    if (const Target& m = Loc().Resolve("Data.RewardManager"); m.ok && m.global) F.rewardMgr = (void*)m.global;
    else log::Warn("Unlocks", "Reward manager missing, ship rewards hand out their item");
    F.knownTech = Off("Data.KnownTech");
    F.knownProducts = Off("Data.KnownProducts");
    F.knownSpecials = Off("Data.KnownSpecials");
    F.redeemedSeason = Off("Data.RedeemedSeasonRewards");
    F.wordGroups = Off("Data.KnownWordGroups");
    F.userSettings = Off("Data.UserSettings");
    F.usTitles = Off("Data.UserSettingsTitles");
    F.usDirty = Off("Data.UserSettingsDirty");
    F.titleTable = Off("Data.TitleTable");
    F.runes = Off("Data.KnownPortalRunes");
    {
        Bound b;
        b.id = "ship.core_swap";
        b.toggle = [](bool on) {
            if (!game::FrameHookReady()) return false;
            game::Post([on] { SetEngineCore(!on); });
            return true;
        };
        Ok(std::move(b));
    }
    log::Info("Unlocks", "Ready: tech {}, products {}, specials {}, titles {}, words {}", F.learnTech != nullptr, F.learnProduct != nullptr,
              F.learnSpecial != nullptr, F.unlockTitle != nullptr, F.learnWord != nullptr);

    Registry::Get().Post([] {
        for (int c = 0; c < CatCount; ++c) g.browser.chips.push_back(L::T(kCatKeys[c]));
        g.browser.multi = true;
        g.browser.sortable = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] {
            if (!g.snap) return std::string();
            size_t known = 0;
            for (auto& e : *g.snap) known += e.known;
            return "(" + std::to_string(known) + "/" + std::to_string(g.snap->size()) + ")";
        };
        ui::RegisterPage("unlocks", std::move(page));
    });
}

}
