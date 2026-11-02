#pragma once
#include <imgui.h>
#include <functional>
#include <string>

namespace os::ui {

// What the menu needs from whoever hosts it.
struct Host
{
    virtual ~Host() = default;
    virtual const char* BackendName() = 0;
    virtual std::string GameBuild() = 0;
    virtual bool Live() = 0;
    virtual void OpenFolder() {}
    virtual void WriteDiagnostics() {}
    virtual void OnOpenChanged(bool) {}
    virtual ImTextureID CreditLogo(float& aspect) { (void)aspect; return ImTextureID{}; } // 0 until uploaded
};

constexpr const char* kVersion = "1.0.5";

void Init(Host* host);
void SetOpen(bool open);
void ToggleOpen();
bool IsOpen();

// Call once per frame between ImGui::NewFrame and ImGui::Render.
void Draw();

// Called by input code when a bound key fires.
void TriggerBinding(const std::string& id);

void Toast(const std::string& title, const std::string& body = "", unsigned icon = 0);

// A custom tab page (spawner, unlocker, garage). Replaces the feature list of that tab.
struct Page
{
    std::function<void(const char* query)> list;      // inside the scrolling list area
    std::function<float(ImVec2 a, float w)> inspector; // right panel content, returns its bottom y; empty = default
    std::function<std::string()> badge;               // header text after the title
    float inspectorWidth = 0;                          // unscaled; 0 keeps the default
    bool inspectorScrolls = true;                      // false: the page scrolls its own inspector parts
};
void RegisterPage(const std::string& tab, Page page);
// A feature's own row (switch, sliders, hotkey) inside a page, so the page and its home tab share one setting.
void DrawFeatureRow(const std::string& id);

// Navigation helpers
void SelectTab(const std::string& tab, const std::string& sub = "");
void SetSearch(const std::string& text);
void ForceHover(const std::string& featureId);
void SetWindowPos(float x, float y);

}
