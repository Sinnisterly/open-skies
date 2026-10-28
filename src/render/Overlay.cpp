#include "assets/PakIndex.h"
#include "features/GameFeatures.h"
#include "render/Overlay.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Settings.h"
#include "input/Input.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include <imgui.h>
#include <imgui_impl_win32.h>

namespace os::overlay {

static HWND g_hwnd;
static bool g_ctx;

bool HasContext() { return g_ctx; }
HWND Window() { return g_hwnd; }

bool EnsureContext(HWND hwnd)
{
    if (g_ctx) return true;
    if (!hwnd) return false;
    g_hwnd = hwnd;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    if (!ImGui_ImplWin32_Init(hwnd)) { log::Error("Overlay", "Win32 backend init failed"); return false; }
    ui::theme::LoadFonts(paths::GameDir() + L"\\..");
    assets::paks::BuildAsync(paths::GameDir() + L"\\..\\GAMEDATA\\PCBANKS");
    ui::theme::SetAccent(Settings::Get().Data().accent);
    ui::theme::ApplyStyle(Settings::Get().Data().scale);
    log::Info("Overlay", "ImGui ready, fonts: {}", ui::theme::FontReport());
    g_ctx = true;
    return true;
}

bool BuildFrame()
{
    if (!g_ctx) return false;
    Registry::Get().Pump();
    features::Tick();
    input::Drain();

    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = ui::IsOpen();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    try { ui::Draw(); }
    catch (...) {
        log::Error("Overlay", "Menu draw threw, closing menu");
        ui::SetOpen(false);
    }
    ImGui::Render();

    auto& s = Settings::Get();
    if (s.Dirty() && s.Data().autoSave && !ui::IsOpen()) s.Save();
    ImDrawData* dd = ImGui::GetDrawData();
    return dd && dd->TotalVtxCount > 0;
}

}
