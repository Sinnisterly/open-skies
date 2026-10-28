#include "ui/Menu.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Hotkeys.h"
#include "core/Locale.h"
#include "core/Settings.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include <imgui_internal.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>

namespace os::ui {

using theme::P;
using theme::S;

namespace {

// Layout in unscaled pixels.
constexpr float kW = 1260, kH = 860;
constexpr float kPlateW = 372, kPlateRed = 42, kPlateDark = 46;
constexpr float kContentTop = 132, kInfoW = 420, kBottom = 62;

struct Info
{
    unsigned icon = 0;
    std::string title, subtitle, body;
    const Feature* feature = nullptr;
    bool setting = false;
};

struct State
{
    Host* host = nullptr;
    bool open = false;
    std::string tab = "exosuit";
    std::map<std::string, std::string> sub;
    char search[96] = {};
    std::string forcedHover;
    Info info;
    bool infoSet = false;
    ImVec2 pos{ -1, -1 };
    float appliedScale = 0;
    int appliedAccent = -1;
    bool scrollTop = false;
};
State g;
std::map<std::string, Page> g_pages;

const Page* PageOf(const std::string& tab)
{
    auto it = g_pages.find(tab);
    return it == g_pages.end() ? nullptr : &it->second;
}

struct ToastItem { std::string title, body; unsigned icon; double t0; };
std::deque<ToastItem> g_toasts;

const char* Lt(const std::string& k) { return L::T(k); }
std::string FeatureName(const Feature& f) { return Lt("f." + f.id); }
Settings& Cfg() { return Settings::Get(); }

const TabDef* TabOf(const std::string& id)
{
    for (auto& t : Registry::Get().Tabs()) if (t.id == id) return &t;
    return nullptr;
}

std::string Shown(const char* s) { return Upper(s); }

void SetFeatureInfo(const Feature& f)
{
    const TabDef* t = TabOf(f.tab);
    g.info.icon = t ? t->icon : 0;
    g.info.title = FeatureName(f);
    g.info.subtitle = std::string(Lt("tab." + f.tab)) + "  /  " + Lt("sec." + f.section);
    g.info.body = Lt("f." + f.id + ".tip");
    g.info.feature = &f;
    g.info.setting = false;
    g.infoSet = true;
}

void SetSettingInfo(const char* title, const char* body, unsigned icon)
{
    g.info.icon = icon;
    g.info.title = title;
    g.info.subtitle = Lt("tab." + g.tab);
    g.info.body = body;
    g.info.feature = nullptr;
    g.info.setting = true;
    g.infoSet = true;
}

void ResetInfo() { g.infoSet = false; }

void ToggleFeature(Feature& f, bool on, bool toast)
{
    Registry::Get().SetOn(f, on);
    Cfg().MarkDirty();
    if (toast) Toast(L::Tf(f.on ? "toast.on" : "toast.off", FeatureName(f)), "", f.on ? icon::Check : icon::Power);
}

// ---------- list rows ----------

struct RowCtx { ImVec2 pos; float w, h; bool hov; };

RowCtx RowBegin(const char* id, float h, bool active, bool dim, bool force, float gapAfter = 4)
{
    RowCtx r;
    r.pos = ImGui::GetCursorScreenPos();
    r.w = ImGui::GetContentRegionAvail().x - S(6);
    r.h = h;
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(r.w, h));
    r.hov = force || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    ImGui::PopID();
    float t = Anim(ImGui::GetID(id), r.hov ? 1.f : 0.f, 20.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(r.pos.x + r.w, r.pos.y + h);
    dl->AddRectFilled(r.pos, b, Mix(dim ? Alpha(P().row, 0.5f) : P().row, P().rowHover, t));
    if (active) {
        dl->AddRect(r.pos, b, Alpha(P().gold, 0.55f), 0.f, S(1));
        dl->AddRectFilled(r.pos, ImVec2(r.pos.x + S(4), b.y), P().gold);
    }
    ImGui::SetCursorScreenPos(r.pos);
    (void)gapAfter;
    return r;
}

void RowEnd(const RowCtx& r, float gap = 4)
{
    ImGui::SetCursorScreenPos(ImVec2(r.pos.x, r.pos.y + r.h + S(gap)));
    ImGui::Dummy(ImVec2(0, 0));
}

void RowLabel(const RowCtx& r, const char* text, bool dim, float indent = 0, float size = 18)
{
    float fs = S(size);
    ImGui::GetWindowDrawList()->AddText(theme::Body(), fs, ImVec2(r.pos.x + S(18) + indent, r.pos.y + (r.h - fs) * 0.5f - S(1)),
                                        dim ? P().textFaint : P().text, text);
}

void Slot(const RowCtx& r, float cw, float ch)
{
    ImGui::SetCursorScreenPos(ImVec2(r.pos.x + r.w - S(16) - cw, r.pos.y + (r.h - ch) * 0.5f));
}

void RightTag(const RowCtx& r, const char* key, ImU32 col, float reserved)
{
    float tw = SpacedWidth(theme::Title(), S(13), Lt(key), S(1.2f)) + S(14);
    ImGui::SetCursorScreenPos(ImVec2(r.pos.x + r.w - S(16) - reserved - tw, r.pos.y + (r.h - S(22)) * 0.5f));
    Tag(Lt(key), col);
}

void BindHint(const Feature& f, const RowCtx& r, float reserved)
{
    Chord c = Hotkeys::Get().Of(f.id);
    if (c.Empty()) return;
    std::string name = Hotkeys::Name(c);
    float h = S(24);
    float w = KeyGlyphWidth(name.c_str(), h);
    KeyGlyph(ImGui::GetWindowDrawList(), ImVec2(r.pos.x + r.w - S(16) - reserved - S(14) - w, r.pos.y + (r.h - h) * 0.5f), name.c_str(), h,
             r.hov ? P().text : P().textDim);
}

std::string Compact(int64_t v)
{
    char b[32];
    if (v >= 1000000000) snprintf(b, sizeof b, "+%lldB", (long long)(v / 1000000000));
    else if (v >= 1000000) snprintf(b, sizeof b, "+%lldM", (long long)(v / 1000000));
    else if (v >= 1000) snprintf(b, sizeof b, "+%lldK", (long long)(v / 1000));
    else snprintf(b, sizeof b, "+%lld", (long long)v);
    return b;
}

std::string Grouped(int64_t v)
{
    std::string s = std::to_string(v), out;
    int n = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (n && n % 3 == 0) out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++n;
    }
    return out;
}

void FeatureRow(Feature& f)
{
    bool planned = f.avail == Avail::Planned;
    bool on = f.kind == Kind::Toggle && f.on;
    RowCtx r = RowBegin(f.id.c_str(), S(46), on, planned, g.forcedHover == f.id);
    if (r.hov) SetFeatureInfo(f);
    std::string name = FeatureName(f);
    RowLabel(r, name.c_str(), planned);

    ImGui::PushID(f.id.c_str());
    if (f.kind == Kind::Toggle) {
        if (planned) RightTag(r, "ui.planned", P().planned, 0);
        else {
            Slot(r, S(58), S(24));
            bool v = f.on;
            if (Switch("##sw", &v)) ToggleFeature(f, v, false);
            float reserved = S(58);
            if (f.avail != Avail::Ready && g.host->Live()) {
                RightTag(r, f.avail == Avail::Unavailable ? "ui.unavailable" : "ui.pending", f.avail == Avail::Unavailable ? P().red : P().textDim, reserved + S(14));
                reserved += S(110);
            }
            BindHint(f, r, reserved);
        }
    }
    else if (f.kind == Kind::Number) {
        if (f.avail == Avail::Ready && f.getNumber) {
            int64_t v = 0;
            f.numberKnown = f.getNumber(v);
            if (f.numberKnown) f.number = v;
        }
        float fieldW = S(170), btnW = S(70);
        int nb = (int)f.quickAdds.size() + (f.numMax <= 1000 ? 1 : 0);
        Slot(r, fieldW + nb * (btnW + S(6)), S(30));
        ImVec2 fp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(fp, ImVec2(fp.x + fieldW, fp.y + S(30)), IM_COL32(0, 0, 0, 90));
        dl->AddRect(fp, ImVec2(fp.x + fieldW, fp.y + S(30)), P().line, 0.f, S(1));
        if (f.numberKnown) {
            ImGui::PushFont(theme::Body(), 17.f);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 12));
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Alpha(P().teal, 0.6f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), S(5)));
            ImGui::SetNextItemWidth(fieldW);
            long long val = f.number;
            if (ImGui::InputScalar("##num", ImGuiDataType_S64, &val, nullptr, nullptr, "%lld", ImGuiInputTextFlags_EnterReturnsTrue)) {
                val = std::clamp<long long>(val, 0, f.numMax);
                if (!f.setNumber || f.setNumber(val)) f.number = val;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            ImGui::PopFont();
        }
        else {
            dl->AddText(theme::Body(), S(16), ImVec2(fp.x + S(10), fp.y + S(6)), P().textFaint, Lt("ui.unknown"));
            ImGui::Dummy(ImVec2(fieldW, S(30)));
        }
        for (size_t i = 0; i < f.quickAdds.size(); ++i) {
            ImGui::SameLine(0, S(6));
            std::string lbl = Compact(f.quickAdds[i]);
            if (Button(("##qa" + std::to_string(i)).c_str(), lbl.c_str(), btnW, Tone::Neutral, f.numberKnown)) {
                int64_t nv = std::min<int64_t>(f.number + f.quickAdds[i], f.numMax);
                if (!f.setNumber || f.setNumber(nv)) f.number = nv;
            }
        }
        if (f.numMax <= 1000) {
            ImGui::SameLine(0, S(6));
            if (Button("##max", Lt("ui.max"), btnW, Tone::Teal, f.numberKnown))
                if (!f.setNumber || f.setNumber(f.numMax)) f.number = f.numMax;
        }
    }
    else if (f.kind == Kind::Action) {
        if (planned) RightTag(r, "ui.planned", P().planned, 0);
        else {
            float btnW = S(110);
            Slot(r, btnW, S(28));
            if (Button("##run", Lt("ui.set"), btnW, Tone::Teal, f.avail == Avail::Ready && g.host->Live()))
                Registry::Get().Run(f);
            float reserved = btnW;
            if (f.avail != Avail::Ready && g.host->Live()) {
                RightTag(r, f.avail == Avail::Unavailable ? "ui.unavailable" : "ui.pending", f.avail == Avail::Unavailable ? P().red : P().textDim, reserved + S(14));
                reserved += S(110);
            }
            BindHint(f, r, reserved);
        }
    }
    ImGui::PopID();
    RowEnd(r, f.sliders.empty() ? 4 : 0);

    for (size_t i = 0; i < f.sliders.size(); ++i) {
        std::string sid = f.id + "#" + f.sliders[i].id;
        RowCtx sr = RowBegin(sid.c_str(), S(38), false, true, false);
        if (sr.hov) SetFeatureInfo(f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float mx = sr.pos.x + S(28);
        dl->AddLine(ImVec2(mx, sr.pos.y), ImVec2(mx, sr.pos.y + sr.h * 0.5f), P().lineStrong, S(1));
        dl->AddLine(ImVec2(mx, sr.pos.y + sr.h * 0.5f), ImVec2(mx + S(8), sr.pos.y + sr.h * 0.5f), P().lineStrong, S(1));
        std::string lbl = Lt("f." + f.id + "." + f.sliders[i].id);
        RowLabel(sr, lbl.c_str(), planned || !f.on, S(26), 16);
        float bw = S(360);
        Slot(sr, bw, S(26));
        float v = f.values[i];
        ImGui::PushID(sid.c_str());
        if (!f.sliders[i].choices.empty()) {
            std::vector<std::string> items;
            for (auto& k : f.sliders[i].choices) items.push_back(Lt(k));
            int pick = std::clamp((int)std::lround(v - f.sliders[i].min), 0, (int)items.size() - 1);
            if (Dropdown("##pick", items, &pick, bw, !planned)) {
                Registry::Get().SetSlider(f, (int)i, f.sliders[i].min + (float)pick);
                Cfg().MarkDirty();
            }
        }
        else if (EnergyBar("##bar", &v, f.sliders[i].min, f.sliders[i].max, f.sliders[i].fmt, bw, !planned, !f.on)) {
            Registry::Get().SetSlider(f, (int)i, v);
            Cfg().MarkDirty();
        }
        ImGui::PopID();
        RowEnd(sr, i + 1 == f.sliders.size() ? 4 : 0);
    }
}

