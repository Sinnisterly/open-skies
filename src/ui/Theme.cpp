#include "ui/Theme.h"
#include "assets/GameFonts.h"
#include "core/AppLog.h"
#include <cstring>

namespace os::ui {

ImU32 Alpha(ImU32 c, float a)
{
    ImU32 al = (ImU32)((float)((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
    return (c & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

ImU32 Mix(ImU32 a, ImU32 b, float t)
{
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t));
}

namespace theme {

static Palette g_p = {
    IM_COL32(4, 8, 16, 110),      // backdrop
    IM_COL32(14, 20, 30, 222),    // panel
    IM_COL32(8, 12, 20, 236),     // panelLow
    IM_COL32(255, 255, 255, 10),  // row
    IM_COL32(30, 86, 104, 235),   // rowHover
    IM_COL32(178, 42, 24, 255),   // plate
    IM_COL32(52, 22, 22, 225),    // plateDark
    IM_COL32(214, 64, 36, 255),   // redLine
    IM_COL32(28, 78, 96, 250),    // teal
    IM_COL32(58, 52, 50, 236),    // warm
    IM_COL32(30, 92, 150, 150),   // blue
    IM_COL32(96, 176, 240, 255),  // blueText
    IM_COL32(242, 194, 62, 255),  // gold
    IM_COL32(246, 204, 16, 255),  // yellow
    IM_COL32(122, 104, 18, 255),  // yellowDim
    IM_COL32(32, 140, 82, 255),   // green
    IM_COL32(16, 70, 44, 220),    // greenDeep
    IM_COL32(176, 38, 26, 255),   // red
    IM_COL32(84, 24, 20, 220),    // redDeep
    IM_COL32(244, 244, 244, 255), // text
    IM_COL32(214, 214, 214, 255), // textBody
    IM_COL32(160, 166, 172, 255), // textDim
    IM_COL32(112, 118, 126, 255), // textFaint
    IM_COL32(220, 228, 236, 40),  // line
    IM_COL32(220, 228, 236, 110), // lineStrong
    IM_COL32(120, 160, 205, 255), // planned
};

struct AccentDef { ImU32 c; const char* key; };
static const AccentDef kAccents[] = {
    { IM_COL32(242, 194, 62, 255), "set.accent.amber" },
    { IM_COL32(86, 214, 232, 255), "set.accent.cyan" },
    { IM_COL32(176, 140, 255, 255), "set.accent.violet" },
    { IM_COL32(236, 240, 244, 255), "set.accent.white" },
};

const Palette& P() { return g_p; }
int AccentCount() { return IM_ARRAYSIZE(kAccents); }
const char* AccentKey(int preset) { return kAccents[preset < 0 || preset >= AccentCount() ? 0 : preset].key; }

void SetAccent(int preset)
{
    if (preset < 0 || preset >= AccentCount()) preset = 0;
    g_p.gold = kAccents[preset].c;
}

static ImFont* g_title;
static ImFont* g_body;
static ImFont* g_mono;
static bool g_icons;
static std::string g_report;
static float g_scale = 1.f;

static ImFont* AddBlob(const assets::FontBlob& b, const assets::FontBlob* icons, float iconScale)
{
    ImGuiIO& io = ImGui::GetIO();
    if (!b.Ok()) return nullptr;
    void* mem = IM_ALLOC(b.data.size());
    memcpy(mem, b.data.data(), b.data.size());
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = true;
    strncpy(cfg.Name, b.name.c_str(), sizeof cfg.Name - 1);
    ImFont* f = io.Fonts->AddFontFromMemoryTTF(mem, (int)b.data.size(), 18.f, &cfg);
    if (f && icons && icons->Ok()) {
        void* im = IM_ALLOC(icons->data.size());
        memcpy(im, icons->data.data(), icons->data.size());
        ImFontConfig ic;
        ic.MergeMode = true;
        ic.FontDataOwnedByAtlas = true;
        ic.GlyphMinAdvanceX = 18.f;
        ic.GlyphOffset = ImVec2(0, 1);
        io.Fonts->AddFontFromMemoryTTF(im, (int)icons->data.size(), 18.f * iconScale, &ic);
    }
    return f;
}

void LoadFonts(const std::wstring& gameRoot)
{
    auto set = assets::LoadFonts(gameRoot);
    g_icons = set.icons.Ok();
    g_title = AddBlob(set.heading, &set.icons, 0.9f);
    g_body = AddBlob(set.body, &set.icons, 0.85f);
    g_mono = AddBlob(set.mono, nullptr, 1.f);
    if (!g_body) g_body = ImGui::GetIO().Fonts->AddFontDefault();
    if (!g_title) g_title = g_body;
    if (!g_mono) g_mono = g_body;
    ImGui::GetIO().FontDefault = g_body;
    g_report = set.heading.name + " / " + set.body.name + (g_icons ? " / icons" : " / no icons");
}

ImFont* Title() { return g_title; }
ImFont* Body() { return g_body; }
ImFont* Mono() { return g_mono; }
bool HasIcons() { return g_icons; }
const std::string& FontReport() { return g_report; }

float S(float v) { return v * g_scale; }

void ApplyStyle(float scale)
{
    g_scale = scale;
    ImGuiStyle st;
    st.WindowPadding = ImVec2(0, 0);
    st.WindowBorderSize = 0;
    st.WindowRounding = 0;
    st.ChildRounding = 0;
    st.FrameRounding = 0;
    st.PopupRounding = 0;
    st.ScrollbarRounding = 0;
    st.GrabRounding = 0;
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(8, 0);
    st.ScrollbarSize = 5;
    st.FontSizeBase = 18.f;
    st.FontScaleMain = scale;

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = ImGui::ColorConvertU32ToFloat4(g_p.text);
    c[ImGuiCol_TextDisabled] = ImGui::ColorConvertU32ToFloat4(g_p.textFaint);
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImGui::ColorConvertU32ToFloat4(g_p.panelLow);
    c[ImGuiCol_Border] = ImGui::ColorConvertU32ToFloat4(g_p.line);
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, 0.05f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, 0.08f);
    c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, 0.11f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.26f);
    c[ImGuiCol_ScrollbarGrabActive] = ImGui::ColorConvertU32ToFloat4(g_p.gold);
    c[ImGuiCol_TextSelectedBg] = ImGui::ColorConvertU32ToFloat4(Alpha(g_p.teal, 0.8f));
    c[ImGuiCol_NavCursor] = ImGui::ColorConvertU32ToFloat4(g_p.gold);
    st.ScaleAllSizes(scale);
    st.MouseCursorScale = scale; // ScaleAllSizes truncates this to 0 below 1.0x
    ImGui::GetStyle() = st;
}

}

}
