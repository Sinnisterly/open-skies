#pragma once
#include <imgui.h>
#include <functional>
#include <set>
#include <utility>
#include <string>
#include <vector>

namespace os::ui {

// One selectable thing in a browser grid (item, blueprint, ship, expedition).
struct BrowserEntry
{
    std::string key;       // stable id
    std::string title, subtitle, body;
    std::string icon;      // game texture path, may be empty
    unsigned glyph = 0;    // Font Awesome fallback when there is no icon
    int chip = 0;          // index into Browser::chips (0 is "all")
    bool marked = false;   // already owned or known
    std::string group;     // type name, used by sort and group
    double value = -1;     // base value, -1 when unknown
    double localValue = -1; // value in the current system, -1 when unknown
    int rarity = -1;
    bool rare = false;     // star marker in the corner
    ImU32 tint = 0;        // glyph colour, 0 keeps the default
    std::string count;     // stack size in the bottom corner, like the game's slots
};

enum class SortBy { Default, Name, Type, Value, LocalValue, Rarity };

// Searchable icon grid with category chips and single or multi select, styled like the game's inventory.
class Browser
{
public:
    std::vector<std::string> chips; // chip 0 is "all"
    bool multi = false;
    bool hideMarked = false;
    bool searchBox = false; // own search field above the chips
    bool sortable = false;  // sort dropdown and group switch
    std::vector<std::pair<SortBy, std::string>> sortOptions; // this page's sorts and labels; empty = all of them
    bool captions = false;  // title under each tile
    int hideFromAll = -1;   // a chip whose entries only show under that chip, not under "all"

    // Draws chips and grid into the current list area. `query` is the menu's search text.
    void Draw(const std::vector<BrowserEntry>& all, const char* query);

    const BrowserEntry* Focused(const std::vector<BrowserEntry>& all) const;
    const std::set<std::string>& Selected() const { return m_sel; }
    void SetFocus(const std::string& key) { m_focus = key; }
    void ClearFocus() { m_focus.clear(); }
    void ClearSelection() { m_sel.clear(); m_focus.clear(); }
    void SelectVisible();
    size_t VisibleCount() const { return m_view.size(); }
    void Invalidate() { m_dirty = true; }

private:
    void Filter(const std::vector<BrowserEntry>& all, const std::string& q);
    int m_chip = 0, m_sort = 0, m_lastSort = 0;
    bool m_group = false, m_lastGroup = false;
    std::string m_query;
    const void* m_src = nullptr;
    size_t m_srcSize = 0;
    bool m_dirty = true, m_lastHide = false;
    std::vector<int> m_view;
    std::vector<std::string> m_viewKeys;
    std::set<std::string> m_sel;
    std::string m_focus;
    char m_text[96] = {};
};

// Texture source for icons, set by the host.
void SetIconLoader(std::function<ImTextureID(const std::string&)> fn);
ImTextureID LoadIcon(const std::string& path);

// Inspector header for an entry: large icon, title, subtitle, body. Returns the y below it.
float DrawEntryHeader(ImDrawList* dl, ImVec2 a, float w, const BrowserEntry& e);

// Game text with colour tags like <TECHNOLOGY>Refiner<>. Returns the height drawn.
float RichText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float wrap, ImU32 base, const std::string& text);
std::string StripTags(const std::string& text);

}
