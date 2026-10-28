#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace os {

enum class Kind { Toggle, Action, Number };
enum class Avail { Ready, Pending, Unavailable, Planned };

struct Slider
{
    std::string id;
    float min, max, def;
    const char* fmt = "%.1fx";
    std::vector<std::string> choices; // locale keys; when set the slider is a dropdown of whole values
};

struct Feature
{
    std::string id, tab, sub, section;
    Kind kind = Kind::Toggle;
    std::vector<Slider> sliders; // keep to 3 or fewer
    bool danger = false;
    bool bindable = true;
    int64_t numMax = 0;
    std::vector<int64_t> quickAdds;

    bool on = false;
    std::vector<float> values;
    int64_t number = 0;
    bool numberKnown = false;
    Avail avail = Avail::Pending;
    std::string reason;

    std::function<bool(bool)> onToggle;
    std::function<void()> onAction;
    std::function<void(int, float)> onSlider;
    std::function<bool(int64_t&)> getNumber;
    std::function<bool(int64_t)> setNumber;
};

struct TabDef
{
    std::string id;
    unsigned icon;
    std::vector<std::string> subs;
};

class Registry
{
public:
    static Registry& Get();

    Feature& Add(Feature f);
    void AddTab(TabDef t) { m_tabs.push_back(std::move(t)); }
    Feature* Find(const std::string& id);
    const std::vector<TabDef>& Tabs() const { return m_tabs; }
    std::vector<Feature*> All();

    // Safe from any thread; applied on the render thread by Pump().
    void Post(std::function<void()> fn);
    void Pump();

    void SetOn(Feature& f, bool on);
    void SetSlider(Feature& f, int idx, float v);
    void Run(Feature& f);
    void DisableAll();

    int ReadyCount() const;
    int TotalCount() const;

private:
    std::vector<std::unique_ptr<Feature>> m_features;
    std::vector<TabDef> m_tabs;
    std::mutex m_mx;
    std::vector<std::function<void()>> m_queue;
};

// Declares every tab and feature. Game code attaches callbacks later.
void RegisterCatalog(Registry& r);

}