// Thin title with a gold count, the way the game labels inventories.
bool SectionHeader(const std::string& key, const std::string& title, int on, int total)
{
    bool folds = Cfg().Data().sectionFolds;
    bool& folded = Cfg().Data().folded[key];
    const float h = S(46);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x - S(6);
    ImGui::PushID(key.c_str());
    if (ImGui::InvisibleButton("##sec", ImVec2(w, h)) && folds) { folded = !folded; Cfg().MarkDirty(); }
    bool hov = folds && ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = pos.x + S(2);
    float fs = S(20), sp = S(1.4f);
    float ty = pos.y + h - S(14) - fs;
    if (folds && theme::HasIcons()) {
        std::string chev = Utf8(folded ? icon::ChevRight : icon::ChevDown);
        dl->AddText(theme::Body(), S(11), ImVec2(x, ty + S(5)), hov ? P().text : P().textFaint, chev.c_str());
        x += S(20);
    }
    std::string up = Upper(title.c_str());
    SpacedText(dl, theme::Title(), fs, ImVec2(x, ty), hov ? P().text : P().textBody, up.c_str(), sp);
    if (total > 0) {
        float tx = x + SpacedWidth(theme::Title(), fs, up.c_str(), sp) + S(10);
        char cnt[48];
        snprintf(cnt, sizeof cnt, "(%d/%d %s)", on, total, Lt("ui.active"));
        SpacedText(dl, theme::Title(), fs, ImVec2(tx, ty), P().gold, cnt, sp);
    }
    dl->AddLine(ImVec2(pos.x, pos.y + h - S(6)), ImVec2(pos.x + w, pos.y + h - S(6)), P().lineStrong, S(1));
    ImGui::Dummy(ImVec2(0, S(6)));
    return !(folds && folded);
}

