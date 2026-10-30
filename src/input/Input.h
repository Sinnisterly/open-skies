#pragma once
#include <windows.h>
#include <cstdint>

namespace os::input {

// Subclasses the game window and installs the input blocking hooks.
bool Install(HWND hwnd, HMODULE self);
void Uninstall();

// Feeds queued window messages to ImGui. Render thread only.
void Drain();

void OnMenuOpen(bool open);
bool Blocking();

// While set and the menu is closed, keys it wants never reach the game (the blueprint paste preview
// reads them itself). VK_LBUTTON / VK_RBUTTON are those mouse buttons, kWheel the mouse wheel.
constexpr uint16_t kWheel = 0x07; // a reserved VK code
void SetSwallow(bool (*wants)(uint16_t vk));
int TakeWheel(); // wheel steps (120 each) since the last call, while swallowing

}
