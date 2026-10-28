#pragma once
#include <cstdint>
#include <functional>

namespace os::game {

// Runs jobs on the game's main thread at the start of cGcApplication::Update.
bool InstallFrameHook();
bool FrameHookReady();
void Post(std::function<void()> job);
uint64_t Frame();
void EveryFrame(void (*fn)()); // runs at the start of each game frame

}
