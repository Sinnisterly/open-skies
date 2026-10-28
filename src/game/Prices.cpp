#include "game/Catalog.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "hooks/Meta.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <mutex>
#include <unordered_map>

// Sell price of every item in the current star system, through the game's own shop price roll.
// Filled on the game thread when the system changes.
namespace os::game::prices {

namespace {

namespace meta = hooks::meta;
using features::Rd;

using GenPriceFn = float (*)(void* item, bool buying, bool station, float roll, int32_t type, bool ignoreSold, bool forceMarkup);
GenPriceFn g_gen = nullptr;

int64_t g_curSystem = 0x72B610; // Data.SolarSystem
constexpr uint32_t kSysTrading = 0x2520, kSysAddress = 0x26B0, kTradingTable = 0x2E8, kClasses = 0x360;
enum PriceType { Standard = 0, Expensive = 1, Cheap = 2 };

std::mutex g_mx;
std::unordered_map<std::string, double> g_price;
std::atomic<uint64_t> g_system{ 0 }, g_version{ 0 };
std::atomic<bool> g_busy{ false };

// The current system's TradingClassData Needs and Sells categories, or false when it can't be read.
bool NeedsSells(uintptr_t d, uintptr_t sys, int32_t& needs, int32_t& sells)
{
    int32_t cls = -1;
    uintptr_t tbl = 0;
    if (!Rd(sys + kSysTrading, cls) || cls < 0 || cls > 6 || !Rd(d + kTradingTable, tbl) || !tbl) return false;
    // the classes are inline in the table, or behind a pointer there; accept whichever reads as categories
    uintptr_t arr = 0;
    for (uintptr_t base : { tbl + kClasses, Rd(tbl + kClasses, arr) ? arr : 0 }) {
        if (!base) continue;
        if (Rd(base + cls * 0x38 + 0x30, needs) && Rd(base + cls * 0x38 + 0x34, sells) && needs >= 0 && needs <= 8 && sells >= 0 && sells <= 8)
            return true;
    }
    return false;
}

struct Fields
{
    std::optional<meta::Member> id, value, trade, cost, influence;
    uint32_t size = 0;
    explicit Fields(const char* cls)
    {
        id = meta::Path(cls, "ID");
        value = meta::Path(cls, "BaseValue");
        trade = meta::Path(cls, "TradeCategory");
        cost = meta::Path(cls, "Cost");
        influence = meta::Path(cls, "EconomyInfluenceMultiplier");
        if (auto* ci = meta::Get(cls)) size = ci->size;
    }
    bool Ok() const { return id && value && trade && size; }
};

// Game thread.
void Fill(uint64_t system)
{
    uintptr_t d = Data(), rm = Reality(), sys = 0;
    if (!d || !rm || !g_gen || !Rd(d + g_curSystem, sys) || !sys) { g_busy = false; return; }
    int32_t needs = -1, sells = -1;
    bool classOk = NeedsSells(d, sys, needs, sells);
    std::unordered_map<std::string, double> out;
    struct Tbl { uint32_t off; const char* cls; };
    for (Tbl t : { Tbl{ 0x08, "cGcRealitySubstanceData" }, Tbl{ 0x18, "cGcProductData" }, Tbl{ 0x20, "cGcProductData" }, Tbl{ 0x28, "cGcProductData" } }) {
        Fields f(t.cls);
        uintptr_t obj = 0, arr = 0;
        int32_t n = 0;
        if (!f.Ok() || !Rd(rm + t.off, obj) || !obj || !Rd(obj, arr) || !Rd(obj + 8, n) || !arr || n <= 0 || n > 50000) continue;
        for (int32_t i = 0; i < n; ++i) {
            uintptr_t r = arr + (uintptr_t)i * f.size;
            int32_t value = 0, trade = -1;
            if (!Rd(r + f.value->offset, value) || value <= 0) continue;
            Rd(r + f.trade->offset, trade);
            // ShopItemData: Id +0, BaseValue +0x50, price modifiers +0x54 (5 floats), influence +0x68
            alignas(16) uint8_t item[0x100] = {};
            for (int k = 0; k < 16; ++k) Rd(r + f.id->offset + k, item[k]);
            memcpy(item + 0x50, &value, 4);
            if (f.cost) for (int k = 0; k < 0x14; ++k) Rd(r + f.cost->offset + k, item[0x54 + k]);
            float infl = 1.f;
            if (f.influence) Rd(r + f.influence->offset, infl);
            memcpy(item + 0x68, &infl, 4);
            int type = !classOk ? Standard : trade == needs ? Expensive : trade == sells ? Cheap : Standard;
            float p = g_gen(item, false, false, 0.5f, type, false, false);
            if (std::isfinite(p) && p > 0) out[std::string((const char*)item, strnlen((const char*)item, 16))] = p;
        }
    }
    {
        std::lock_guard lk(g_mx);
        g_price = std::move(out);
    }
    log::Info("Prices", "{} prices for system 0x{:X}{}", g_price.size(), system, classOk ? "" : " (trade class unread)");
    g_system = system;
    ++g_version;
    g_busy = false;
}

// Game thread, every frame: refill when the player is in a new system.
void Tick()
{
    if (Frame() % 120) return;
    uintptr_t d = Data(), sys = 0;
    uint64_t addr = 0;
    if (!d || !Rd(d + g_curSystem, sys) || !sys || !Rd(sys + kSysAddress, addr) || !addr) return;
    if (addr == g_system || !catalog::Ready() || g_busy.exchange(true)) return;
    Fill(addr);
}

}

void Install()
{
    const auto& t = features::Loc().Resolve("Economy.GeneratePrice");
    if (!t.ok) { log::Warn("Prices", "System prices unavailable: {}", t.reason); return; }
    g_gen = (GenPriceFn)features::Abs(t.func);
    if (const auto& s = features::Loc().Resolve("Data.SolarSystem"); s.ok && s.offset) g_curSystem = *s.offset;
    EveryFrame(Tick);
    catalog::SetLocalPricer([](const Item& it) {
        std::lock_guard lk(g_mx);
        auto f = g_price.find(it.id);
        return f == g_price.end() ? -1.0 : f->second;
    });
}

uint64_t Version() { return g_version; }

}
