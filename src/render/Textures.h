#pragma once
#include <imgui.h>
#include <string>

namespace os::render::tex {

// Game texture by pak path (for example an item icon). Render thread only.
// Returns 0 until the texture has been read and uploaded; call again next frame.
ImTextureID Icon(const std::string& path);

// An image the DLL carries itself, as straight RGBA. Same rules as Icon.
ImTextureID Rgba(const std::string& key, const unsigned char* rgba, unsigned w, unsigned h);

bool Available(); // false when the GPU lacks BC7 sampling or the paks are not indexed yet

}
