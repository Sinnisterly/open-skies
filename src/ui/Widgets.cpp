#include "ui/Widgets.h"
#include "ui/Theme.h"
#include <imgui_internal.h>
#include <cctype>
#include <cmath>
#include <unordered_map>

namespace os::ui {

using theme::P;
using theme::S;

std::string Utf8(unsigned c)
{
    char b[5] = {};
    if (c < 0x80) b[0] = (char)c;
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); }
    else { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); }
    return b;
}

std::string Upper(const char* s)
{
    std::string o(s);
    for (auto& ch : o) ch = (char)toupper((unsigned char)ch);
    return o;
}

float SpacedWidth(ImFont* f, float size, const char* text, float spacing)
{
    float w = 0;
    int n = 0;
    for (const char* p = text; *p;) {
        unsigned c;
        const char* next = p + ImTextCharFromUtf8(&c, p, nullptr);
        w += f->CalcTextSizeA(size, FLT_MAX, 0, p, next).x;
        ++n;
        p = next;
    }
    return w + spacing * (n > 0 ? n - 1 : 0);
}

void SpacedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* text, float spacing)
{
    for (const char* p = text; *p;) {
        unsigned c;
        const char* next = p + ImTextCharFromUtf8(&c, p, nullptr);
        dl->AddText(f, size, pos, col, p, next);
        pos.x += f->CalcTextSizeA(size, FLT_MAX, 0, p, next).x + spacing;
        p = next;
    }
}

float KeyGlyphWidth(const char* key, float h)
{
    float tw = theme::Title()->CalcTextSizeA(h * 0.72f, FLT_MAX, 0, key).x;
    return ImMax(h, tw + h * 0.5f);
}

float KeyGlyph(ImDrawList* dl, ImVec2 pos, const char* key, float h, ImU32 col)
{
    float w = KeyGlyphWidth(key, h);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), col, h * 0.18f, S(1.5f));
    float fs = h * 0.72f;
    ImVec2 ts = theme::Title()->CalcTextSizeA(fs, FLT_MAX, 0, key);
    dl->AddText(theme::Title(), fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), col, key);
    return w;
}

float Anim(ImGuiID id, float target, float speed)
{
    static std::unordered_map<ImGuiID, float> vals;
    auto [it, fresh] = vals.try_emplace(id, target);
    if (!fresh) it->second += (target - it->second) * ImMin(1.f, ImGui::GetIO().DeltaTime * speed);
    return it->second;
}

bool Switch(const char* id, bool* v, bool enabled)
{
    const auto& p = P();
    ImVec2 size(S(58), S(24));
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool changed = false;
    if (enabled && ImGui::IsItemClicked()) { *v = !*v; changed = true; }
    bool hov = enabled && ImGui::IsItemHovered();
    float t = Anim(ImGui::GetID(id), *v ? 1.f : 0.f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, Mix(IM_COL32(0, 0, 0, 90), p.yellow, t * (enabled ? 1.f : 0.4f)));
    dl->AddRect(pos, b, hov ? p.text : Mix(p.lineStrong, p.yellow, t), 0.f, S(1));
    float k = size.y - S(8);
    float kx = pos.x + S(4) + (size.x - k - S(8)) * t;
    dl->AddRectFilled(ImVec2(kx, pos.y + S(4)), ImVec2(kx + k, pos.y + S(4) + k), Mix(enabled ? p.textDim : p.textFaint, IM_COL32(30, 26, 8, 255), t));
    const char* label = *v ? "ON" : "OFF";
    float fs = S(13);
    float lw = SpacedWidth(theme::Title(), fs, label, S(1));
    float lx = *v ? pos.x + S(7) : b.x - S(7) - lw;
    SpacedText(dl, theme::Title(), fs, ImVec2(lx, pos.y + (size.y - fs) * 0.5f - S(1)),
               *v ? Alpha(IM_COL32(30, 26, 8, 255), t) : Alpha(p.textDim, 1 - t), label, S(1));
    return changed;
}

