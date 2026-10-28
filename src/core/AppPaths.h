#pragma once
#include <string>

namespace os::paths {

// Picks the game folder if writable, else %AppData%\OpenSkies.
void Init(const std::wstring& preferredRoot = L"");
const std::wstring& Root();
const std::wstring& GameDir();
std::wstring File(const wchar_t* name);
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);

}