void DrawFeatureList(const std::vector<Feature*>& list, const std::string& scope)
{
    std::vector<std::string> order;
    for (auto* f : list)
        if (std::find(order.begin(), order.end(), f->section) == order.end()) order.push_back(f->section);
    for (auto& sec : order) {
        int on = 0, total = 0;
        for (auto* f : list)
            if (f->section == sec && f->kind == Kind::Toggle && f->avail != Avail::Planned) { ++total; on += f->on; }
        if (SectionHeader(scope + "/" + sec, Lt("sec." + sec), on, total))
            for (auto* f : list)
                if (f->section == sec) FeatureRow(*f);
        ImGui::Dummy(ImVec2(0, S(8)));
    }
}

template <class Fn> void SettingRow(const char* id, const char* label, const char* tip, float ctrlW, float ctrlH, Fn control)
{
    RowCtx r = RowBegin(id, S(46), false, false, false);
    if (r.hov) SetSettingInfo(label, tip, icon::Gear);
    RowLabel(r, label, false);
    Slot(r, ctrlW, ctrlH);
    ImGui::PushID(id);
    control();
    ImGui::PopID();
    RowEnd(r);
}

void BindRow(const std::string& id, const std::string& label, const std::string& tip)
{
    auto& hk = Hotkeys::Get();
    bool capturing = hk.Capturing() == id;
    auto conflicts = hk.ConflictsOf(id);
    std::string text = capturing ? Lt("hk.press") : Hotkeys::Name(hk.Of(id));
    if (text.empty()) text = Lt("hk.none");
    std::string body = tip;
    if (!conflicts.empty()) {
        auto* other = Registry::Get().Find(conflicts[0]);
        body = L::Tf("hk.conflict", other ? FeatureName(*other) : std::string(Lt(conflicts[0] == Hotkeys::MenuId ? "hk.open_menu" : "hk.panic")));
    }
    SettingRow(("bind." + id).c_str(), label.c_str(), body.c_str(), S(200), S(28), [&] {
        if (KeyChip("##chip", text.c_str(), capturing, !conflicts.empty(), S(200))) {
            if (capturing) hk.CancelCapture(); else hk.BeginCapture(id);
            Cfg().MarkDirty();
        }
    });
}

void DrawHotkeys()
{
    if (SectionHeader("hotkeys/menu", Lt("hk.section.menu"), 0, 0)) {
        BindRow(Hotkeys::MenuId, Lt("hk.open_menu"), Lt("set.open_menu.tip"));
        BindRow(Hotkeys::PanicId, Lt("hk.panic"), Lt("set.panic.tip"));
    }
    ImGui::Dummy(ImVec2(0, S(8)));
    for (auto& tab : Registry::Get().Tabs()) {
        std::vector<Feature*> list;
        for (auto* f : Registry::Get().All())
            if (f->tab == tab.id && f->bindable && f->kind != Kind::Number) list.push_back(f);
        if (list.empty()) continue;
        if (SectionHeader("hotkeys/" + tab.id, Lt("tab." + tab.id), 0, 0))
            for (auto* f : list) BindRow(f->id, FeatureName(*f), Lt("hk.note"));
        ImGui::Dummy(ImVec2(0, S(8)));
    }
}

