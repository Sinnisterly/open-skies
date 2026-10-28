#pragma once
#include <map>
#include <string>
#include <vector>

namespace os {

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
