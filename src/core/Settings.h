#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace os {

// One HUD marker kind's look. Colours are 0xAABBGGRR, 0 = the theme's colour for that kind.
struct MarkerStyle
{
    uint32_t icon = 0, name = 0, dist = 0;
    bool showName = true, showDist = true;
};
constexpr int kMarkerKinds = 7; // teleport, base, then the POI groups: buildings, crashes, ancient, settlements, sentinels

struct SettingsData
{
    int version = 1;
    std::string language = "en";
    float scale = 1.0f;
    float windowX = -1, windowY = -1;
    int accent = 0;
    bool sectionFolds = true;
    bool tooltips = true;
    bool blockGameInputs = true;
    bool skipLogos = true;
    bool autoSave = true;
    bool rememberToggles = false;
    bool verboseLog = false;
    int glyphMode = 2;        // address pad: 0 glyphs, 1 hex digits, 2 both
    bool previewSpin = false; // Garage preview turns on its own
    bool markers = true, markTeleports = true, markBases = true; // in-world HUD markers
    float markerRange = 10.f; // km, 0 = no limit
    bool markPois = true;
    std::array<MarkerStyle, kMarkerKinds> markerStyles{};
    int poiGroups = 0x1F; // bits: buildings, crashes and distress, ancient, settlements, sentinels
    std::map<std::string, bool> toggles;
    std::map<std::string, std::vector<float>> sliders;
    std::map<std::string, bool> folded;
};

class Settings
{
public:
    static Settings& Get();
    SettingsData& Data() { return m_data; }

    bool Load();
    bool Save();
    void Reset();
    void MarkDirty() { m_dirty = true; }
    bool Dirty() const { return m_dirty; }

    // Copies feature state into Data and back.
    void CaptureFeatures();
    void ApplyToFeatures(bool includeToggles);

private:
    SettingsData m_data;
    bool m_dirty = false;
};

}
