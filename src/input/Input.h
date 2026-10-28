#pragma once
#include <windows.h>

namespace os::input {

// Subclasses the game window and installs the input blocking hooks.
bool Install(HWND hwnd, HMODULE self);
void Uninstall();

// Feeds queued window messages to ImGui. Render thread only.
void Drain();

void OnMenuOpen(bool open);
bool Blocking();

}