bool EnergyBar(const char* id, float* v, float vmin, float vmax, const char* fmt, float width, bool enabled, bool dim)
{
    const auto& p = P();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float h = S(26);
    float valW = S(76);
    float barW = width - valW;
    ImGui::InvisibleButton(id, ImVec2(width, h));
    bool active = enabled && ImGui::IsItemActive(), hov = enabled && ImGui::IsItemHovered();
    bool changed = false;
    if (active) {
        float t = ImClamp((ImGui::GetIO().MousePos.x - pos.x) / barW, 0.f, 1.f);
        float nv = vmin + (vmax - vmin) * t;
        float step = (vmax - vmin) > 20 ? 1.f : 0.1f;
        nv = std::round(nv / step) * step;
        if (nv != *v) { *v = nv; changed = true; }
    }
    float t = ImClamp((*v - vmin) / (vmax - vmin), 0.f, 1.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float bh = S(9);
    float y0 = pos.y + (h - bh) * 0.5f;
    ImU32 fill = !enabled ? p.textFaint : dim ? Mix(p.yellow, p.yellowDim, 0.55f) : p.yellow;
    ImU32 rest = !enabled ? Alpha(p.textFaint, 0.3f) : p.yellowDim;
    dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + barW, y0 + bh), Alpha(rest, dim ? 0.6f : 1.f));
    dl->AddRectFilled(ImVec2(pos.x, y0), ImVec2(pos.x + barW * t, y0 + bh), fill);
    if (hov || active) {
        float gx = pos.x + barW * t;
        dl->AddRectFilled(ImVec2(gx - S(1.5f), y0 - S(5)), ImVec2(gx + S(1.5f), y0 + bh + S(5)), p.text);
    }
    char buf[32];
    snprintf(buf, sizeof buf, fmt, *v);
    float fs = S(17);
    ImVec2 ts = theme::Body()->CalcTextSizeA(fs, FLT_MAX, 0, buf);
    float tx = pos.x + width - ts.x;
    ImU32 tc = enabled && !dim ? p.text : p.textFaint;
    if (theme::HasIcons()) {
        std::string bolt = Utf8(0xf0e7);
        float bw = theme::Body()->CalcTextSizeA(S(13), FLT_MAX, 0, bolt.c_str()).x;
        dl->AddText(theme::Body(), S(13), ImVec2(tx - bw - S(4), pos.y + (h - S(13)) * 0.5f), tc, bolt.c_str());
    }
    dl->AddText(theme::Body(), fs, ImVec2(tx, pos.y + (h - ts.y) * 0.5f), tc, buf);
    return changed;
}

bool KeyChip(const char* id, const char* text, bool capturing, bool conflict, float width)
{
    const auto& p = P();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(28));
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(pos.x + size.x, pos.y + size.y);
    if (capturing) {
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.f);
        dl->AddRectFilled(pos, b, Alpha(p.teal, 0.6f + 0.4f * pulse));
    }
    else dl->AddRectFilled(pos, b, hov ? Alpha(p.teal, 0.7f) : IM_COL32(0, 0, 0, 80));
    ImU32 border = capturing ? p.text : conflict ? p.red : hov ? p.lineStrong : p.line;
    dl->AddRect(pos, b, border, size.y * 0.15f, S(1.5f));
    float fs = S(16);
    ImVec2 ts = theme::Title()->CalcTextSizeA(fs, FLT_MAX, 0, text);
    ImU32 tc = capturing ? p.text : conflict ? IM_COL32(255, 140, 120, 255) : (*text ? p.text : p.textFaint);
    dl->AddText(theme::Title(), fs, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), tc, text);
    return clicked;
}

static void ToneColors(Tone t, ImU32& band, ImU32& hover)
{
    const auto& p = P();
    switch (t) {
    case Tone::Green: band = p.green; hover = Mix(p.green, IM_COL32(255, 255, 255, 255), 0.15f); break;
    case Tone::Red: band = p.red; hover = Mix(p.red, IM_COL32(255, 255, 255, 255), 0.15f); break;
    case Tone::Neutral: band = IM_COL32(255, 255, 255, 18); hover = IM_COL32(255, 255, 255, 40); break;
    default: band = p.teal; hover = Mix(p.teal, IM_COL32(255, 255, 255, 255), 0.15f); break;
    }
}

