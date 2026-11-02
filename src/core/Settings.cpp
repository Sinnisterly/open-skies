#include "core/Settings.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include "core/Features.h"
#include "core/Hotkeys.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <windows.h>

using nlohmann::json;

namespace os {

Settings& Settings::Get()
{
    static Settings s;
    return s;
}

static std::wstring SettingsPath() { return paths::File(L"OpenSkies.settings.json"); }

bool Settings::Load()
{
    std::ifstream f(SettingsPath(), std::ios::binary);
    if (!f) { log::Info("Settings", "No settings file yet, using defaults"); return false; }
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) { log::Warn("Settings", "Settings file is not valid JSON, using defaults"); return false; }

    SettingsData d;
    d.version = j.value("version", 1);
    d.language = j.value("language", d.language);
    d.scale = j.value("scale", d.scale);
    d.windowX = j.value("windowX", d.windowX);
    d.windowY = j.value("windowY", d.windowY);
    d.accent = j.value("accent", d.accent);
    d.sectionFolds = j.value("sectionFolds", d.sectionFolds);
    d.tooltips = j.value("tooltips", d.tooltips);
    d.blockGameInputs = j.value("blockGameInputs", d.blockGameInputs);
    d.skipLogos = j.value("skipLogos", d.skipLogos);
    d.autoSave = j.value("autoSave", d.autoSave);
    d.rememberToggles = j.value("rememberToggles", d.rememberToggles);
    d.verboseLog = j.value("verboseLog", d.verboseLog);
    d.glyphMode = std::clamp(j.value("glyphMode", d.glyphMode), 0, 2);
    d.previewSpin = j.value("previewSpin", d.previewSpin);
    d.markers = j.value("markers", d.markers);
    d.markTeleports = j.value("markTeleports", d.markTeleports);
    d.markBases = j.value("markBases", d.markBases);
    d.markerRange = std::clamp(j.value("markerRange", d.markerRange), 0.f, 100000.f);
    d.markPois = j.value("markPois", d.markPois);
    d.poiGroups = j.value("poiGroups", d.poiGroups) & 0x1F;
    if (j.contains("markerStyles") && j["markerStyles"].is_array())
        for (size_t i = 0; i < j["markerStyles"].size() && i < d.markerStyles.size(); ++i) {
            const json& m = j["markerStyles"][i];
            auto& s = d.markerStyles[i];
            s.icon = m.value("icon", 0u);
            s.name = m.value("name", 0u);
            s.dist = m.value("dist", 0u);
            s.showName = m.value("showName", true);
            s.showDist = m.value("showDist", true);
        }
    if (j.contains("toggles")) d.toggles = j["toggles"].get<std::map<std::string, bool>>();
    if (j.contains("sliders")) d.sliders = j["sliders"].get<std::map<std::string, std::vector<float>>>();
    if (j.contains("folded")) d.folded = j["folded"].get<std::map<std::string, bool>>();
    m_data = std::move(d);

    auto& hk = Hotkeys::Get();
    hk.Reset();
    if (j.contains("hotkeys"))
        for (auto& [id, v] : j["hotkeys"].items())
            hk.Set(id, Chord{ v.value<uint16_t>("vk", 0), v.value<uint8_t>("mods", 0) });
    if (hk.Of(Hotkeys::MenuId).Empty()) hk.Set(Hotkeys::MenuId, { 0x77, 0 });

    log::SetVerbose(m_data.verboseLog);
    m_dirty = false;
    log::Info("Settings", "Loaded {} hotkeys, {} toggles", hk.All().size(), m_data.toggles.size());
    return true;
}

bool Settings::Save()
{
    CaptureFeatures();
    const auto& d = m_data;
    json j = {
        { "version", d.version }, { "language", d.language }, { "scale", d.scale },
        { "windowX", d.windowX }, { "windowY", d.windowY }, { "accent", d.accent },
        { "sectionFolds", d.sectionFolds }, { "tooltips", d.tooltips },
        { "blockGameInputs", d.blockGameInputs }, { "skipLogos", d.skipLogos }, { "autoSave", d.autoSave },
        { "rememberToggles", d.rememberToggles }, { "verboseLog", d.verboseLog },
        { "glyphMode", d.glyphMode }, { "previewSpin", d.previewSpin },
        { "markers", d.markers }, { "markTeleports", d.markTeleports }, { "markBases", d.markBases }, { "markerRange", d.markerRange },
        { "markPois", d.markPois }, { "poiGroups", d.poiGroups },
        { "markerStyles", [&] {
             json a = json::array();
             for (auto& s : d.markerStyles)
                 a.push_back({ { "icon", s.icon }, { "name", s.name }, { "dist", s.dist }, { "showName", s.showName }, { "showDist", s.showDist } });
             return a;
         }() },
        { "toggles", d.toggles }, { "sliders", d.sliders }, { "folded", d.folded },
    };
    json hk = json::object();
    for (auto& [id, c] : Hotkeys::Get().All()) hk[id] = { { "vk", c.vk }, { "mods", c.mods } };
    j["hotkeys"] = hk;

    std::wstring tmp = SettingsPath() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { log::Error("Settings", "Cannot write {}", paths::Utf8(tmp)); return false; }
        f << j.dump(2);
    }
    if (!MoveFileExW(tmp.c_str(), SettingsPath().c_str(), MOVEFILE_REPLACE_EXISTING)) {
        log::Error("Settings", "Cannot replace settings file ({})", GetLastError());
        return false;
    }
    m_dirty = false;
    log::Debug("Settings", "Saved");
    return true;
}

void Settings::Reset()
{
    for (auto* f : Registry::Get().All()) {
        if (f->kind == Kind::Toggle && f->on) Registry::Get().SetOn(*f, false);
        for (size_t i = 0; i < f->sliders.size(); ++i) Registry::Get().SetSlider(*f, (int)i, f->sliders[i].def);
    }
    m_data = SettingsData{};
    Hotkeys::Get().Reset();
    log::SetVerbose(false);
    m_dirty = true;
}

void Settings::CaptureFeatures()
{
    m_data.toggles.clear();
    m_data.sliders.clear();
    for (auto* f : Registry::Get().All()) {
        if (f->kind == Kind::Toggle && f->on) m_data.toggles[f->id] = true;
        if (!f->values.empty()) m_data.sliders[f->id] = f->values;
    }
}

void Settings::ApplyToFeatures(bool includeToggles)
{
    auto& r = Registry::Get();
    for (auto& [id, vals] : m_data.sliders)
        if (auto* f = r.Find(id))
            for (size_t i = 0; i < vals.size() && i < f->values.size(); ++i) r.SetSlider(*f, (int)i, vals[i]);
    if (!includeToggles) return;
    for (auto& [id, on] : m_data.toggles)
        if (auto* f = r.Find(id); f && on) r.SetOn(*f, true);
}

}
