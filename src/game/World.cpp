#include "game/World.h"
#include "core/AppLog.h"
#include "features/Bind.h"

namespace os::game {

namespace {
uintptr_t g_slot = 0;
int64_t g_state = -1, g_reality = -1;
}

bool InitGraph()
{
    auto& L = features::Loc();
    const auto& slot = L.Resolve("Graph.DataSlot");
    const auto& ps = L.Resolve("Graph.PlayerState");
    const auto& rm = L.Resolve("Graph.RealityManager");
    if (slot.ok && slot.global) g_slot = slot.global;
    if (ps.ok && ps.offset) g_state = *ps.offset;
    if (rm.ok && rm.offset) g_reality = *rm.offset;
    log::Info("Game", "Graph: data slot {}, player state +0x{:X}, reality +0x{:X}", g_slot ? "found" : "missing",
              g_state, g_reality);
    return g_slot && g_state > 0;
}

uintptr_t Data()
{
    uintptr_t d = 0;
    if (!g_slot || !features::Rd(g_slot, d)) return 0;
    return d;
}

uintptr_t PlayerState()
{
    uintptr_t d = Data();
    return d && g_state > 0 ? d + g_state : 0;
}

uintptr_t Reality()
{
    uintptr_t d = Data();
    return d && g_reality >= 0 ? d + g_reality : 0;
}

}
