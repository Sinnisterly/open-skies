#include "features/ItemsPage.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/Catalog.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include "render/Textures.h"
#include "ui/Browser.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>

namespace os::features {

namespace {

namespace meta = hooks::meta;
using game::ItemKind;
using ui::theme::P;
using ui::theme::S;

// cGcPlayerState inventories (NMS.py layout, stride confirmed in code at the TECH_ALREADY_INSTALLED handler).
constexpr uint32_t kSuitInventories = 0x910, kShipInventories = 0x7458, kShipTech = 0xAB28, kPrimaryShip = 0x182A0, kStride = 0x248;

enum Dest { Suit, SuitCargo, SuitTech, Ship, ShipTech, Weapon, DestCount };
const char* kDestKeys[DestCount] = { "items.dest.suit", "items.dest.cargo", "items.dest.suittech", "items.dest.ship", "items.dest.shiptech", "items.dest.weapon" };

using AddFn = void (*)(void* store, int32_t* outIndex, const void* element);
using PlaceObjFn = void* (*)(void* mgr, const void* id16, const uint16_t* baseIdx, const void* xf, void* extra, bool a5, float a6, bool a7);
AddFn g_add = nullptr;
PlaceObjFn g_placeObj = nullptr;
uintptr_t g_baseMgr = 0;
int64_t g_playerOff = 0x72CDE0, g_baseList = 0x31AE60; // Player.InstanceOffset, Base.List (count 4 bytes before)

struct State
{
    ui::Browser browser;
    std::shared_ptr<const std::vector<game::Item>> snap;
    std::vector<ui::BrowserEntry> entries;
    float amount = 1;
    int dest = Suit;
    bool requested = false;
} g;

int ChipOf(const game::Item& it)
{
    switch (it.kind) {
    case ItemKind::Substance: return 1;
    case ItemKind::Technology: return 3;
    case ItemKind::BasePart: return 4;
    case ItemKind::Customisation: return 5;
    default: return 2;
    }
}

// "BuildingPart" -> "Building Part"
std::string Spaced(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && isupper((unsigned char)s[i]) && islower((unsigned char)s[i - 1])) o += ' ';
        o += s[i] == '_' ? ' ' : s[i];
    }
    return o;
}

// Readable enum value of a table field, cached per class and field.
const std::string& EnumText(const char* cls, const char* field, int v)
{
    static std::map<std::string, std::map<int, std::string>> cache;
    static const std::string none;
    auto& m = cache[std::string(cls) + "." + field];
    if (m.empty()) {
        for (auto& [k, n] : meta::EnumNames(cls, field)) m[k] = Spaced(n);
        if (m.empty()) m[INT_MIN] = ""; // nothing to load, don't look again
    }
    auto it = m.find(v);
    return it == m.end() ? none : it->second;
}

const char* ClassOf(ItemKind k)
{
    return k == ItemKind::Substance ? "cGcRealitySubstanceData" : k == ItemKind::Technology ? "cGcTechnology" : "cGcProductData";
}

