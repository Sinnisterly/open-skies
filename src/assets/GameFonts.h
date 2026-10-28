#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace os::assets {

struct FontBlob
{
    std::string name;
    std::vector<uint8_t> data;
    bool Ok() const { return !data.empty(); }
};

struct FontSet
{
    FontBlob body, heading, icons, mono;
};

// Reads fonts from <gameRoot>\GAMEDATA\PCBANKS\NMSARC.fonts.pak, with Windows fonts as fallback.
FontSet LoadFonts(const std::wstring& gameRoot);

// Lists every font family inside the pak (diagnostics).
std::vector<std::string> ListPakFonts(const std::wstring& gameRoot);

}
