#pragma once
#include <string>
#include <vector>

namespace os::L {

// Loads embedded English, then an optional override pack next to the game.
void Init(const std::string& language);
const char* T(const std::string& key);
bool Has(const std::string& key);
std::string Tf(const std::string& key, const std::string& a0, const std::string& a1 = "");
std::vector<std::string> Languages();
bool ExportEnglish(const std::wstring& path);

}