void DrawSettings()
{
    auto& d = Cfg().Data();
    auto toggleRow = [&](const char* id, const char* lk, bool& v, auto&& onChange) {
        SettingRow(id, Lt(lk), Lt(std::string(lk) + ".tip"), S(58), S(24), [&] {
            if (Switch("##sw", &v)) { onChange(); Cfg().MarkDirty(); }
        });
    };
    auto actionRow = [&](const char* id, const char* lk, Tone tone, auto&& fn) {
        SettingRow(id, Lt(lk), Lt(std::string(lk) + ".tip"), S(150), S(30), [&] {
            if (Button("##btn", Lt(std::string(lk) + ".action"), S(150), tone)) fn();
        });
    };
    auto cycleRow = [&](const char* id, const char* lk, const char* value, auto&& fn) {
        SettingRow(id, Lt(lk), Lt(std::string(lk) + ".tip"), S(200), S(30), [&] {
            if (Button("##cyc", value, S(200), Tone::Neutral)) { fn(); Cfg().MarkDirty(); }
        });
    };

    if (SectionHeader("settings/controls", Lt("set.section.controls"), 0, 0)) {
        BindRow(Hotkeys::MenuId, Lt("set.open_menu"), Lt("set.open_menu.tip"));
        BindRow(Hotkeys::PanicId, Lt("set.panic"), Lt("set.panic.tip"));
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    if (SectionHeader("settings/interface", Lt("set.section.interface"), 0, 0)) {
        auto langs = L::Languages();
        int idx = (int)(std::find(langs.begin(), langs.end(), d.language) - langs.begin());
        if (idx >= (int)langs.size()) idx = 0;
        std::string cur = langs[idx] == "en" ? "English" : langs[idx];
        cycleRow("lang", "set.language", cur.c_str(), [&] { d.language = langs[(idx + 1) % langs.size()]; L::Init(d.language); });
        cycleRow("layout", "set.layout", Lt(d.sectionFolds ? "set.layout.folds" : "set.layout.full"), [&] { d.sectionFolds = !d.sectionFolds; });
        SettingRow("scale", Lt("set.scale"), Lt("set.scale.tip"), S(360), S(26), [&] {
            float v = d.scale;
            if (EnergyBar("##scale", &v, 0.7f, 1.6f, "%.2fx", S(360))) { d.scale = v; Cfg().MarkDirty(); }
        });
        cycleRow("accent", "set.accent", Lt(theme::AccentKey(d.accent)), [&] { d.accent = (d.accent + 1) % theme::AccentCount(); });
        toggleRow("tips", "set.tooltips", d.tooltips, [] {});
        toggleRow("block", "set.block", d.blockGameInputs, [] {});
        toggleRow("logos", "set.skip_logos", d.skipLogos, [] {});
        actionRow("winpos", "set.window", Tone::Teal, [] { g.pos = ImVec2(-1, -1); Cfg().Data().windowX = Cfg().Data().windowY = -1; });
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    if (SectionHeader("settings/saved", Lt("set.section.saved"), 0, 0)) {
        toggleRow("autosave", "set.autosave", d.autoSave, [] {});
        toggleRow("remember", "set.remember", d.rememberToggles, [] {});
        actionRow("save", "set.save", Tone::Green, [] { if (Cfg().Save()) Toast(Lt("toast.saved"), "", icon::Check); });
        actionRow("load", "set.load", Tone::Teal, [] {
            if (Cfg().Load()) { Cfg().ApplyToFeatures(Cfg().Data().rememberToggles); L::Init(Cfg().Data().language); Toast(Lt("toast.loaded"), "", icon::Check); }
        });
        SettingRow("defaults", Lt("set.defaults"), Lt("set.defaults.tip"), S(200), S(30), [&] {
            if (HoldButton("##def", Lt("ui.hold"), S(200))) { Cfg().Reset(); L::Init("en"); Toast(Lt("toast.defaults"), "", icon::Warning); }
        });
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    if (SectionHeader("settings/diag", Lt("set.section.diag"), 0, 0)) {
        toggleRow("debug", "set.debug", d.verboseLog, [&] { log::SetVerbose(d.verboseLog); log::Info("Settings", "Debug log {}", d.verboseLog ? "on" : "off"); });
        actionRow("diag", "set.diag", Tone::Teal, [] { g.host->WriteDiagnostics(); Toast(Lt("toast.diag"), "", icon::Info); });
        actionRow("folder", "set.folder", Tone::Teal, [] { g.host->OpenFolder(); });
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    if (SectionHeader("settings/locales", Lt("set.section.locales"), 0, 0)) {
        actionRow("export", "set.export", Tone::Teal, [] {
            std::wstring dir = paths::Root() + L"\\Locales";
            CreateDirectoryW(dir.c_str(), nullptr);
            if (L::ExportEnglish(dir + L"\\en.json")) Toast(Lt("toast.exported"), "Locales\\en.json", icon::Check);
        });
    }
    ImGui::Dummy(ImVec2(0, S(12)));
}

void DrawSearchResults()
{
    std::string q = g.search;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    int shown = 0;
    for (auto& tab : Registry::Get().Tabs()) {
        std::vector<Feature*> hits;
        for (auto* f : Registry::Get().All()) {
            if (f->tab != tab.id) continue;
            std::string hay = FeatureName(*f) + " " + Lt("f." + f->id + ".tip") + " " + Lt("sec." + f->section);
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(q) != std::string::npos) hits.push_back(f);
        }
        if (hits.empty()) continue;
        shown += (int)hits.size();
        if (SectionHeader("search/" + tab.id, Lt("tab." + tab.id), 0, 0))
            for (auto* f : hits) FeatureRow(*f);
        ImGui::Dummy(ImVec2(0, S(8)));
    }
    if (!shown) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        std::string msg = L::Tf("ui.search.empty", g.search);
        ImGui::GetWindowDrawList()->AddText(theme::Body(), S(18), ImVec2(p0.x + S(4), p0.y + S(16)), P().textFaint, msg.c_str());
        ImGui::Dummy(ImVec2(0, S(60)));
    }
}

// ---------- chrome ----------

void DrawPlate(ImVec2 win, float winW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(win.x + S(24), win.y + S(20));
    float w = S(kPlateW);
    // full width red rule the plate sits on
    dl->AddRectFilled(ImVec2(win.x, a.y + S(kPlateRed) - S(2)), ImVec2(win.x + winW, a.y + S(kPlateRed) + S(1)), P().redLine);
    dl->AddRectFilledMultiColor(a, ImVec2(a.x + w, a.y + S(kPlateRed)), P().plate, Mix(P().plate, P().redLine, 0.5f), P().plate, P().plate);
    dl->AddText(theme::Title(), S(28), ImVec2(a.x + S(12), a.y + (S(kPlateRed) - S(28)) * 0.5f), P().text, Lt("app.name"));
    std::string ver = std::string("v") + kVersion;
    ImVec2 vs = theme::Body()->CalcTextSizeA(S(14), FLT_MAX, 0, ver.c_str());
    dl->AddText(theme::Body(), S(14), ImVec2(a.x + w - S(12) - vs.x, a.y + (S(kPlateRed) - vs.y) * 0.5f), Alpha(P().text, 0.75f), ver.c_str());

    ImVec2 d(a.x, a.y + S(kPlateRed) + S(1));
    dl->AddRectFilled(d, ImVec2(d.x + w, d.y + S(kPlateDark)), P().plateDark);
    auto stat = [&](float x, unsigned ic, const std::string& value, const char* caption) {
        float cx = d.x + x;
        if (theme::HasIcons()) {
            std::string s = Utf8(ic);
            dl->AddText(theme::Body(), S(20), ImVec2(cx, d.y + S(10)), P().text, s.c_str());
            cx += S(28);
        }
        dl->AddText(theme::Body(), S(19), ImVec2(cx, d.y + S(4)), P().text, value.c_str());
        dl->AddText(theme::Body(), S(12.5f), ImVec2(cx, d.y + S(26)), P().textDim, caption);
    };
    auto& reg = Registry::Get();
    std::string ready = g.host->Live() ? std::to_string(reg.ReadyCount()) + " / " + std::to_string(reg.TotalCount()) : Lt("ui.status.preview");
    stat(S(10), icon::Bolt, ready, Lt("ui.plate.features"));
    int active = 0;
    for (auto* f : reg.All()) active += f->kind == Kind::Toggle && f->on;
    stat(S(150), icon::Check, std::to_string(active), Lt("ui.plate.active"));
    stat(S(256), icon::Keyboard, Hotkeys::Name(Hotkeys::Get().Of(Hotkeys::MenuId)), Lt("ui.plate.menu"));
}

void DrawStrip(ImVec2 win, float winW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tabs = Registry::Get().Tabs();
    int n = (int)tabs.size(), cur = 0;
    for (int i = 0; i < n; ++i) if (tabs[i].id == g.tab) cur = i;

    float left = win.x + S(24 + kPlateW + 30), right = win.x + winW - S(24 + 250 + 24);
    float cy = win.y + S(42);
    // Up to five entries; drop to fewer until the strip and both key glyphs fit.
    struct Item { int idx; std::string txt; float fs, w; };
    std::vector<Item> items;
    float gap = S(30);
    float gw = KeyGlyphWidth("Q", S(26));
    float aw = S(17); // chevron plus its gap
    float total = 0;
    for (int shown = std::min(n, 5); shown >= 1; shown -= 2) {
        int first = std::clamp(cur - shown / 2, 0, n - shown);
        items.clear();
        total = (gw + aw) * 2 + gap * 2 + gap * (shown - 1);
        for (int i = first; i < first + shown; ++i) {
            std::string t = Upper(Lt("tab." + tabs[i].id));
            float fs = i == cur ? S(30) : S(19);
            items.push_back({ i, t, fs, SpacedWidth(theme::Title(), fs, t.c_str(), S(1.5f)) });
            total += items.back().w;
        }
        if (total <= right - left) break;
    }
    float x = ImMax(left, (left + right - total) * 0.5f);

    float stripX = x;
    // Key glyph with a chevron on its outer side, so the strip reads as scrollable.
    auto glyph = [&](const char* k, int dir) {
        ImVec2 hp(x, cy - S(13));
        ImGui::SetCursorScreenPos(hp);
        ImGui::PushID(k);
        bool clicked = ImGui::InvisibleButton("##g", ImVec2(gw + aw, S(26)));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        ImU32 col = hov ? P().text : P().textDim;
        float gx = dir < 0 ? x + aw : x;
        KeyGlyph(dl, ImVec2(gx, cy - S(13)), k, S(26), col);
        float tip = dir < 0 ? x + S(1) : x + gw + aw - S(1), back = tip - dir * S(7);
        dl->PathLineTo(ImVec2(back, cy - S(7)));
        dl->PathLineTo(ImVec2(tip, cy));
        dl->PathLineTo(ImVec2(back, cy + S(7)));
        dl->PathStroke(col, S(2), 0);
        if (clicked) { g.tab = tabs[(cur + dir + n) % n].id; g.search[0] = 0; g.scrollTop = true; ResetInfo(); }
        x += gw + aw + gap;
    };
    glyph("Q", -1);
    for (size_t j = 0; j < items.size(); ++j) {
        auto& it = items[j];
        bool active = it.idx == cur;
        int dist = std::abs(it.idx - cur);
        ImVec2 tp(x, cy - it.fs * 0.5f - S(2));
        ImGui::SetCursorScreenPos(ImVec2(x, cy - S(18)));
        ImGui::PushID(it.idx);
        bool clicked = ImGui::InvisibleButton("##t", ImVec2(it.w, S(36)));
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        if (clicked && !active) { g.tab = tabs[it.idx].id; g.search[0] = 0; g.scrollTop = true; ResetInfo(); }
        ImU32 col = active && !g.search[0] ? P().text : hov ? P().text : Alpha(P().textBody, dist >= 2 ? 0.55f : 0.85f);
        SpacedText(dl, theme::Title(), it.fs, tp, col, it.txt.c_str(), S(1.5f));
        x += it.w;
        if (j + 1 < items.size()) {
            dl->AddLine(ImVec2(x + gap * 0.5f, cy - S(13)), ImVec2(x + gap * 0.5f, cy + S(13)), P().lineStrong, S(1.5f));
            x += gap;
        }
    }
    x += gap;
    glyph("E", +1);

    // One pip per category, current one lit.
    if (n > (int)items.size()) {
        float pip = S(5), pg = S(7), pw = n * pip + (n - 1) * pg;
        float px = (stripX + x - gap - pw) * 0.5f, py = cy + S(25);
        for (int i = 0; i < n; ++i) {
            ImVec2 a(px + i * (pip + pg), py);
            bool on = i == cur, vis = i >= items.front().idx && i <= items.back().idx;
            dl->AddRectFilled(a, ImVec2(a.x + pip, a.y + pip), on ? P().yellow : vis ? Alpha(P().text, 0.7f) : Alpha(P().text, 0.25f));
        }
    }
}

void DrawSearch(ImVec2 win, float winW)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = S(250);
    ImVec2 a(win.x + winW - S(24) - w, win.y + S(26)), b(a.x + w, a.y + S(32));
    bool active = ImGui::GetActiveID() == ImGui::GetID("##search");
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 110));
    dl->AddRect(a, b, active || g.search[0] ? P().text : P().line, 0.f, S(1));
    if (theme::HasIcons()) {
        std::string ic = Utf8(icon::Search);
        dl->AddText(theme::Body(), S(14), ImVec2(a.x + S(11), a.y + S(9)), P().textDim, ic.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(32), a.y + S(3)));
    ImGui::PushFont(theme::Body(), 17.f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(2), S(4)));
    ImGui::SetNextItemWidth(w - S(40));
    if (ImGui::InputTextWithHint("##search", Lt("ui.search"), g.search, sizeof g.search)) { g.scrollTop = true; ResetInfo(); }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopFont();
}

