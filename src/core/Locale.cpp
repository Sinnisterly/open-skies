#include "core/Locale.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <unordered_map>
#include <windows.h>

#include "EmbeddedLocale.inc"

namespace os::L {

static std::unordered_map<std::string, std::string> g_map;

static void Merge(const std::string& text, const char* source)
{
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { log::Warn("Locale", "Bad locale data in {}", source); return; }
    for (auto& [k, v] : j.items())
        if (v.is_string()) g_map[k] = v.get<std::string>();
}

void Init(const std::string& language)
{
    g_map.clear();
    Merge(reinterpret_cast<const char*>(kEnJson), "embedded en");
    if (language.empty() || language == "en") return;
    std::wstring path = paths::Root() + L"\\Locales\\" + paths::Wide(language) + L".json";
    std::ifstream f(path, std::ios::binary);
    if (!f) { log::Warn("Locale", "No pack for '{}', using English", language); return; }
    std::string text((std::istreambuf_iterator<char>(f)), {});
    Merge(text, language.c_str());
}

const char* T(const std::string& key)
{
    auto it = g_map.find(key);
    if (it != g_map.end()) return it->second.c_str();
    auto [ins, _] = g_map.emplace(key, key);
    return ins->second.c_str();
}

bool Has(const std::string& key) { return g_map.contains(key); }

std::string Tf(const std::string& key, const std::string& a0, const std::string& a1)
{
    std::string s = T(key);
    auto rep = [&](const char* tok, const std::string& v) {
        for (size_t p; (p = s.find(tok)) != std::string::npos;) s.replace(p, 3, v);
    };
    rep("{0}", a0);
    rep("{1}", a1);
    return s;
}

std::vector<std::string> Languages()
{
    std::vector<std::string> out{ "en" };
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((paths::Root() + L"\\Locales\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = fd.cFileName;
        n = n.substr(0, n.size() - 5);
        if (n != L"en") out.push_back(paths::Utf8(n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

bool ExportEnglish(const std::wstring& path)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << reinterpret_cast<const char*>(kEnJson);
    return true;
}

}
