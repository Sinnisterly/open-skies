#pragma once
#include <imgui.h>
#include <string>

namespace os::ui {

// Colors sampled from the game's own menus.
struct Palette
{
    ImU32 backdrop;    // dims the world behind the menu
    ImU32 panel;       // window body
    ImU32 panelLow;
    ImU32 row;         // list row fill
    ImU32 rowHover;    // teal selection band
    ImU32 plate;       // red name plate
    ImU32 plateDark;
    ImU32 redLine;     // full width rule under the plate
    ImU32 teal;        // info panel header
    ImU32 warm;        // info panel body
    ImU32 blue;        // subtitle bands
    ImU32 blueText;
    ImU32 gold;        // counts and highlights (accent)
    ImU32 yellow;      // energy bar fill
    ImU32 yellowDim;   // energy bar remainder
    ImU32 green, greenDeep;
    ImU32 red, redDeep;
    ImU32 text;        // titles
    ImU32 textBody;    // paragraph text
    ImU32 textDim;
    ImU32 textFaint;
    ImU32 line;
    ImU32 lineStrong;
    ImU32 planned;
};

namespace theme {

const Palette& P();
void SetAccent(int preset); // 0 gold, 1 cyan, 2 violet, 3 white
int AccentCount();
const char* AccentKey(int preset);

// Title font is the game's Futura (the one with the open A), body is Roboto.
void LoadFonts(const std::wstring& gameRoot);
ImFont* Title();
ImFont* Body();
ImFont* Mono();
bool HasIcons();
const std::string& FontReport();

void ApplyStyle(float scale);
float S(float v);

}

ImU32 Alpha(ImU32 c, float a);
ImU32 Mix(ImU32 a, ImU32 b, float t);

}