bool Button(const char* id, const char* label, float width, Tone tone, bool enabled)
{
    const auto& p = P();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(30));
    bool clicked = ImGui::InvisibleButton(id, size) && enabled;
    bool hov = enabled && ImGui::IsItemHovered();
    bool held = enabled && ImGui::IsItemActive();
    ImU32 band, hover;
    ToneColors(tone, band, hover);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, !enabled ? IM_COL32(255, 255, 255, 10) : held ? band : hov ? hover : Alpha(band, 0.85f));
    if (hov) dl->AddRect(pos, b, Alpha(p.text, 0.6f), 0.f, S(1));
    float fs = S(18);
    ImVec2 ts = theme::Title()->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(theme::Title(), fs, ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), enabled ? p.text : p.textFaint, label);
    return clicked;
}

namespace {

void PushField()
{
    const auto& p = P();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 110));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Alpha(p.rowHover, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Alpha(p.rowHover, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, Mix(p.panel, IM_COL32(0, 0, 0, 255), 0.2f));
    ImGui::PushStyleColor(ImGuiCol_Header, p.rowHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Alpha(p.rowHover, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 110));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Alpha(p.rowHover, 0.6f));
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleColor(ImGuiCol_Border, Alpha(p.blueText, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
    ImGui::PushFont(theme::Body(), S(17));
}

void PopField()
{
    ImGui::PopFont();
    ImGui::PopStyleColor(10);
    ImGui::PopStyleVar(4);
}

}

bool Dropdown(const char* id, const std::vector<std::string>& items, int* index, float width, bool enabled)
{
    PushField();
    ImGui::SetNextItemWidth(width);
    ImGui::BeginDisabled(!enabled);
    bool changed = false;
    const char* cur = *index >= 0 && *index < (int)items.size() ? items[*index].c_str() : "";
    if (ImGui::BeginCombo(id, cur, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < (int)items.size(); ++i) {
            bool sel = i == *index;
            if (ImGui::Selectable(items[i].c_str(), sel)) { *index = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    PopField();
    return changed;
}

bool TextField(const char* id, char* buf, size_t size, float width, const char* hint)
{
    PushField();
    ImGui::SetNextItemWidth(width);
    bool enter = ImGui::InputTextWithHint(id, hint ? hint : "", buf, size, ImGuiInputTextFlags_EnterReturnsTrue);
    PopField();
    return enter;
}

bool HoldButton(const char* id, const char* label, float width, float seconds)
{
    const auto& p = P();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(width, S(30));
    ImGui::InvisibleButton(id, size);
    ImGuiID gid = ImGui::GetID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    float prog = st->GetFloat(gid, 0.f);
    bool fired = false;
    if (ImGui::IsItemActive()) {
        prog += ImGui::GetIO().DeltaTime / seconds;
        if (prog >= 1.f) { fired = true; prog = 0.f; ImGui::ClearActiveID(); }
    }
    else prog = ImMax(0.f, prog - ImGui::GetIO().DeltaTime * 3.f);
    st->SetFloat(gid, prog);

    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, b, hov ? Mix(p.red, IM_COL32(255, 255, 255, 255), 0.12f) : Alpha(p.red, 0.85f));
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x * prog, b.y), IM_COL32(255, 255, 255, 40));
    if (hov) dl->AddRect(pos, b, Alpha(p.text, 0.6f), 0.f, S(1));

    // the game's hold ring
    ImVec2 c(pos.x + S(17), pos.y + size.y * 0.5f);
    float r = S(8);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 70), 32, S(2));
    if (prog > 0.f) {
        dl->PathArcTo(c, r, -IM_PI * 0.5f, -IM_PI * 0.5f + IM_PI * 2.f * prog, 40);
        dl->PathStroke(p.text, S(2.5f));
    }
    float fs = S(18);
    ImVec2 ts = theme::Title()->CalcTextSizeA(fs, FLT_MAX, 0, label);
    dl->AddText(theme::Title(), fs, ImVec2(pos.x + S(14) + (size.x - S(14) - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f), p.text, label);
    return fired;
}

void Tag(const char* text, ImU32 col)
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float fs = S(13), sp = S(1.2f);
    float tw = SpacedWidth(theme::Title(), fs, text, sp);
    ImVec2 size(tw + S(14), S(22));
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Alpha(col, 0.16f));
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), Alpha(col, 0.75f), 0.f, S(1));
    SpacedText(dl, theme::Title(), fs, ImVec2(pos.x + S(7), pos.y + (size.y - fs) * 0.5f - S(1)), col, text, sp);
}

}