void DrawSubPages(const TabDef& tab, float rightX, float y)
{
    if (tab.subs.empty()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    std::string& cur = g.sub[tab.id];
    float fs = S(17), sp = S(1.2f), gap = S(22);
    std::vector<float> ws;
    float total = 0;
    for (auto& s : tab.subs) { ws.push_back(SpacedWidth(theme::Title(), fs, Upper(Lt("sub." + s)).c_str(), sp)); total += ws.back(); }
    total += gap * (tab.subs.size() - 1);
    float x = rightX - total;
    for (size_t i = 0; i < tab.subs.size(); ++i) {
        const auto& s = tab.subs[i];
        std::string up = Upper(Lt("sub." + s));
        ImGui::SetCursorScreenPos(ImVec2(x - S(6), y - S(4)));
        ImGui::PushID(s.c_str());
        if (ImGui::InvisibleButton("##sub", ImVec2(ws[i] + S(12), S(28)))) { cur = s; g.scrollTop = true; ResetInfo(); }
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool act = cur == s;
        SpacedText(dl, theme::Title(), fs, ImVec2(x, y), act ? P().gold : hov ? P().text : P().textDim, up.c_str(), sp);
        if (act) dl->AddRectFilled(ImVec2(x, y + fs + S(5)), ImVec2(x + ws[i], y + fs + S(7)), P().gold);
        x += ws[i];
        if (i + 1 < tab.subs.size()) {
            dl->AddLine(ImVec2(x + gap * 0.5f, y + S(1)), ImVec2(x + gap * 0.5f, y + fs), P().lineStrong, S(1));
            x += gap;
        }
    }
}

void DrawList(ImVec2 a, ImVec2 b)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const TabDef* tab = TabOf(g.tab);
    if (!tab) tab = &Registry::Get().Tabs()[0];
    std::string& sub = g.sub[tab->id];
    if (sub.empty() && !tab->subs.empty()) sub = tab->subs[0];
    const Page* page = PageOf(tab->id);
    bool searching = g.search[0] != 0 && !page;

    // header: "EXOSUIT (3/5 ACTIVE)" with the page switcher on the right
    std::string title = Upper(searching ? Lt("ui.results") : Lt("tab." + tab->id));
    float fs = S(30), sp = S(1.6f);
    SpacedText(dl, theme::Title(), fs, ImVec2(a.x, a.y), P().text, title.c_str(), sp);
    if (page && page->badge) {
        std::string cnt = page->badge();
        SpacedText(dl, theme::Title(), fs, ImVec2(a.x + SpacedWidth(theme::Title(), fs, title.c_str(), sp) + S(12), a.y), P().gold, cnt.c_str(), sp);
    }
    else if (!searching && !page && tab->id != "hotkeys" && tab->id != "settings") {
        int on = 0, total = 0;
        for (auto* f : Registry::Get().All())
            if (f->tab == tab->id && f->kind == Kind::Toggle && f->avail != Avail::Planned) { ++total; on += f->on; }
        char cnt[48];
        snprintf(cnt, sizeof cnt, "(%d/%d %s)", on, total, Lt("ui.active"));
        SpacedText(dl, theme::Title(), fs, ImVec2(a.x + SpacedWidth(theme::Title(), fs, title.c_str(), sp) + S(12), a.y), P().gold, cnt, sp);
    }
    if (!searching) DrawSubPages(*tab, b.x, a.y + S(9));
    dl->AddLine(ImVec2(a.x - S(8), a.y + fs + S(12)), ImVec2(b.x, a.y + fs + S(12)), P().lineStrong, S(1.5f));

    float top = a.y + fs + S(18);
    ImGui::SetCursorScreenPos(ImVec2(a.x, top));
    ImGui::BeginChild("##list", ImVec2(b.x - a.x + S(6), b.y - top), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (g.scrollTop) { ImGui::SetScrollY(0); g.scrollTop = false; }
    if (page) page->list(g.search);
    else if (searching) DrawSearchResults();
    else if (tab->id == "hotkeys") DrawHotkeys();
    else if (tab->id == "settings") DrawSettings();
    else {
        std::vector<Feature*> list;
        for (auto* f : Registry::Get().All())
            if (f->tab == tab->id && (tab->subs.empty() || f->sub == sub)) list.push_back(f);
        DrawFeatureList(list, tab->id + "/" + sub);
    }
    ImGui::Dummy(ImVec2(0, S(12)));
    ImGui::EndChild();
}

// Item inspector: teal title band, warm body, action lines.
void DrawInfo(ImVec2 a, float w)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& p = P();
    Info info = g.info;
    if (!g.infoSet) {
        const TabDef* tab = TabOf(g.tab);
        info = {};
        info.icon = tab ? tab->icon : 0;
        info.title = Lt("tab." + g.tab);
        info.subtitle = Lt("app.name");
        info.body = Lt("tabdesc." + g.tab);
    }

    const Page* page = PageOf(g.tab);
    float headH = S(86);
    if (page && page->inspector && !(g.search[0] && !page)) {
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        float bottom = page->inspector(a, w);
        dl->ChannelsSetCurrent(0);
        if (bottom > a.y + headH) {
            dl->AddRectFilled(a, ImVec2(a.x + w, a.y + headH), p.teal);
            dl->AddRectFilled(ImVec2(a.x, a.y + headH), ImVec2(a.x + w, bottom), p.warm);
            dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y + S(3)), Alpha(p.blueText, 0.6f));
        }
        dl->ChannelsMerge();
        if (bottom > a.y + headH) return;
    }
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    float pad = S(22);
    // header
    float ib = S(64);
    ImVec2 ia(a.x + S(11), a.y + (headH - ib) * 0.5f);
    dl->AddRectFilled(ia, ImVec2(ia.x + ib, ia.y + ib), IM_COL32(10, 34, 44, 255));
    dl->AddRect(ia, ImVec2(ia.x + ib, ia.y + ib), Alpha(p.blueText, 0.5f), 0.f, S(1));
    if (info.icon && theme::HasIcons()) {
        std::string s = Utf8(info.icon);
        ImVec2 is = theme::Body()->CalcTextSizeA(S(30), FLT_MAX, 0, s.c_str());
        dl->AddText(theme::Body(), S(30), ImVec2(ia.x + (ib - is.x) * 0.5f, ia.y + (ib - is.y) * 0.5f), p.text, s.c_str());
    }
    float tx = ia.x + ib + S(16);
    std::string up = Upper(info.title.c_str());
    float tfs = S(25);
    while (tfs > S(17) && SpacedWidth(theme::Title(), tfs, up.c_str(), S(1.5f)) > a.x + w - tx - S(14)) tfs -= S(1);
    SpacedText(dl, theme::Title(), tfs, ImVec2(tx, a.y + S(18)), p.text, up.c_str(), S(1.5f));
    dl->AddText(theme::Body(), S(17), ImVec2(tx, a.y + S(50)), p.textBody, info.subtitle.c_str());

    // body
    float y = a.y + headH + pad;
    float bw = w - pad * 2;
    if (Cfg().Data().tooltips || !info.feature) {
        dl->AddText(theme::Body(), S(18), ImVec2(a.x + pad, y), p.textBody, info.body.c_str(), nullptr, bw);
        y += theme::Body()->CalcTextSizeA(S(18), FLT_MAX, bw, info.body.c_str()).y + pad;
    }

    auto sep = [&] { dl->AddLine(ImVec2(a.x + pad, y), ImVec2(a.x + w - pad, y), p.lineStrong, S(1.5f)); y += pad; };
    auto action = [&](unsigned ic, const char* title, const std::string& desc) {
        float x = a.x + pad;
        if (theme::HasIcons()) {
            std::string s = Utf8(ic);
            dl->AddText(theme::Body(), S(20), ImVec2(x, y + S(2)), p.text, s.c_str());
            x += S(30);
        }
        SpacedText(dl, theme::Title(), S(21), ImVec2(x, y), p.text, Upper(title).c_str(), S(1.5f));
        dl->AddText(theme::Body(), S(17), ImVec2(a.x + pad, y + S(30)), p.textDim, desc.c_str(), nullptr, bw);
        y += S(30) + theme::Body()->CalcTextSizeA(S(17), FLT_MAX, bw, desc.c_str()).y + S(16);
    };

    const Feature* f = info.feature;
    if (f) {
        sep();
        if (f->avail == Avail::Planned) {
            action(icon::Clock, Lt("info.research"), Lt("ui.planned.note"));
        }
        else if (f->kind == Kind::Toggle) {
            dl->AddText(theme::Body(), S(26), ImVec2(a.x + pad, y), p.text, Lt("info.status"));
            const char* st = Lt(f->on ? "info.active" : "info.inactive");
            ImVec2 ss = theme::Body()->CalcTextSizeA(S(20), FLT_MAX, 0, st);
            dl->AddText(theme::Body(), S(20), ImVec2(a.x + w - pad - ss.x, y + S(5)), f->on ? p.yellow : p.textDim, st);
            y += S(38);
            float bh = S(10);
            dl->AddRectFilled(ImVec2(a.x + pad, y), ImVec2(a.x + w - pad, y + bh), f->on ? p.yellow : Alpha(p.yellowDim, 0.5f));
            y += bh + pad;
            sep();
            Chord c = Hotkeys::Get().Of(f->id);
            action(icon::Power, Lt("info.toggle"), c.Empty() ? std::string(Lt("info.toggle.click")) : L::Tf("info.toggle.key", Hotkeys::Name(c)));
            action(icon::Keyboard, Lt("info.hotkey"), c.Empty() ? std::string(Lt("info.nokey")) : Hotkeys::Name(c));
            if (f->avail == Avail::Unavailable) action(icon::Warning, Lt("ui.unavailable"), f->reason.empty() ? Lt("ui.unavailable.note") : f->reason);
        }
        else if (f->kind == Kind::Number) {
            dl->AddText(theme::Body(), S(26), ImVec2(a.x + pad, y), p.text, Lt("info.value"));
            std::string v = f->numberKnown ? Grouped(f->number) : std::string(Lt("ui.unknown"));
            ImVec2 vs = theme::Body()->CalcTextSizeA(S(22), FLT_MAX, 0, v.c_str());
            dl->AddText(theme::Body(), S(22), ImVec2(a.x + w - pad - vs.x, y + S(3)), p.gold, v.c_str());
            y += S(38) + pad * 0.5f;
            sep();
            action(icon::Plus, Lt("info.set"), Lt("info.set.desc"));
        }
        else if (f->kind == Kind::Action) {
            Chord c = Hotkeys::Get().Of(f->id);
            action(icon::Power, Lt("ui.set"), c.Empty() ? std::string(Lt("info.toggle.click")) : L::Tf("info.toggle.key", Hotkeys::Name(c)));
            if (f->bindable) action(icon::Keyboard, Lt("info.hotkey"), c.Empty() ? std::string(Lt("info.nokey")) : Hotkeys::Name(c));
            if (f->avail == Avail::Unavailable) action(icon::Warning, Lt("ui.unavailable"), f->reason.empty() ? Lt("ui.unavailable.note") : f->reason);
        }
    }
    y += S(4);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + headH), p.teal);
    dl->AddRectFilled(ImVec2(a.x, a.y + headH), ImVec2(a.x + w, y), p.warm);
    dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + w, a.y + S(3)), Alpha(p.blueText, 0.6f));
    dl->ChannelsMerge();
}

