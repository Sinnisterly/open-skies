#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace os::render {

// Hooks vulkan-1.dll (and Streamline's interposer when present) so the overlay draws on every present.
bool InstallVulkanHooks();

struct Stats
{
    bool device = false;
    bool imgui = false;
    uint32_t format = 0, width = 0, height = 0, images = 0;
    uint64_t presents = 0;
    std::string note;
};
Stats GetStats();

// The game's sub render targets only hold their image inside the frame that draws them (by present time the
// memory holds something else). Once watched, the image behind gameView is copied into our own image right after
// each game pass that draws it, in the game's own command buffer. Each kind of pass gets its own copy (a source).
bool CaptureTarget(uint64_t gameView, uint32_t width, uint32_t height); // render thread
struct CaptureSource
{
    uint64_t view = 0; // our copy, alpha read as 1; 0 until a copy has been recorded
    uint32_t attachments = 0, index = 0;
    bool depth = false;
    int layout = 0;
};
std::vector<CaptureSource> CaptureSources();

}