std::string Units(double v)
{
    std::string s = std::to_string((long long)v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

void Rebuild()
{
    g.entries.clear();
    if (!g.snap) return;
    for (auto& it : *g.snap) {
        if (it.name.empty() || it.name == it.id) continue;
        ui::BrowserEntry e;
        e.key = it.id;
        e.title = it.name;
        e.body = it.desc;
        e.icon = it.icon;
        e.glyph = icon::Cubes;
        e.chip = ChipOf(it);
        Describe(it.id, e);
        e.subtitle = (e.group.empty() ? std::string(L::T(ChipOf(it) == 1 ? "items.kind.substance" : ChipOf(it) == 3 ? "items.kind.tech" : "items.kind.product"))
                                      : e.group) + "   /   " + it.id;
        g.entries.push_back(std::move(e));
    }
    g.browser.Invalidate();
}

uintptr_t Store(int dest)
{
    uintptr_t ps = game::PlayerState();
    if (!ps) return 0;
    uint32_t ship = 0;
    Rd(ps + kPrimaryShip, ship);
    if (ship > 11) ship = 0;
    uintptr_t s = 0;
    switch (dest) {
    case Suit: s = ps + kSuitInventories; break;
    case SuitTech: s = ps + kSuitInventories + 1 * kStride; break;
    case SuitCargo: s = ps + kSuitInventories + 2 * kStride; break;
    case Weapon: s = ps + kSuitInventories + 3 * kStride; break;
    case Ship: s = ps + kShipInventories + ship * kStride; break;
    case ShipTech: s = ps + kShipTech + ship * kStride; break;
    }
    int16_t w = 0, h = 0;
    if (!Rd(s + 0x80, w) || !Rd(s + 0x82, h) || w < 1 || w > 16 || h < 1 || h > 16) return 0; // not a live store
    return s;
}

struct ElemLayout
{
    uint32_t size = 0x30, id = 0x00, index = 0x10, amount = 0x18, damage = 0x1C, maxAmount = 0x20, type = 0x24, installed = 0, autoAdded = 0;
    bool hasFlags = false;
    void Load()
    {
        auto get = [](const char* f, uint32_t& out) { if (auto m = meta::Path("cGcInventoryElement", f)) out = m->offset; };
        if (auto* ci = meta::Get("cGcInventoryElement")) size = ci->size;
        get("Id", id); get("Index", index); get("Amount", amount); get("DamageFactor", damage); get("MaxAmount", maxAmount); get("Type", type);
        hasFlags = meta::Path("cGcInventoryElement", "FullyInstalled").has_value();
        get("FullyInstalled", installed); get("AddedAutomatically", autoAdded);
    }
};
ElemLayout g_el;

// cGcTechnology.ChargeAmount for a tech id, 0 if it has no charge. Game thread.
int32_t TechCharge(const std::string& id)
{
    auto cm = meta::Path("cGcTechnology", "ChargeAmount");
    auto im = meta::Path("cGcTechnology", "ID");
    auto* ci = meta::Get("cGcTechnology");
    uintptr_t rm = game::Reality(), obj = 0, arr = 0;
    int32_t n = 0;
    if (!cm || !im || !ci || !ci->size || !rm || !Rd(rm + 0x10, obj) || !obj || !Rd(obj, arr) || !Rd(obj + 8, n) || !arr || n <= 0 || n > 100000)
        return 0;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t r = arr + (uintptr_t)i * ci->size;
        char key[17] = {};
        for (int k = 0; k < 16; ++k) Rd(r + im->offset + k, key[k]);
        if (id != key) continue;
        int32_t c = 0;
        return Rd(r + cm->offset, c) && c > 0 ? c : 0;
    }
    return 0;
}

// Game thread.
bool AddNow(const std::string& id, ItemKind kind, int amount, int dest)
{
    uintptr_t store = Store(dest);
    if (!store || !g_add) { log::Warn("Items", "Spawn {}: inventory not available", id); return false; }
    std::vector<uint8_t> e(std::max<uint32_t>(g_el.size, 0x30), 0);
    memcpy(e.data() + g_el.id, id.c_str(), std::min<size_t>(id.size(), 16));
    int32_t idx[2] = { -1, -1 };
    memcpy(e.data() + g_el.index, idx, 8);
    int32_t type = kind == ItemKind::Substance ? 0 : kind == ItemKind::Technology ? 1 : 2;
    // Tech with a MaxAmount of 0 can never be charged, so give it the table's charge.
    int32_t amt = kind == ItemKind::Technology ? TechCharge(id) : amount;
    memcpy(e.data() + g_el.amount, &amt, 4);
    memcpy(e.data() + g_el.maxAmount, &amt, 4);
    memcpy(e.data() + g_el.type, &type, 4);
    if (g_el.hasFlags) e[g_el.installed] = 1;
    int32_t out[2] = { -1, -1 };
    g_add((void*)store, out, e.data());
    if (out[0] < 0 && out[1] < 0) { log::Warn("Items", "Spawn {}: no free slot", id); return false; }
    log::Info("Items", "Spawned {} x{} -> slot {},{}", id, amt, out[0], out[1]);
    return true;
}

void Spawn(std::string id, ItemKind kind, int amount, int dest)
{
    game::BackupSaveOnce();
    game::Post([=] { AddNow(id, kind, amount, dest); });
}

void PlaceBasePart(std::string id)
{
    game::BackupSaveOnce();
    game::Post([id] {
        uintptr_t d = game::Data();
        if (!d || !g_placeObj || !g_baseMgr) return;
        uintptr_t p = d + (uintptr_t)g_playerOff;
        struct alignas(16) Vec4 { float x = 0, y = 0, z = 0, w = 0; };
        struct alignas(16) PhysMat34 { Vec4 right, up, at, local, offset; } xf{};
        auto rdVec4 = [](uintptr_t a, Vec4& v) {
            return Rd(a, v.x) && Rd(a + 4, v.y) && Rd(a + 8, v.z) && Rd(a + 12, v.w);
        };
        if (!rdVec4(p + 0x310, xf.right) || !rdVec4(p + 0x320, xf.up) || !rdVec4(p + 0x330, xf.at) || !rdVec4(p + 0x360, xf.local) || !rdVec4(p + 0x370, xf.offset))
            return;
        // the body's at row points backwards, so in front is along -at
        xf.local.x -= xf.at.x * 3.5f;
        xf.local.y -= xf.at.y * 3.5f;
        xf.local.z -= xf.at.z * 3.5f;

        uint16_t baseIdx = 0xFFFF;
        uint32_t baseCnt = 0;
        uintptr_t baseArr = 0;
        if (Rd(d + g_baseList - 4, baseCnt) && Rd(d + g_baseList, baseArr) && baseArr && baseCnt <= 512) {
            float wx = xf.local.x + xf.offset.x, wy = xf.local.y + xf.offset.y, wz = xf.local.z + xf.offset.z;
            float bestSq = 1000.f * 1000.f;
            for (uint32_t i = 0; i < baseCnt; ++i) {
                uintptr_t b = 0;
                Vec4 bl{}, bo{};
                if (!Rd(baseArr + i * 8, b) || !b || !rdVec4(b + 0x80, bl) || !rdVec4(b + 0x90, bo)) continue;
                float dx = (bl.x + bo.x) - wx, dy = (bl.y + bo.y) - wy, dz = (bl.z + bo.z) - wz;
                float dsq = dx * dx + dy * dy + dz * dz;
                if (dsq < bestSq) { bestSq = dsq; baseIdx = (uint16_t)i; }
            }
        }
        char id16[16] = {};
        memcpy(id16, id.c_str(), std::min<size_t>(id.size(), 15));
        struct { uint64_t userData = 0; int32_t flags = 0; int32_t pad = 0; } extra{};
        void* obj = g_placeObj((void*)g_baseMgr, id16, &baseIdx, &xf, &extra, false, 0.f, true);
        log::Info("Items", "PlaceBasePart {} (baseIdx 0x{:X}) -> {}", id, baseIdx, obj != nullptr);
    });
}

void DrawList(const char* query)
{
    if (!game::catalog::Ready()) {
        if (!g.requested) { game::catalog::Request(); g.requested = true; }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(P().textDim), "%s", L::T(game::PlayerState() ? "items.loading" : "items.ingame"));
        if (!game::catalog::Building() && game::PlayerState()) game::catalog::Request();
        return;
    }
    auto snap = game::catalog::Items();
    static uint64_t prices = 0;
    if (snap != g.snap || prices != game::prices::Version()) { g.snap = snap; prices = game::prices::Version(); Rebuild(); }
    g.browser.Draw(g.entries, query);
}