void DrawBottom(ImVec2 win, float winW, float winH)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = win.y + winH - S(kBottom) + S(10);
    std::string status = std::string(g.host->BackendName()) + "   /   " + g.host->GameBuild() + "   /   ";
    std::string ver = std::string("v") + kVersion;
    // the main build signs with the logo image; text until it is uploaded, and in the forum build
    float aspect = 0;
    ImTextureID logo = std::string(Lt("ui.credit.logo")) == "1" ? g.host->CreditLogo(aspect) : ImTextureID{};
    ImVec2 at(win.x + S(24), y + S(10));
    if (logo) {
        status += L::Tf("ui.credit.short", ver) + "   /   ";
        dl->AddText(theme::Body(), S(16), at, P().textFaint, status.c_str());
        float tw = theme::Body()->CalcTextSizeA(S(16), FLT_MAX, 0, status.c_str()).x, lh = S(34);
        ImVec2 la(at.x + tw, at.y + S(8) - lh * 0.5f);
        dl->AddImage(logo, la, ImVec2(la.x + lh * aspect, la.y + lh), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 200));
    }
    else {
        status += L::Tf("ui.credit", ver);
        dl->AddText(theme::Body(), S(16), at, P().textFaint, status.c_str());
    }

    // CLOSE [F8] pill, same as the game's RETURN prompt
    std::string key = Hotkeys::Name(Hotkeys::Get().Of(Hotkeys::MenuId));
    const char* lbl = Lt("ui.close");
    std::string up = Upper(lbl);
    float fs = S(19), kh = S(24);
    float tw = SpacedWidth(theme::Title(), fs, up.c_str(), S(1.5f));
    float kw = KeyGlyphWidth(key.c_str(), kh);
    float w = tw + kw + S(40);
    ImVec2 a(win.x + winW - S(24) - w, y), b(a.x + w, y + S(34));
    ImGui::SetCursorScreenPos(a);
    if (ImGui::InvisibleButton("##close", ImVec2(w, S(34)))) SetOpen(false);
    bool hov = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, hov ? Alpha(P().teal, 0.9f) : IM_COL32(0, 0, 0, 120), S(17));
    SpacedText(dl, theme::Title(), fs, ImVec2(a.x + S(16), a.y + (S(34) - fs) * 0.5f - S(1)), P().text, up.c_str(), S(1.5f));
    KeyGlyph(dl, ImVec2(a.x + S(16) + tw + S(10), a.y + (S(34) - kh) * 0.5f), key.c_str(), kh, P().text);
}

