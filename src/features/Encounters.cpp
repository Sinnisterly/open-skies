#include "features/Flight.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include <atomic>

// No pirate encounters and no wanted level.
namespace os::features {

namespace {

using StartFn = bool (*)(void* director, void* request, bool flag);
using SetLevelFn = void (*)(void* wanted, int32_t level, bool force, bool alt);

safetyhook::InlineHook g_start, g_setLevel;
std::atomic<bool> g_noPirates{ false }, g_noWanted{ false };
int64_t g_wantedOff = 0x72C710;

// Every hostile ship encounter starts here; false is the game's own "could not start".
bool StartHk(void* director, void* request, bool flag)
{
    if (g_noPirates) return false;
    return g_start.call<bool>(director, request, flag);
}

// Raising never sticks; clearing still runs the game's cleanup.
void SetLevelHk(void* wanted, int32_t level, bool force, bool alt)
{
    g_setLevel.call<void>(wanted, g_noWanted ? 0 : level, force, alt);
}

}

void InstallEncounters()
{
    if (auto* t = Need("HostileEncounter.Start", { "world.no_pirates" })) {
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t->func)), reinterpret_cast<void*>(&StartHk))) {
            g_start = std::move(*h);
            Bound b;
            b.id = "world.no_pirates";
            b.toggle = [](bool on) { g_noPirates = on; return true; };
            Ok(std::move(b));
        }
        else Fail("world.no_pirates", "hook could not be placed");
    }

    if (auto* t = Need("Wanted.SetLevel", { "world.no_wanted" })) {
        if (const Target& o = Loc().Resolve("Wanted.LocalOffset"); o.ok && o.offset) g_wantedOff = *o.offset;
        if (auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t->func)), reinterpret_cast<void*>(&SetLevelHk))) {
            g_setLevel = std::move(*h);
            Bound b;
            b.id = "world.no_wanted";
            b.toggle = [](bool on) {
                g_noWanted = on;
                if (on && game::FrameHookReady())
                    game::Post([] {
                        if (uintptr_t d = game::Data()) SetLevelHk((void*)(d + g_wantedOff), 0, false, false);
                    });
                return true;
            };
            Ok(std::move(b));
        }
        else Fail("world.no_wanted", "hook could not be placed");
    }
}

}
