#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace os::assets::paks {

// Every file in GAMEDATA\PCBANKS by lowercase path, built from the pak manifests on a background thread.
void BuildAsync(const std::wstring& pcbanks); // later calls do nothing
bool Ready();
bool Exists(const std::string& path);
std::vector<uint8_t> Read(const std::string& path, size_t limit = 0);

}