float DrawInspector(ImVec2 a, float w)
{
    const ui::BrowserEntry* e = g.browser.Focused(g.entries);
    if (!e) return 0; // default tab info
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = ui::DrawEntryHeader(dl, a, w, *e);
    float pad = S(22), bw = w - pad * 2;
    const game::Item* it = game::catalog::Find(e->key);

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    dl->AddText(ui::theme::Body(), S(16), ImVec2(a.x + pad, y), P().textDim, L::T("items.amount"));
    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y + S(22)));
    bool tech = it && it->kind == ItemKind::Technology;
    if (!tech) ui::EnergyBar("##amt", &g.amount, 1, 9999, "%.0f", bw);
    y += tech ? S(4) : S(58);

    // destination chips
    dl->AddText(ui::theme::Body(), S(16), ImVec2(a.x + pad, y), P().textDim, L::T("items.dest"));
    y += S(24);
    float x = a.x + pad;
    for (int d = 0; d < DestCount; ++d) {
        if (tech != (d == SuitTech || d == ShipTech || d == Weapon)) continue;
        const char* lbl = L::T(kDestKeys[d]);
        float cw = ui::theme::Body()->CalcTextSizeA(S(15), FLT_MAX, 0, lbl).x + S(22);
        if (x + cw > a.x + w - pad) { x = a.x + pad; y += S(32); }
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(d);
        if (ImGui::InvisibleButton("##dest", ImVec2(cw, S(26)))) g.dest = d;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = g.dest == d;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + cw, y + S(26)), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 90), S(13));
        dl->AddText(ui::theme::Body(), S(15), ImVec2(x + S(11), y + S(5)), on || hov ? P().text : P().textDim, lbl);
        x += cw + S(6);
    }
    if (tech != (g.dest == SuitTech || g.dest == ShipTech || g.dest == Weapon)) g.dest = tech ? SuitTech : Suit;
    y += S(40);

    ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
    bool live = g_add && game::FrameHookReady() && game::PlayerState();
    if (ui::Button("##spawn", L::T("items.spawn"), bw, ui::Tone::Green, live && it)) {
        Spawn(it->id, it->kind, (int)g.amount, g.dest);
        ui::Toast(L::Tf("items.spawned", e->title), "", icon::Check);
    }
    y += S(46);
    if (it && it->kind == ItemKind::BasePart && g_placeObj && g_baseMgr) {
        ImGui::SetCursorScreenPos(ImVec2(a.x + pad, y));
        if (ui::Button("##place", L::T("items.place"), bw, ui::Tone::Teal, live)) {
            PlaceBasePart(it->id);
            ui::Toast(L::Tf("items.placed", e->title), "", icon::Check);
        }
        y += S(46);
    }
    y += S(4);
    return y;
}

}

