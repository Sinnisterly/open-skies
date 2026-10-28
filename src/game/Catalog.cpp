#include "game/Catalog.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace os::game::catalog {

namespace {

namespace meta = hooks::meta;
using features::Rd;

std::mutex g_mx;
std::shared_ptr<const std::vector<Item>> g_items;
std::vector<std::shared_ptr<const std::vector<Item>>> g_old; // kept so Find() pointers stay valid
std::atomic<bool> g_building{ false };
std::shared_ptr<const std::unordered_map<std::string, const Item*>> g_index;

using GetInstanceFn = void* (*)();
using TranslateFn = const char* (*)(void* mgr, const char* key, void* fallback);

struct Lang
{
    GetInstanceFn get = nullptr;
    TranslateFn tr = nullptr;
    void* mgr = nullptr;

    bool Init()
    {
        auto& L = features::Loc();
        const auto& g = L.Resolve("Lang.GetInstance");
        const auto& t = L.Resolve("Lang.Translate");
        if (!g.ok || !t.ok) return false;
        get = (GetInstanceFn)features::Abs(g.func);
        tr = (TranslateFn)features::Abs(t.func);
        mgr = get();
        return mgr != nullptr;
    }

    std::string operator()(const std::string& key) const
    {
        if (key.empty() || !tr) return key;
        const char* s = tr(mgr, key.c_str(), nullptr);
        return s ? std::string(s) : key;
    }
};

// Text of a string-like member: inline chars, or a pointer for the variable size types.
std::string Text(uintptr_t at, const meta::Member& m)
{
    std::string out;
    if (m.type == meta::String || m.type == meta::VarString) {
        uintptr_t p = 0;
        if (!Rd(at, p) || !p) return out;
        at = p;
        for (char c; out.size() < 512 && Rd(at++, c) && c;) out += c;
        return out;
    }
    for (uint32_t i = 0; i < m.size; ++i) {
        char c;
        if (!Rd(at + i, c) || !c) break;
        out += c;
    }
    return out;
}

// Field accessors for one element class, resolved once by name.
struct Fields
{
    std::optional<meta::Member> id, name, desc, icon, iconFile, type, category, rarity, value;

    explicit Fields(const char* cls)
    {
        id = meta::Path(cls, "ID");
        name = meta::Path(cls, "Name");
        desc = meta::Path(cls, "Description");
        icon = meta::Path(cls, "Icon");
        iconFile = meta::Path("cTkTextureResource", "Filename");
        type = meta::Path(cls, "Type");
        category = meta::Path(cls, "Category");
        rarity = meta::Path(cls, "Rarity");
        value = meta::Path(cls, "BaseValue");
    }

    bool Ok() const { return id && name; }

    static int Int(uintptr_t e, const std::optional<meta::Member>& m)
    {
        int32_t v = -1;
        if (m) Rd(e + m->offset, v);
        return v;
    }

    Item Read(uintptr_t e, ItemKind kind, const Lang& tr) const
    {
        Item it;
        it.kind = kind;
        it.id = Text(e + id->offset, *id);
        it.name = tr(Text(e + name->offset, *name));
        if (desc) it.desc = tr(Text(e + desc->offset, *desc));
        if (icon && iconFile) it.icon = Text(e + icon->offset + iconFile->offset, *iconFile);
        it.type = Int(e, type);
        it.category = Int(e, category);
        it.rarity = Int(e, rarity);
        it.value = Int(e, value);
        it.procedural = !it.id.empty() && it.id[0] == '^';
        return it;
    }
};

bool Printable(const std::string& s)
{
    if (s.empty()) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7E) return false;
    return true;
}

// Reads a table object (cGcProductTable and friends): a dynamic array of elements at the class's "Table" member.
size_t ReadTable(uintptr_t tableObj, const char* tableCls, const char* elemCls, ItemKind kind, const Lang& tr, std::vector<Item>& out)
{
    auto tm = meta::Path(tableCls, "Table");
    Fields f(elemCls);
    if (!tm || !f.Ok() || !tableObj) return 0;
    uintptr_t arr = 0;
    int32_t count = 0;
    if (!Rd(tableObj + tm->offset, arr) || !Rd(tableObj + tm->offset + 8, count) || !arr || count <= 0 || count > 50000) return 0;
    size_t added = 0;
    for (int32_t i = 0; i < count; ++i) {
        Item it = f.Read(arr + (uintptr_t)i * tm->size, kind, tr);
        if (!Printable(it.id)) continue;
        out.push_back(std::move(it));
        ++added;
    }
    return added;
}

void Build()
{
    uintptr_t rm = Reality();
    Lang tr;
    if (!rm || !tr.Init()) { log::Warn("Catalog", "Game tables not reachable yet"); g_building = false; return; }

    // cGcRealityManager table pointers, in NMS.py order; each is checked by reading it as its class.
    struct Slot { uint32_t off; const char* table; const char* elem; ItemKind kind; };
    static const Slot slots[] = {
        { 0x08, "cGcSubstanceTable", "cGcRealitySubstanceData", ItemKind::Substance },
        { 0x10, "cGcTechnologyTable", "cGcTechnology", ItemKind::Technology },
        { 0x18, "cGcProductTable", "cGcProductData", ItemKind::Product },
        { 0x20, "cGcProductTable", "cGcProductData", ItemKind::BasePart },
        { 0x28, "cGcProductTable", "cGcProductData", ItemKind::Customisation },
    };
    auto items = std::make_shared<std::vector<Item>>();
    for (auto& s : slots) {
        uintptr_t obj = 0;
        Rd(rm + s.off, obj);
        size_t n = ReadTable(obj, s.table, s.elem, s.kind, tr, *items);
        log::Info("Catalog", "{} at reality+0x{:X}: {} entries", s.table, s.off, n);
    }
    {
        std::lock_guard lk(g_mx);
        auto index = std::make_shared<std::unordered_map<std::string, const Item*>>();
        for (auto& it : *items) index->emplace(it.id, &it);
        if (g_items) g_old.push_back(g_items);
        g_items = std::move(items);
        g_index = std::move(index);
    }
    g_building = false;
}

}

void Request()
{
    if (g_building.exchange(true)) return;
    game::Post(Build);
}

bool Ready()
{
    std::lock_guard lk(g_mx);
    return g_items != nullptr;
}

bool Building() { return g_building; }

std::shared_ptr<const std::vector<Item>> Items()
{
    std::lock_guard lk(g_mx);
    return g_items;
}

std::string Translate(const std::string& key)
{
    static Lang tr;
    static bool ok = tr.Init();
    return ok ? tr(key) : key;
}

namespace {
std::function<double(const Item&)> g_pricer;
}

double LocalValue(const Item& it) { return g_pricer ? g_pricer(it) : -1; }
void SetLocalPricer(std::function<double(const Item&)> fn) { g_pricer = std::move(fn); }

const Item* Find(const std::string& id)
{
    std::shared_ptr<const std::unordered_map<std::string, const Item*>> index;
    {
        std::lock_guard lk(g_mx);
        index = g_index;
    }
    if (!index) return nullptr;
    auto it = index->find(id);
    return it == index->end() ? nullptr : it->second;
}

}
