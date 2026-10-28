#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Hotkeys.h"
#include "core/Locale.h"
#include "core/Settings.h"
#include "input/Input.h"
#include "render/Textures.h"
#include "render/VulkanHook.h"
#include "features/GameFeatures.h"
#include "ui/Menu.h"
#include "ui/Theme.h"
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <vector>

namespace os {

namespace {

HMODULE g_self;

#include "ui/CreditLogo.inc"

std::string GameBuild()
{
    static std::string cached;
    if (!cached.empty()) return cached;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD h = 0, n = GetFileVersionInfoSizeW(exe, &h);
    std::string build = "unknown";
    if (n) {
        std::vector<uint8_t> buf(n);
        struct Lang { WORD lang, cp; }* tr = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(exe, 0, n, buf.data()) && VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&tr, &len) && tr) {
            wchar_t key[96];
            swprintf(key, 96, L"\\StringFileInfo\\%04x%04x\\FileVersion", tr->lang, tr->cp);
            wchar_t* ver = nullptr;
            if (VerQueryValueW(buf.data(), key, (void**)&ver, &len) && ver) build = paths::Utf8(ver);
        }
    }
    cached = "NMS " + build;
    return cached;
}

struct GameHost : ui::Host
{
    const char* BackendName() override { return "Vulkan"; }
    std::string GameBuild() override { return os::GameBuild(); }
    bool Live() override { return true; }
    void OpenFolder() override { ShellExecuteW(nullptr, L"open", paths::Root().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    void OnOpenChanged(bool open) override { input::OnMenuOpen(open); }
    ImTextureID CreditLogo(float& aspect) override
    {
        aspect = (float)kCreditLogoW / kCreditLogoH;
        return render::tex::Rgba("credit:logo", kCreditLogo, kCreditLogoW, kCreditLogoH);
    }
    void WriteDiagnostics() override
    {
        auto st = render::GetStats();
        log::Info("Diag", "Open Skies {} on {}", ui::kVersion, GameBuild());
        log::Info("Diag", "Root {}", paths::Utf8(paths::Root()));
        log::Info("Diag", "Fonts {}", ui::theme::FontReport());
        log::Info("Diag", "Vulkan device {} overlay {} swapchain {}x{} fmt {} images {} presents {} {}", st.device, st.imgui,
                  st.width, st.height, st.format, st.images, st.presents, st.note);
        auto& r = Registry::Get();
        log::Info("Diag", "Features ready {} of {}", r.ReadyCount(), r.TotalCount());
        static const char* names[] = { "ready", "pending", "unavailable", "planned" };
        for (auto* f : r.All())
            log::Info("Diag", "  {:<22} {:<11} {} {}", f->id, names[(int)f->avail], f->on ? "on" : "off", f->reason);
        for (auto& [id, c] : Hotkeys::Get().All()) log::Info("Diag", "  bind {} = {}", id, Hotkeys::Name(c));
    }
};

GameHost g_host;

HWND FindGameWindow()
{
    struct Ctx { DWORD pid; HWND found; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto* c = (Ctx*)p;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == c->pid && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { c->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

DWORD WINAPI Worker(LPVOID)
{
    try {
        paths::Init();
        log::Init(paths::File(L"OpenSkies.log"));
        log::Info("Boot", "Open Skies {} loading in {}", ui::kVersion, GameBuild());
        Settings::Get().Load();
        L::Init(Settings::Get().Data().language);
        RegisterCatalog(Registry::Get());
        Settings::Get().ApplyToFeatures(false);
        ui::Init(&g_host);

        render::InstallVulkanHooks();

        HWND hwnd = nullptr;
        for (int i = 0; i < 600 && !hwnd; ++i) { hwnd = FindGameWindow(); if (!hwnd) Sleep(250); }
        if (!hwnd) { log::Error("Boot", "Game window not found, input disabled"); return 0; }
        input::Install(hwnd, g_self);

        // Remembered toggles first, so the bindings switch them on once ready.
        Registry::Get().Post([] { if (Settings::Get().Data().rememberToggles) Settings::Get().ApplyToFeatures(true); });
        features::Install();
        log::Info("Boot", "Ready. Press {} to open the menu.", Hotkeys::Name(Hotkeys::Get().Of(Hotkeys::MenuId)));
    }
    catch (const std::exception& e) { log::Error("Boot", "Startup failed: {}", e.what()); }
    catch (...) { log::Error("Boot", "Startup failed"); }
    return 0;
}

}

}

extern "C" void OpenSkies_Start(HMODULE self)
{
    os::g_self = self;
    if (HANDLE t = CreateThread(nullptr, 0, &os::Worker, nullptr, 0, nullptr)) CloseHandle(t);
}