bool GiveProduct(const std::string& id) { return AddNow(id, ItemKind::Product, 1, Suit); }

void Describe(const std::string& id, ui::BrowserEntry& e)
{
    const game::Item* it = game::catalog::Find(id);
    if (!it) return;
    const char* cls = ClassOf(it->kind);
    // products group by Type, substances and tech by Category
    e.group = it->kind == ItemKind::Substance || it->kind == ItemKind::Technology ? EnumText(cls, "Category", it->category)
                                                                                    : EnumText(cls, "Type", it->type);
    e.rarity = it->rarity;
    e.value = it->value > 0 ? it->value : -1;
    e.localValue = game::catalog::LocalValue(*it);
    std::string facts;
    auto add = [&](const std::string& k, const std::string& v) {
        if (v.empty()) return;
        facts += "<TECHNOLOGY>" + k + "<> " + v + "\n";
    };
    add(L::T("items.f.type"), e.group);
    add(L::T("items.f.rarity"), EnumText(cls, "Rarity", it->rarity));
    if (e.value > 0) add(L::T("items.f.value"), Units(e.value) + " u");
    if (e.localValue >= 0) {
        std::string v = Units(e.localValue) + " u";
        if (e.value > 0) {
            int pct = (int)std::lround((e.localValue / e.value - 1.0) * 100.0);
            v += " (" + std::string(pct >= 0 ? "+" : "") + std::to_string(pct) + "%)";
        }
        add(L::T("items.f.local"), v);
    }
    if (!facts.empty()) e.body = facts + (e.body.empty() ? "" : "\n" + e.body);
}

void InstallItemsPage()
{
    g_el.Load();
    const auto& t = Loc().Resolve("Inventory.Add");
    if (t.ok) g_add = (AddFn)Abs(t.func);
    else log::Warn("Items", "Spawner unavailable: {}", t.reason);
    if (const auto& po = Loc().Resolve("Base.PlaceObject"); po.ok) g_placeObj = (PlaceObjFn)Abs(po.func);
    if (const auto& bm = Loc().Resolve("Base.Manager"); bm.ok) g_baseMgr = bm.global;
    if (const auto& pl = Loc().Resolve("Player.InstanceOffset"); pl.ok && pl.offset) g_playerOff = *pl.offset;
    if (const auto& bl = Loc().Resolve("Base.List"); bl.ok && bl.offset) g_baseList = *bl.offset;

    Registry::Get().Post([] {
        ui::SetIconLoader([](const std::string& p) { return render::tex::Icon(p); });
        g.browser.searchBox = true;
        g.browser.sortable = true;
        g.browser.chips = { L::T("items.chip.all"), L::T("items.chip.substances"), L::T("items.chip.products"), L::T("items.chip.tech"),
                            L::T("items.chip.baseparts"), L::T("items.chip.custom") };
        ui::Page page;
        page.list = DrawList;
        page.inspector = DrawInspector;
        page.badge = [] { return g.snap ? "(" + std::to_string(g.entries.size()) + ")" : std::string(); };
        ui::RegisterPage("items", std::move(page));
    });
}

}
