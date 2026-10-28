#include "features/WorldFx.h"
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
#undef LoadIcon // windows.h renames it to LoadIconW
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <type_traits>

// Storage tab: every container the player owns in one place. Find an item,
// send items between containers at any distance, and sort with the game's own sort.
namespace os::features {

namespace {

namespace meta = hooks::meta;
using ui::theme::P;
using ui::theme::S;

using StoreFn = uintptr_t (*)(uintptr_t inv, int32_t choice, int32_t index);
using AddFn = void (*)(uintptr_t store, int32_t* out, const void* elem);
using RemoveFn = bool (*)(uintptr_t store, uint64_t index, int32_t amount);
using SortFn = void (*)(uintptr_t store, int32_t mode);
using NotifyFn = void (*)(uintptr_t inv, const void* what);
StoreFn g_store = nullptr;
AddFn g_add = nullptr;
RemoveFn g_remove = nullptr;
SortFn g_sort = nullptr;
NotifyFn g_notify = nullptr;

constexpr uint32_t kInv = 0x900; // PlayerState block the store calls take as `this`
constexpr uint32_t kShipNames = 0x1AD93, kNameLen = 0x20; // as in Garage

// InventoryChoice values, from the game's reflected enum
enum Choice
{
    Personal = 0, PersonalCargo = 2, Ship = 4, ShipTech = 5, ShipCargo = 6, Freighter = 7, FreighterTech = 8, FreighterCargo = 9,
    Vehicle = 10, VehicleTech = 11, Chest1 = 12, ChestMagic = 22, ChestMagic2 = 23, Cooking = 26, RocketLocker = 27,
    FishPlatform = 29, FishBait = 30, FoodUnit = 31, CorvetteParts = 32
};

bool Indexed(int c) { return c == Ship || c == ShipTech || c == ShipCargo || c == Vehicle || c == VehicleTech; }

// cGcInventoryElement, offsets from the reflection data when it has them
struct
{
    uint32_t size = 0x30, id = 0x00, index = 0x10, amount = 0x18, max = 0x20, type = 0x24;
    void Load()
    {
        auto get = [](const char* f, uint32_t& out) { if (auto m = meta::Path("cGcInventoryElement", f)) out = m->offset; };
        if (auto* ci = meta::Get("cGcInventoryElement"); ci && ci->size >= 0x28 && ci->size <= 0x100) size = ci->size;
        get("Id", id); get("Index", index); get("Amount", amount); get("MaxAmount", max); get("Type", type);
    }
} E;

enum Type { Substance = 0, Technology = 1, Product = 2 };

struct Stack
{
    std::string id;
    int32_t x = 0, y = 0, amount = 0, max = 0, type = 0;
};

struct Box
{
    int choice = 0, index = -1;
    std::string label;
    int slots = 0;
    std::vector<Stack> stacks;
    bool Is(int c, int i) const { return choice == c && index == i; }
};

std::mutex g_mx;
std::vector<Box> g_boxes; // under g_mx
std::atomic<uint64_t> g_version{ 0 };
std::atomic<bool> g_wanted{ false }, g_dirty{ true };

struct Result { uint64_t seq = 0; std::string title, body; bool ok = true; };
Result g_result; // under g_mx

void Report(bool ok, std::string title, std::string body = "")
{
    std::lock_guard lk(g_mx);
    g_result = { g_result.seq + 1, std::move(title), std::move(body), ok };
}

// ---- game calls, guarded ----

uintptr_t CallStore(uintptr_t inv, int choice, int index)
{
    __try { return g_store(inv, choice, index); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool CallAdd(uintptr_t store, int32_t* out, const void* elem)
{
    __try { g_add(store, out, elem); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallRemove(uintptr_t store, uint64_t index, int32_t amount)
{
    __try { return g_remove(store, index, amount); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallSort(uintptr_t store, int32_t mode)
{
    __try { g_sort(store, mode); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void CallNotify(uintptr_t inv, const void* what)
{
    __try { g_notify(inv, what); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- reading stores (game thread) ----

uintptr_t StoreOf(int choice, int index)
{
    uintptr_t ps = game::PlayerState();
    return ps && g_store ? CallStore(ps + kInv, choice, index) : 0;
}

void Notify(int choice, int index)
{
    uintptr_t ps = game::PlayerState();
    if (!ps || !g_notify) return;
    struct { uint64_t zero; int32_t choice; int32_t index; } what{ 0, choice, Indexed(choice) ? index : -1 };
    CallNotify(ps + kInv, &what);
}

// Valid slots from the layout bits: one u64 row per Y, bit X.
int SlotCount(uintptr_t s)
{
    int16_t w = 0, h = 0;
    if (!s || !Rd(s + 0x80, w) || !Rd(s + 0x82, h) || w < 1 || w > 64 || h < 1 || h > 16) return 0;
    int n = 0;
    for (int y = 0; y < h; ++y) {
        uint64_t row = 0;
        if (!Rd(s + y * 8, row)) return 0;
        if (w < 64) row &= (1ull << w) - 1;
        n += std::popcount(row);
    }
    return n;
}

uint32_t ElemCount(uintptr_t s)
{
    uint32_t n = 0;
    return s && Rd(s + 0x8C, n) && n <= 4096 ? n : 0;
}

uintptr_t ElemAt(uintptr_t s, uint32_t i)
{
    uintptr_t data = 0;
    return Rd(s + 0x90, data) && data ? data + (uintptr_t)i * E.size : 0;
}

std::string ReadFixed(uintptr_t at, size_t max)
{
    std::string s;
    for (char c; s.size() < max && Rd(at + s.size(), c) && c;) s += c;
    return s;
}

bool ReadStack(uintptr_t e, Stack& st)
{
    st.id = ReadFixed(e + E.id, 16);
    return !st.id.empty() && Rd(e + E.index, st.x) && Rd(e + E.index + 4, st.y) && Rd(e + E.amount, st.amount) && Rd(e + E.max, st.max)
        && Rd(e + E.type, st.type);
}

void ReadStacks(uintptr_t s, std::vector<Stack>& out)
{
    uint32_t n = ElemCount(s);
    for (uint32_t i = 0; i < n; ++i) {
        Stack st;
        if (uintptr_t e = ElemAt(s, i); e && ReadStack(e, st)) out.push_back(std::move(st));
    }
}

void Consider(std::vector<Box>& out, int choice, int index, std::string label, int techChoice = -1)
{
    uintptr_t s = StoreOf(choice, index);
    Box b{ choice, index, std::move(label), SlotCount(s), {} };
    if (b.slots <= 0) return;
    // ships, exocraft and the freighter are owned when their tech store holds something
    if (techChoice >= 0 && !ElemCount(StoreOf(techChoice, index))) return;
    ReadStacks(s, b.stacks);
    out.push_back(std::move(b));
}

void Snapshot()
{
    uintptr_t ps = game::PlayerState();
    std::vector<Box> boxes;
    if (ps && g_store) {
        Consider(boxes, Personal, -1, L::T("store.suit"));
        Consider(boxes, PersonalCargo, -1, L::T("store.suitcargo"));
        for (int i = 0; i < 12; ++i) {
            std::string name = ReadFixed(ps + kShipNames + i * kNameLen, kNameLen);
            if (name.empty()) name = L::Tf("store.ship", std::to_string(i + 1));
            Consider(boxes, Ship, i, name, ShipTech);
        }
        Consider(boxes, Freighter, -1, L::T("store.freighter"), FreighterTech);
        Consider(boxes, FreighterCargo, -1, L::T("store.freightercargo"), FreighterTech);
        for (int i = 0; i < 10; ++i) Consider(boxes, Chest1 + i, -1, L::Tf("store.chest", std::to_string(i)));
        for (int i = 0; i < 7; ++i) Consider(boxes, Vehicle, i, L::T(("store.exo." + std::to_string(i)).c_str()), VehicleTech);
        Consider(boxes, ChestMagic, -1, L::T("store.magic1"));
        Consider(boxes, ChestMagic2, -1, L::T("store.magic2"));
        Consider(boxes, Cooking, -1, L::T("store.cooking"));
        Consider(boxes, RocketLocker, -1, L::T("store.rocket"));
        Consider(boxes, FishPlatform, -1, L::T("store.fishplatform"));
        Consider(boxes, FishBait, -1, L::T("store.fishbait"));
        Consider(boxes, FoodUnit, -1, L::T("store.food"));
        Consider(boxes, CorvetteParts, -1, L::T("store.corvette"));
    }
    std::lock_guard lk(g_mx);
    g_boxes = std::move(boxes);
    ++g_version;
}

// ---- actions (game thread) ----

bool Same(const Stack& s, const std::string& id, int type) { return s.type == type && s.id == id; }

// Sends up to `amount` of one item. Fills part stacks in the target first, then adds new
// stacks with the game's own Add, and takes what landed from the smallest source stacks.
int Move(int sc, int si, int dc, int di, const std::string& id, int type, int amount)
{
    uintptr_t src = StoreOf(sc, si), dst = StoreOf(dc, di);
    if (!src || !dst || src == dst || type == Technology || amount <= 0) return 0;

    std::vector<Stack> from;
    std::vector<uint8_t> tmpl;
    int have = 0;
    for (uint32_t i = 0, n = ElemCount(src); i < n; ++i) {
        uintptr_t e = ElemAt(src, i);
        Stack st;
        if (!e || !ReadStack(e, st) || !Same(st, id, type) || st.amount <= 0) continue;
        if (tmpl.empty()) {
            tmpl.resize(E.size);
            for (uint32_t k = 0; k < E.size; ++k) Rd(e + k, tmpl[k]);
        }
        have += st.amount;
        from.push_back(st);
    }
    int left = std::min(amount, have);
    if (!left) return 0;
    int want = left;

    for (uint32_t i = 0, n = ElemCount(dst); i < n && left > 0; ++i) {
        uintptr_t e = ElemAt(dst, i);
        Stack st;
        if (!e || !ReadStack(e, st) || !Same(st, id, type) || st.amount >= st.max) continue;
        int add = std::min(left, st.max - st.amount);
        if (Wr<int32_t>(e + E.amount, st.amount + add)) left -= add;
    }
    for (int guard = 0; left > 0 && guard < 500; ++guard) {
        std::vector<uint8_t> el = tmpl;
        int32_t none[2] = { -1, -1 };
        memcpy(el.data() + E.index, none, 8);
        memcpy(el.data() + E.amount, &left, 4);
        int32_t out[2] = { -1, -1 };
        if (!CallAdd(dst, out, el.data()) || (out[0] < 0 && out[1] < 0)) break; // full
        // Add sets the target's stack cap; split anything past it into the next stack
        int placed = left;
        for (uint32_t i = 0, n = ElemCount(dst); i < n; ++i) {
            uintptr_t e = ElemAt(dst, i);
            Stack st;
            if (!e || !ReadStack(e, st) || st.x != out[0] || st.y != out[1]) continue;
            if (st.max > 0 && st.amount > st.max) {
                Wr<int32_t>(e + E.amount, st.max);
                placed = st.max;
            }
            break;
        }
        left -= placed;
    }
    int moved = want - left;

    std::sort(from.begin(), from.end(), [](const Stack& a, const Stack& b) { return a.amount < b.amount; });
    int take = moved;
    for (auto& st : from) {
        if (take <= 0) break;
        int t = std::min(take, st.amount);
        uint64_t idx = (uint32_t)st.x | (uint64_t)(uint32_t)st.y << 32;
        if (CallRemove(src, idx, t)) take -= t;
    }
    if (take > 0) log::Warn("Storage", "Move {}: {} could not be taken from the source", id, take);
    Notify(sc, si);
    Notify(dc, di);
    log::Info("Storage", "Moved {} x{} from {}:{} to {}:{} (asked {}, had {})", id, moved, sc, si, dc, di, amount, have);
    return moved;
}

// ---- the page (UI thread) ----

struct
{
    ui::Browser browser;
    std::vector<ui::BrowserEntry> view;
    std::vector<Box> boxes;
    uint64_t version = ~0ull, resultSeq = 0;
    int filter = 0;     // 0 all, k = boxes[k - 1]
    int fc = -2, fi = -2; // the filtered box, kept across rebuilds
    int src = -1;       // source box for the focused item
    std::string focusKey;
    float amount = 1;
    int dc = -2, di = -2; // destination box
    int sortMode = 0;   // 0 value, 1 type, 2 name, 3 colour
    bool requested = false;
} g;

std::string Units(long long v)
{
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

std::string Short(long long v)
{
    if (v < 10000) return std::to_string(v);
    if (v < 1000000) return std::to_string(v / 1000) + "k";
    return std::to_string(v / 1000000) + "m";
}

std::string BaseId(const std::string& id)
{
    size_t h = id.find('#'); // procedural items carry a seed
    return h == std::string::npos ? id : id.substr(0, h);
}

std::string KeyOf(const Stack& s) { return std::to_string(s.type) + "|" + s.id; }

int Total(const Box& b, const std::string& id, int type)
{
    int n = 0;
    for (auto& s : b.stacks)
        if (Same(s, id, type)) n += s.amount;
    return n;
}

const Box* Filtered()
{
    return g.filter > 0 && g.filter <= (int)g.boxes.size() ? &g.boxes[g.filter - 1] : nullptr;
}

void Rebuild()
{
    {
        std::lock_guard lk(g_mx);
        g.boxes = g_boxes;
    }
    // keep the filter on the same box when the list shifts
    g.filter = 0;
    for (size_t k = 0; k < g.boxes.size(); ++k)
        if (g.boxes[k].Is(g.fc, g.fi)) g.filter = (int)k + 1;

    struct Agg { Stack first; long long total = 0; std::vector<std::string> where; };
    std::map<std::string, Agg> items;
    for (size_t k = 0; k < g.boxes.size(); ++k) {
        if (g.filter && (int)k != g.filter - 1) continue;
        const Box& b = g.boxes[k];
        for (auto& s : b.stacks) {
            if (s.type == Technology || s.amount <= 0) continue;
            Agg& a = items[KeyOf(s)];
            if (!a.total) a.first = s;
            a.total += s.amount;
            if (a.where.empty() || a.where.back() != b.label) a.where.push_back(b.label);
        }
    }
    g.view.clear();
    bool names = game::catalog::Ready();
    for (auto& [key, a] : items) {
        ui::BrowserEntry e;
        e.key = key;
        const game::Item* it = names ? game::catalog::Find(BaseId(a.first.id)) : nullptr;
        e.title = it && !it->name.empty() ? it->name : a.first.id;
        e.icon = it ? it->icon : "";
        e.body = it ? it->desc : "";
        e.glyph = icon::Cubes;
        e.chip = a.first.type == Substance ? 1 : 2;
        e.count = Short(a.total);
        if (it) Describe(it->id, e);
        std::string where;
        for (auto& w : a.where) where += (where.empty() ? "" : ", ") + w;
        e.subtitle = "x " + Units(a.total) + "   /   " + where;
        g.view.push_back(std::move(e));
    }
    g.browser.Invalidate();
}

bool Live() { return game::FrameHookReady() && game::PlayerState() && g_store; }

void DrawList(const char* query)
{
    g_wanted = true;
    if (!game::catalog::Ready() && !game::catalog::Building() && game::PlayerState() && !g.requested) {
        game::catalog::Request();
        g.requested = true;
    }
    static bool hadNames = false;
    if (g_version != g.version || hadNames != game::catalog::Ready()) {
        g.version = g_version;
        hadNames = game::catalog::Ready();
        Rebuild();
    }
    {
        std::lock_guard lk(g_mx);
        if (g_result.seq != g.resultSeq) {
            g.resultSeq = g_result.seq;
            ui::Toast(g_result.title, g_result.body, g_result.ok ? icon::Check : icon::Warning);
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x - S(8);
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!Live()) {
        dl->AddText(ui::theme::Body(), S(15), p, P().textFaint, L::T(g_store ? "store.ingame" : "store.missing"));
        ImGui::Dummy(ImVec2(w, S(24)));
        return;
    }
    // container picker: the grid shows one container, or everything
    std::vector<std::string> names{ L::T("store.all") };
    for (auto& b : g.boxes) {
        int used = 0;
        for (auto& s : b.stacks) used += s.type != Technology;
        names.push_back(b.label + "   " + std::to_string(used) + " / " + std::to_string(b.slots));
    }
    int before = g.filter;
    ui::Dropdown("##storebox", names, &g.filter, w);
    if (g.filter != before) {
        const Box* b = Filtered();
        g.fc = b ? b->choice : -2;
        g.fi = b ? b->index : -2;
        g.browser.ClearFocus();
        Rebuild();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(46)));
    if (g.view.empty()) {
        dl->AddText(ui::theme::Body(), S(15), ImGui::GetCursorScreenPos(), P().textFaint, L::T(g.boxes.empty() ? "store.loading" : "store.empty"));
        ImGui::Dummy(ImVec2(w, S(24)));
        return;
    }
    g.browser.Draw(g.view, query);
}

void Fact(ImDrawList* dl, float x, float& y, float w, const std::string& k, const std::string& v)
{
    if (v.empty()) return;
    y += ui::RichText(dl, ui::theme::Body(), S(15), ImVec2(x, y), w, P().textBody, "<TECHNOLOGY>" + k + "<> " + v) + S(4);
}

// Destination dropdown over every box; returns the chosen one or nullptr.
const Box* DestPicker(const char* id, float x, float& y, float bw, const Box* src)
{
    std::vector<std::string> names;
    int cur = -1;
    for (size_t k = 0; k < g.boxes.size(); ++k) {
        names.push_back(g.boxes[k].label);
        if (g.boxes[k].Is(g.dc, g.di)) cur = (int)k;
    }
    if (cur < 0) // default: the first box that isn't the source
        for (size_t k = 0; k < g.boxes.size() && cur < 0; ++k)
            if (&g.boxes[k] != src) cur = (int)k;
    if (cur < 0) return nullptr;
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::Dropdown(id, names, &cur, bw);
    g.dc = g.boxes[cur].choice;
    g.di = g.boxes[cur].index;
    y += S(42);
    return &g.boxes[cur];
}

const int kSortModes[4] = { 1, 2, 3, 4 }; // the game's value, type, name, colour

std::vector<std::string>& SortNames()
{
    static std::vector<std::string> v;
    if (v.empty()) v = { L::T("store.sort.value"), L::T("store.sort.type"), L::T("store.sort.name"), L::T("store.sort.colour") };
    return v;
}

void Label(ImDrawList* dl, float x, float& y, const char* key)
{
    dl->AddText(ui::theme::Body(), S(16), ImVec2(x, y), P().textDim, L::T(key));
    y += S(24);
}

float DrawItem(ImDrawList* dl, ImVec2 a, float w, const ui::BrowserEntry& f)
{
    float y = ui::DrawEntryHeader(dl, a, w, f);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    size_t bar = f.key.find('|');
    int type = std::atoi(f.key.c_str());
    std::string id = f.key.substr(bar + 1);

    // where it is; click a row to send from there
    std::vector<int> holders;
    for (size_t k = 0; k < g.boxes.size(); ++k)
        if (Total(g.boxes[k], id, type) > 0) holders.push_back((int)k);
    if (holders.empty()) return y;
    if (f.key != g.focusKey) {
        g.focusKey = f.key;
        g.src = Filtered() ? g.filter - 1 : holders.front();
        g.amount = 0;
    }
    if (std::find(holders.begin(), holders.end(), g.src) == holders.end()) g.src = holders.front();
    Label(dl, x, y, "store.where");
    for (int k : holders) {
        const Box& b = g.boxes[k];
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(k);
        if (ImGui::InvisibleButton("##src", ImVec2(bw, S(28))) && g.src != k) { g.src = k; g.amount = 0; }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = g.src == k;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bw, y + S(28)), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 90), S(4));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(10), y + S(5)), on || hov ? P().text : P().textDim, b.label.c_str());
        std::string n = Units(Total(b, id, type));
        float tw = ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, 0, n.c_str()).x;
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + bw - tw - S(10), y + S(5)), P().text, n.c_str());
        y += S(32);
    }
    y += S(8);
    const Box& src = g.boxes[g.src];
    int have = Total(src, id, type);
    if (g.amount <= 0 || g.amount > have) g.amount = (float)have;
    if (have > 1) {
        Label(dl, x, y, "store.amount");
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ui::EnergyBar("##storeamt", &g.amount, 1, (float)have, "%.0f", bw);
        y += S(36);
    }
    Label(dl, x, y, "store.to");
    const Box* dst = DestPicker("##storeto", x, y, bw, &src);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool can = Live() && g_add && g_remove && dst && dst != &src;
    if (ui::Button("##storesend", L::T(dst == &src ? "store.samebox" : "store.send"), bw, ui::Tone::Green, can)) {
        int amt = std::clamp((int)std::lround(g.amount), 1, have);
        int sc = src.choice, si = src.index, dc = dst->choice, di = dst->index;
        std::string title = f.title, to = dst->label;
        game::BackupSaveOnce();
        game::Post([=] {
            int moved = Move(sc, si, dc, di, id, type, amt);
            if (moved == amt) Report(true, L::Tf("store.sent", Units(moved), title), to);
            else if (moved > 0) Report(false, L::Tf("store.sentpart", Units(moved), Units(amt)), title + ". " + L::Tf("store.full", to));
            else Report(false, L::T("store.nothing"), L::Tf("store.full", to));
            g_dirty = true;
        });
        g.amount = 0;
    }
    y += S(46);
    return y;
}

float DrawBox(ImDrawList* dl, ImVec2 a, float w)
{
    const Box* box = Filtered();
    ui::BrowserEntry head;
    head.title = box ? box->label : L::T("tab.storage");
    head.subtitle = L::T("store.subtitle");
    head.body = L::T("store.help");
    head.glyph = icon::Dolly;
    float y = ui::DrawEntryHeader(dl, a, w, head);
    float pad = S(22), bw = w - pad * 2, x = a.x + pad;
    bool live = Live();

    if (box) {
        int used = 0, kinds = 0;
        long long items = 0;
        for (auto& s : box->stacks) {
            if (s.type == Technology) continue;
            ++used;
            items += s.amount;
        }
        kinds = (int)g.view.size();
        Fact(dl, x, y, bw, L::T("store.f.slots"), std::to_string(used) + " / " + std::to_string(box->slots));
        Fact(dl, x, y, bw, L::T("store.f.items"), L::Tf("store.f.itemsv", Units(items), std::to_string(kinds)));
        y += S(8);
    }

    Label(dl, x, y, "store.sortby");
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    ui::Dropdown("##storesort", SortNames(), &g.sortMode, bw);
    y += S(42);
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    if (ui::Button("##storesortgo", L::T(box ? "store.sort" : "store.sortall"), bw, ui::Tone::Teal, live && g_sort && !g.boxes.empty())) {
        std::vector<std::pair<int, int>> which;
        for (auto& b : g.boxes)
            if (!box || &b == box) which.push_back({ b.choice, b.index });
        int mode = kSortModes[std::clamp(g.sortMode, 0, 3)];
        std::string label = box ? box->label : L::T("store.everything");
        game::BackupSaveOnce();
        game::Post([=] {
            int n = 0;
            for (auto [c, i] : which) {
                uintptr_t s = StoreOf(c, i);
                if (s && CallSort(s, mode)) { Notify(c, i); ++n; }
            }
            log::Info("Storage", "Sorted {} containers, mode {}", n, mode);
            Report(n > 0, L::T(n > 0 ? "store.sorted" : "store.sortfail"), label);
            g_dirty = true;
        });
    }
    y += S(46);

    if (box) {
        dl->AddLine(ImVec2(x, y + S(2)), ImVec2(x + bw, y + S(2)), P().line, S(1));
        y += S(12);
        Label(dl, x, y, "store.moveall");
        const Box* dst = DestPicker("##storeallto", x, y, bw, box);
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        bool can = live && g_add && g_remove && dst && dst != box && !g.view.empty();
        if (dst == box) ui::Button("##storeallsame", L::T("store.samebox"), bw, ui::Tone::Neutral, false);
        else if (ui::HoldButton("##storeallgo", L::T("store.moveallgo"), bw) && can) {
            std::vector<std::pair<std::string, int>> what; // id, type
            for (auto& s : box->stacks)
                if (s.type != Technology && std::find(what.begin(), what.end(), std::pair{ s.id, s.type }) == what.end()) what.push_back({ s.id, s.type });
            int sc = box->choice, si = box->index, dc = dst->choice, di = dst->index;
            std::string from = box->label, to = dst->label;
            game::BackupSaveOnce();
            game::Post([=] {
                int kinds = 0, missed = 0;
                for (auto& [id, type] : what) {
                    uintptr_t s = StoreOf(sc, si);
                    int have = 0;
                    for (uint32_t i = 0, n = ElemCount(s); i < n; ++i) {
                        Stack st;
                        if (uintptr_t e = ElemAt(s, i); e && ReadStack(e, st) && Same(st, id, type)) have += st.amount;
                    }
                    int moved = have ? Move(sc, si, dc, di, id, type, have) : 0;
                    kinds += moved > 0;
                    missed += moved < have;
                }
                if (!missed) Report(true, L::Tf("store.movedall", from), to);
                else Report(false, L::Tf("store.movedsome", std::to_string(kinds), std::to_string(kinds + missed)), L::Tf("store.full", to));
                g_dirty = true;
            });
        }
        y += S(46);
    }
    const char* note = L::T("store.note");
    dl->AddText(ui::theme::Body(), S(14), ImVec2(x, y), P().textFaint, note, nullptr, bw);
    y += ui::theme::Body()->CalcTextSizeA(S(14), FLT_MAX, bw, note).y + pad;
    return y;
}

float DrawInspector(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (const ui::BrowserEntry* f = g.browser.Focused(g.view)) return DrawItem(dl, a, w, *f);
    g.focusKey.clear();
    return DrawBox(dl, a, w);
}

// Reads the stores while the page is open, and right after a change.
void Tick()
{
    static uint64_t last = 0;
    uint64_t now = game::Frame();
    bool dirty = g_dirty.exchange(false);
    if (!dirty && (!g_wanted || now - last < 30)) return;
    g_wanted = false;
    last = now;
    Snapshot();
}

}

void InstallStorage()
{
    E.Load();
    auto bind = [](const char* name, auto& fn) {
        if (const Target& t = Loc().Resolve(name); t.ok) fn = (std::remove_reference_t<decltype(fn)>)Abs(t.func);
        else log::Warn("Storage", "{} missing: {}", name, t.reason);
    };
    bind("Inventory.StoreFromChoice", g_store);
    bind("Inventory.Add", g_add);
    bind("Inventory.Remove", g_remove);
    bind("Inventory.Sort", g_sort);
    bind("Inventory.Notify", g_notify);
    log::Info("Storage", "Element 0x{:X} bytes; send {}, sort {}", E.size, g_store && g_add && g_remove ? "ready" : "unavailable",
              g_sort ? "ready" : "unavailable");
    game::EveryFrame(Tick);

    Registry::Get().Post([] {
        g.browser.chips = { L::T("store.chip.all"), L::T("store.chip.substances"), L::T("store.chip.products") };
        g.browser.searchBox = true;
        g.browser.sortable = true;
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { return g.boxes.empty() ? std::string() : "(" + std::to_string(g.boxes.size()) + ")"; };
        ui::RegisterPage("storage", std::move(page));
    });
}

}
