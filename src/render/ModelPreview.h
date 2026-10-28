#pragma once
#include <imgui.h>
#include <cstdint>
#include <functional>
#include <string>

namespace os::render::preview {

struct Info
{
    const char* step = "";
    uint32_t width = 0, height = 0, format = 0;
    uintptr_t tex = 0;
    bool own = false; // true when the selected item's renderer has an image
};

// What to render: a model file, and a fill that applies the look onto a scratch customisation (game thread).
struct Request
{
    std::string key, file;
    uint64_t version = 0; // bump to rebuild the same item after an edit
    std::function<void(void* scratch)> fill;
};

void Install();                // worker thread: resolve and hook
void Want(const Request& r);   // any thread; ignored when nothing changed
void SetScale(float s);        // camera zoom, bigger is closer
void SetAspect(float aspect);  // displayed panel width / height
void SetView(float yaw, float pitch); // degrees, on top of the HUD camera's own angles

// The preview image. Render thread only. Returns 0 while nothing is ready; info says why.
ImTextureID Image(Info* info = nullptr);
void Release(); // before the device or the ImGui backend goes away

}
