#include "ui/Browser.h"
#include "core/Locale.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace os::ui {

using theme::P;
using theme::S;

namespace {

std::function<ImTextureID(const std::string&)> g_loader;

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

void DrawGlyph(ImDrawList* dl, ImVec2 a, float size, unsigned glyph, ImU32 col)
{
    if (!glyph || !theme::HasIcons()) return;
    std::string s = Utf8(glyph);
    float fs = size * 0.45f;
    ImVec2 ts = theme::Body()->CalcTextSizeA(fs, FLT_MAX, 0, s.c_str());
    dl->AddText(theme::Body(), fs, ImVec2(a.x + (size - ts.x) * 0.5f, a.y + (size - ts.y) * 0.5f), col, s.c_str());
}

void DrawIconBox(ImDrawList* dl, ImVec2 a, float size, const BrowserEntry& e)
{
    ImTextureID tex = LoadIcon(e.icon);
    if (tex) {
        float pad = size * 0.08f;
        dl->AddImage(ImTextureRef(tex), ImVec2(a.x + pad, a.y + pad), ImVec2(a.x + size - pad, a.y + size - pad));
    }
    else DrawGlyph(dl, a, size, e.glyph ? e.glyph : icon::Cubes, e.tint ? e.tint : P().textFaint);
}

}

void SetIconLoader(std::function<ImTextureID(const std::string&)> fn) { g_loader = std::move(fn); }
ImTextureID LoadIcon(const std::string& path) { return g_loader && !path.empty() ? g_loader(path) : ImTextureID{}; }

void Browser::Filter(const std::vector<BrowserEntry>& all, const std::string& q)
{
    m_view.clear();
    m_viewKeys.clear();
    std::string lq = Lower(q);
    for (int i = 0; i < (int)all.size(); ++i) {
        const auto& e = all[i];
        if (m_chip > 0 && e.chip != m_chip) continue;
        if (m_chip == 0 && hideFromAll > 0 && e.chip == hideFromAll) continue;
        if (hideMarked && e.marked) continue;
        if (!lq.empty() && Lower(e.title).find(lq) == std::string::npos && Lower(e.key).find(lq) == std::string::npos &&
            Lower(e.subtitle).find(lq) == std::string::npos) continue;
        m_view.push_back(i);
    }
    auto sort = sortOptions.empty() ? (SortBy)m_sort : sortOptions[std::clamp(m_sort, 0, (int)sortOptions.size() - 1)].first;
    if (sort != SortBy::Default || m_group) {
        // unknown values sort last, values high to low
        auto num = [](double a, double b) { return a < 0 ? false : b < 0 ? true : a > b; };
        auto metric = [&](const BrowserEntry& e) -> double {
            switch (sort) {
            case SortBy::Value: return e.value;
            case SortBy::LocalValue: return e.localValue >= 0 ? e.localValue : e.value;
            case SortBy::Rarity: return e.rarity;
            default: return -1;
            }
        };
        // grouped, the groups follow the sort too: the group holding the top item comes first
        std::unordered_map<std::string, double> best;
        if (m_group)
            for (int i : m_view) {
                auto [it, fresh] = best.emplace(all[i].group, metric(all[i]));
                if (!fresh) it->second = std::max(it->second, metric(all[i]));
            }
        std::stable_sort(m_view.begin(), m_view.end(), [&](int ia, int ib) {
            const auto &a = all[ia], &b = all[ib];
            if (m_group && a.group != b.group) {
                if (a.group.empty() != b.group.empty()) return b.group.empty();
                double ga = best[a.group], gb = best[b.group];
                if (ga != gb) return num(ga, gb);
                return a.group < b.group;
            }
            switch (sort) {
            case SortBy::Name: return Lower(StripTags(a.title)) < Lower(StripTags(b.title));
            case SortBy::Type: return a.group != b.group ? (a.group.empty() ? false : b.group.empty() ? true : a.group < b.group) : false;
            case SortBy::Value: if (a.value != b.value) return num(a.value, b.value); return false;
            case SortBy::LocalValue: {
                double va = a.localValue >= 0 ? a.localValue : a.value, vb = b.localValue >= 0 ? b.localValue : b.value;
                return va != vb && num(va, vb);
            }
            case SortBy::Rarity: if (a.rarity != b.rarity) return num(a.rarity, b.rarity); return false;
            default: return false;
            }
        });
    }
    for (int i : m_view) m_viewKeys.push_back(all[i].key);
}