void DrawToasts()
{
    double now = ImGui::GetTime();
    while (!g_toasts.empty() && now - g_toasts.front().t0 > 3.4) g_toasts.pop_front();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    float y = S(30);
    for (auto& t : g_toasts) {
        float age = (float)(now - t.t0);
        float k = ImMin(ImClamp(age / 0.22f, 0.f, 1.f), ImClamp((3.4f - age) / 0.35f, 0.f, 1.f));
        float ease = 1 - (1 - k) * (1 - k);
        float w = S(380), head = S(44), body = t.body.empty() ? 0 : S(34);
        float x = disp.x - S(30) - w + (1 - ease) * S(40);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + head), Alpha(P().teal, k));
        if (body > 0) dl->AddRectFilled(ImVec2(x, y + head), ImVec2(x + w, y + head + body), Alpha(P().warm, k));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + S(3)), Alpha(P().blueText, 0.7f * k));
        float tx = x + S(16);
        if (t.icon && theme::HasIcons()) {
            std::string ic = Utf8(t.icon);
            dl->AddText(theme::Body(), S(20), ImVec2(x + S(16), y + (head - S(20)) * 0.5f), Alpha(P().text, k), ic.c_str());
            tx = x + S(48);
        }
        std::string up = Upper(t.title.c_str());
        SpacedText(dl, theme::Title(), S(20), ImVec2(tx, y + (head - S(20)) * 0.5f - S(1)), Alpha(P().text, k), up.c_str(), S(1.4f));
        if (body > 0) dl->AddText(theme::Body(), S(16), ImVec2(x + S(16), y + head + S(8)), Alpha(P().textBody, k), t.body.c_str());
        y += head + body + S(10);
    }
}

