#pragma once
#include <windows.h>

namespace os::overlay {

// Creates the ImGui context, Win32 backend, fonts and style. Safe to call repeatedly.
bool EnsureContext(HWND hwnd);
bool HasContext();

// Runs one UI frame. Returns true when there is something to draw.
bool BuildFrame();

HWND Window();

}