void Browser::SelectVisible()
{
    for (auto& k : m_viewKeys) m_sel.insert(k);
}

const BrowserEntry* Browser::Focused(const std::vector<BrowserEntry>& all) const
{
    if (m_focus.empty()) return nullptr;
    for (auto& e : all) if (e.key == m_focus) return &e;
    return nullptr;
}

void Browser::Draw(const std::vector<BrowserEntry>& all, const char* query)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string q = query ? query : "";
    if (searchBox) {
        float w = ImGui::GetContentRegionAvail().x - S(8);
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + S(32)), IM_COL32(0, 0, 0, 110));
        dl->AddRect(p, ImVec2(p.x + w, p.y + S(32)), m_text[0] ? P().text : P().line, 0.f, S(1));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(10), p.y + S(3)));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::SetNextItemWidth(w - S(20));
        ImGui::InputTextWithHint("##browsersearch", L::T("browser.search"), m_text, sizeof m_text);
        ImGui::PopStyleColor(3);
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(42)));
        if (m_text[0]) q = m_text; // the field wins over the menu search
    }
    if (sortable) {
        static std::vector<std::string> names;
        if (sortOptions.empty())
            names = { L::T("browser.sort.default"), L::T("browser.sort.name"), L::T("browser.sort.type"),
                      L::T("browser.sort.value"), L::T("browser.sort.local"), L::T("browser.sort.rarity") };
        else {
            names.clear();
            for (auto& [by, label] : sortOptions) names.push_back(label);
        }
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddText(theme::Body(), S(15), ImVec2(p.x, p.y + S(5)), P().textDim, L::T("browser.sort"));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(56), p.y));
        Dropdown("##sort", names, &m_sort, S(190));
        float gx = p.x + S(262);
        ImGui::SetCursorScreenPos(ImVec2(gx, p.y + S(2)));
        Switch("##group", &m_group);
        dl->AddText(theme::Body(), S(15), ImVec2(gx + S(66), p.y + S(5)), P().textDim, L::T("browser.group"));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(40)));
    }
    if (m_src != all.data() || m_srcSize != all.size() || q != m_query || hideMarked != m_lastHide ||
        m_sort != m_lastSort || m_group != m_lastGroup) m_dirty = true;

    // category chips
    if (!chips.empty()) {
        float x0 = ImGui::GetCursorScreenPos().x, maxX = x0 + ImGui::GetContentRegionAvail().x - S(8);
        float x = x0, y = ImGui::GetCursorScreenPos().y, h = S(28);
        for (int i = 0; i < (int)chips.size(); ++i) {
            std::string up = Upper(chips[i].c_str());
            float w = SpacedWidth(theme::Title(), S(15), up.c_str(), S(1.2f)) + S(26);
            if (x + w > maxX && x > x0) { x = x0; y += h + S(6); }
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(i);
            bool click = ImGui::InvisibleButton("##chip", ImVec2(w, h));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            bool on = i == m_chip;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), on ? P().teal : hov ? P().rowHover : IM_COL32(0, 0, 0, 110), S(14));
            SpacedText(dl, theme::Title(), S(15), ImVec2(x + S(13), y + (h - S(15)) * 0.5f - S(1)), on || hov ? P().text : P().textDim, up.c_str(), S(1.2f));
            if (click && !on) { m_chip = i; m_dirty = true; }
            x += w + S(6);
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, y + h + S(10)));
    }

    if (m_dirty) {
        Filter(all, q);
        m_src = all.data();
        m_srcSize = all.size();
        m_query = q;
        m_lastHide = hideMarked;
        m_lastSort = m_sort;
        m_lastGroup = m_group;
        m_dirty = false;
    }

    // count line
    {
        std::string line = L::Tf("browser.count", std::to_string(m_view.size()));
        if (multi && !m_sel.empty()) line += "   /   " + L::Tf("browser.selected", std::to_string(m_sel.size()));
        dl->AddText(theme::Body(), S(15), ImGui::GetCursorScreenPos(), P().textDim, line.c_str());
        ImGui::Dummy(ImVec2(0, S(24)));
    }

    // grid, split into runs when grouping so each type gets a header
    float cell = S(78), gap = S(6), capH = captions ? S(20) : 0.f, headH = S(30);
    float rowH = cell + capH + gap;
    float avail = ImGui::GetContentRegionAvail().x - S(8);
    int cols = std::max(1, (int)((avail + gap) / (cell + gap)));
    struct Run { int start, count; };
    std::vector<Run> runs;
    for (int i = 0; i < (int)m_view.size(); ++i) {
        if (runs.empty() || (m_group && all[m_view[i]].group != all[m_view[runs.back().start]].group)) runs.push_back({ i, 0 });
        runs.back().count++;
    }
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float y = origin.y;
    for (auto& run : runs) {
        if (m_group) {
            const std::string& g = all[m_view[run.start]].group;
            std::string up = Upper(g.empty() ? L::T("browser.other") : StripTags(g).c_str());
            SpacedText(dl, theme::Title(), S(15), ImVec2(origin.x, y + S(6)), P().textDim, up.c_str(), S(1.2f));
            dl->AddLine(ImVec2(origin.x, y + headH - S(5)), ImVec2(origin.x + avail, y + headH - S(5)), P().line, S(1));
            y += headH;
        }
        int rows = (run.count + cols - 1) / cols;
        for (int r = 0; r < rows; ++r, y += rowH) {
            if (!ImGui::IsRectVisible(ImVec2(origin.x, y), ImVec2(origin.x + avail, y + rowH))) continue;
            for (int c = 0; c < cols; ++c) {
                int vi = run.start + r * cols + c;
                if (vi >= run.start + run.count) break;
                const BrowserEntry& e = all[m_view[vi]];
                ImVec2 a(origin.x + c * (cell + gap), y);
                ImVec2 b(a.x + cell, a.y + cell);
                ImGui::SetCursorScreenPos(a);
                ImGui::PushID(vi);
                bool click = ImGui::InvisibleButton("##cell", ImVec2(cell, cell + capH));
                bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                bool sel = m_sel.count(e.key) != 0, focus = e.key == m_focus;
                dl->AddRectFilled(a, b, focus ? Alpha(P().teal, 0.85f) : hov ? P().rowHover : IM_COL32(0, 0, 0, 120));
                DrawIconBox(dl, a, cell, e);
                ImU32 border = sel ? P().yellow : hov || focus ? P().text : P().line;
                dl->AddRect(a, b, border, 0.f, sel ? S(2.5f) : S(1));
                if (e.marked && theme::HasIcons()) {
                    std::string ck = Utf8(icon::Check);
                    dl->AddText(theme::Body(), S(12), ImVec2(b.x - S(15), a.y + S(3)), P().green, ck.c_str());
                }
                if (e.rare && theme::HasIcons()) {
                    std::string st = Utf8(icon::Star);
                    dl->AddText(theme::Body(), S(12), ImVec2(a.x + S(4), a.y + S(3)), P().gold, st.c_str());
                }
                if (!e.count.empty()) {
                    ImVec2 ts = theme::Body()->CalcTextSizeA(S(13), FLT_MAX, 0, e.count.c_str());
                    ImVec2 tp(b.x - ts.x - S(5), b.y - ts.y - S(3));
                    dl->AddRectFilled(ImVec2(tp.x - S(3), tp.y), ImVec2(b.x - S(2), b.y - S(2)), IM_COL32(0, 0, 0, 150), S(3));
                    dl->AddText(theme::Body(), S(13), tp, P().text, e.count.c_str());
                }
                if (captions) {
                    std::string t = StripTags(e.title);
                    while (t.size() > 1 && theme::Body()->CalcTextSizeA(S(13), FLT_MAX, 0, t.c_str()).x > cell) t.pop_back();
                    dl->AddText(theme::Body(), S(13), ImVec2(a.x, b.y + S(3)), focus ? P().text : P().textDim, t.c_str());
                }
                if (hov && !e.title.empty()) ImGui::SetTooltip("%s", StripTags(e.title).c_str());
                if (click) {
                    if (multi) {
                        m_focus = e.key;
                        if (sel) m_sel.erase(e.key); else m_sel.insert(e.key);
                    }
                    else m_focus = focus ? std::string() : e.key; // click again to unfocus
                }
            }
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
    ImGui::Dummy(ImVec2(0, S(4)));
}

namespace {

// Colours for the game's text tags, close to the in-game palette.
ImU32 TagColour(const std::string& tag, ImU32 base)
{
    static const std::pair<const char*, ImU32> tags[] = {
        { "EARTH", IM_COL32(226, 150, 76, 255) },     { "TECHNOLOGY", IM_COL32(92, 178, 232, 255) },
        { "FUEL", IM_COL32(232, 92, 80, 255) },       { "STELLAR", IM_COL32(240, 204, 72, 255) },
        { "CATALYST", IM_COL32(184, 128, 232, 255) }, { "TRADEABLE", IM_COL32(116, 204, 124, 255) },
        { "COMMODITY", IM_COL32(116, 204, 124, 255) },{ "SPECIAL", IM_COL32(214, 120, 220, 255) },
        { "EXOTIC", IM_COL32(232, 120, 184, 255) },   { "HEALTH", IM_COL32(232, 92, 80, 255) },
        { "CURIOSITY", IM_COL32(196, 160, 232, 255) },{ "CONSUMABLE", IM_COL32(116, 204, 124, 255) },
    };
    for (auto& [k, c] : tags) if (tag == k) return c;
    return tag.empty() ? base : IM_COL32(240, 222, 140, 255);
}

}

std::string StripTags(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '<') {
            size_t e = text.find('>', i);
            if (e != std::string::npos && e - i <= 24) { i = e; continue; }
        }
        out += text[i];
    }
    return out;
}