void HandleKeys()
{
    if (ImGui::GetIO().WantTextInput || !Hotkeys::Get().Capturing().empty()) return;
    const auto& tabs = Registry::Get().Tabs();
    int n = (int)tabs.size(), cur = 0;
    for (int i = 0; i < n; ++i) if (tabs[i].id == g.tab) cur = i;
    int dir = ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1 : 0;
    if (dir) { g.tab = tabs[(cur + dir + n) % n].id; g.search[0] = 0; g.scrollTop = true; ResetInfo(); }
}

void ApplyLook()
{
    auto& d = Cfg().Data();
    if (d.accent != g.appliedAccent) { theme::SetAccent(d.accent); g.appliedAccent = d.accent; g.appliedScale = 0; }
    if (d.scale != g.appliedScale && !ImGui::IsMouseDown(0)) { theme::ApplyStyle(d.scale); g.appliedScale = d.scale; }
}

}

void Init(Host* host)
{
    g.host = host;
    g.pos = ImVec2(Cfg().Data().windowX, Cfg().Data().windowY);
}

bool IsOpen() { return g.open; }

void SetOpen(bool open)
{
    if (g.open == open) return;
    g.open = open;
    Hotkeys::Get().CancelCapture();
    if (!open && Cfg().Data().autoSave && Cfg().Dirty()) Cfg().Save();
    if (g.host) g.host->OnOpenChanged(open);
}

void ToggleOpen() { SetOpen(!g.open); }

void TriggerBinding(const std::string& id)
{
    if (id == Hotkeys::MenuId) { ToggleOpen(); return; }
    if (id == Hotkeys::PanicId) { Registry::Get().DisableAll(); Toast(Lt("toast.panic"), "", icon::Power); return; }
    if (auto* f = Registry::Get().Find(id)) {
        if (f->kind == Kind::Toggle && f->avail != Avail::Planned) ToggleFeature(*f, !f->on, true);
        else if (f->kind == Kind::Action) Registry::Get().Run(*f);
    }
}

void Toast(const std::string& title, const std::string& body, unsigned icon)
{
    g_toasts.push_back({ title, body, icon, ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0 });
    if (g_toasts.size() > 4) g_toasts.pop_front();
}

void SelectTab(const std::string& tab, const std::string& sub)
{
    g.tab = tab;
    if (!sub.empty()) g.sub[tab] = sub;
    g.search[0] = 0;
    g.scrollTop = true;
    ResetInfo();
}

void RegisterPage(const std::string& tab, Page page) { g_pages[tab] = std::move(page); }

void SetSearch(const std::string& text) { strncpy(g.search, text.c_str(), sizeof g.search - 1); }
void ForceHover(const std::string& id) { g.forcedHover = id; }
void SetWindowPos(float x, float y) { g.pos = ImVec2(x, y); }

void Draw()
{
    ApplyLook();
    if (g.open) {
        HandleKeys();
        ImVec2 disp = ImGui::GetIO().DisplaySize;
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), disp, P().backdrop);

        ImVec2 size(S(kW), S(kH));
        if (g.pos.x < 0 || g.pos.y < 0) g.pos = ImVec2((disp.x - size.x) * 0.5f, (disp.y - size.y) * 0.5f);
        g.pos.x = ImClamp(g.pos.x, -size.x + S(160), disp.x - S(160));
        g.pos.y = ImClamp(g.pos.y, 0.f, disp.y - S(80));

        ImGui::SetNextWindowPos(g.pos);
        ImGui::SetNextWindowSize(size);
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar;
        ImGui::Begin("##OpenSkies", nullptr, fl);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a = g.pos, b(a.x + size.x, a.y + size.y);
        dl->AddRectFilledMultiColor(a, b, P().panel, P().panel, P().panelLow, P().panelLow);

        // drag by the empty top band
        ImGui::SetCursorScreenPos(ImVec2(a.x, a.y));
        ImGui::InvisibleButton("##drag", ImVec2(size.x, S(18)));
        if (ImGui::IsItemActive()) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            g.pos.x += d.x; g.pos.y += d.y;
            Cfg().Data().windowX = g.pos.x; Cfg().Data().windowY = g.pos.y;
        }

        DrawPlate(a, size.x);
        DrawStrip(a, size.x);
        DrawSearch(a, size.x);
        // a page can ask for a wider inspector (the Garage, for its preview); the list gives up the room
        const Page* pg = PageOf(g.tab);
        float infoW = pg && pg->inspectorWidth > 0 ? pg->inspectorWidth : kInfoW;
        float listL = a.x + S(24), listR = b.x - S(24 + infoW + 28);
        DrawList(ImVec2(listL, a.y + S(kContentTop)), ImVec2(listR, b.y - S(kBottom)));
        DrawInfo(ImVec2(b.x - S(24 + infoW), a.y + S(kContentTop) + S(6)), S(infoW));
        DrawBottom(a, size.x, size.y);
        ImGui::End();
    }
    DrawToasts();
}

}
