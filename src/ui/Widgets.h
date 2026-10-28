#pragma once
#include <imgui.h>
#include <string>
#include <vector>

namespace os::ui {

std::string Utf8(unsigned codepoint);
std::string Upper(const char* s);

float SpacedWidth(ImFont* f, float size, const char* text, float spacing);
void SpacedText(ImDrawList* dl, ImFont* f, float size, ImVec2 pos, ImU32 col, const char* text, float spacing);

// Boxed key letter like the game's [Q] / [E] prompts. Returns its width.
float KeyGlyph(ImDrawList* dl, ImVec2 pos, const char* key, float height, ImU32 col);
float KeyGlyphWidth(const char* key, float height);

enum class Tone { Teal, Green, Red, Neutral };

// Controls draw at the current cursor and return true when the value changed or fired.
bool Switch(const char* id, bool* v, bool enabled = true);
bool EnergyBar(const char* id, float* v, float min, float max, const char* fmt, float width, bool enabled = true, bool dim = false);
bool KeyChip(const char* id, const char* text, bool capturing, bool conflict, float width);
bool Button(const char* id, const char* label, float width, Tone tone = Tone::Teal, bool enabled = true);
bool HoldButton(const char* id, const char* label, float width, float seconds = 0.9f);
// Themed combo box. Returns true when the pick changed.
bool Dropdown(const char* id, const std::vector<std::string>& items, int* index, float width, bool enabled = true);
// Themed single line text field. Returns true when Enter was pressed.
bool TextField(const char* id, char* buf, size_t size, float width, const char* hint = nullptr);
void Tag(const char* text, ImU32 col);

float Anim(ImGuiID id, float target, float speed = 14.f);

}