float RichText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float wrap, ImU32 base, const std::string& text)
{
    float x = 0, y = 0, line = font->CalcTextSizeA(size, FLT_MAX, 0.f, "Ay").y;
    float space = font->CalcTextSizeA(size, FLT_MAX, 0.f, " ").x;
    ImU32 col = base;
    std::string word;
    bool pendingSpace = false;
    auto flush = [&] {
        if (word.empty()) return;
        float ww = font->CalcTextSizeA(size, FLT_MAX, 0.f, word.c_str()).x;
        float gap = pendingSpace && x > 0 ? space : 0;
        if (x > 0 && x + gap + ww > wrap) { x = 0; y += line; gap = 0; }
        if (dl) dl->AddText(font, size, ImVec2(pos.x + x + gap, pos.y + y), col, word.c_str());
        x += gap + ww;
        word.clear();
        pendingSpace = false;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '<') {
            size_t e = text.find('>', i);
            if (e != std::string::npos && e - i <= 24) {
                flush();
                col = TagColour(text.substr(i + 1, e - i - 1), base);
                i = e;
                continue;
            }
        }
        if (c == '\n') { flush(); x = 0; y += line; pendingSpace = false; continue; }
        if (c == ' ' || c == '\t') { flush(); pendingSpace = true; continue; }
        if (c == '\r') continue;
        word += c;
    }
    flush();
    return y + line;
}

