#pragma once
#include <cstdint>

namespace os::game {

// Roots of the game's object graph, with offsets read from code.
bool InitGraph();
uintptr_t Data();        // cGcApplication::mpData, 0 before the game has built it
uintptr_t PlayerState(); // cGcPlayerState
uintptr_t Reality();     // cGcRealityManager

}