float DrawEntryHeader(ImDrawList* dl, ImVec2 a, float w, const BrowserEntry& e)
{
    const auto& p = P();
    float headH = S(86), ib = S(64), pad = S(22);
    ImVec2 ia(a.x + S(11), a.y + (headH - ib) * 0.5f);
    dl->AddRectFilled(ia, ImVec2(ia.x + ib, ia.y + ib), IM_COL32(10, 34, 44, 255));
    dl->AddRect(ia, ImVec2(ia.x + ib, ia.y + ib), Alpha(p.blueText, 0.5f), 0.f, S(1));
    DrawIconBox(dl, ia, ib, e);
    float tx = ia.x + ib + S(16);
    std::string up = Upper(StripTags(e.title).c_str());
    float tfs = S(25);
    while (tfs > S(15) && SpacedWidth(theme::Title(), tfs, up.c_str(), S(1.5f)) > a.x + w - tx - S(14)) tfs -= S(1);
    SpacedText(dl, theme::Title(), tfs, ImVec2(tx, a.y + S(18)), p.text, up.c_str(), S(1.5f));
    dl->AddText(theme::Body(), S(17), ImVec2(tx, a.y + S(50)), p.textBody, StripTags(e.subtitle).c_str());
    float y = a.y + headH + pad;
    if (!e.body.empty()) {
        float bw = w - pad * 2;
        y += RichText(dl, theme::Body(), S(17), ImVec2(a.x + pad, y), bw, p.textBody, e.body) + pad;
    }
    return y;
}

}
