#include "assets/PakIndex.h"
#include "assets/HgPak.h"
#include "core/AppLog.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

namespace os::assets::paks {

namespace {

struct Loc { uint32_t pak, entry; };

std::vector<std::unique_ptr<HgPak>> g_paks;
std::unordered_map<std::string, Loc> g_index;
std::atomic<bool> g_ready{ false }, g_started{ false };

std::string Norm(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

void Build(std::wstring dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\NMSARC.*.pak").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            auto pak = std::make_unique<HgPak>();
            if (!pak->Open(dir + L"\\" + fd.cFileName)) continue;
            auto names = pak->Manifest();
            if (names.size() + 1 != pak->Count()) continue;
            uint32_t id = (uint32_t)g_paks.size();
            for (uint32_t i = 0; i < names.size(); ++i) g_index.emplace(Norm(names[i]), Loc{ id, i + 1 });
            g_paks.push_back(std::move(pak));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    log::Info("Paks", "Indexed {} files in {} paks", g_index.size(), g_paks.size());
    g_ready = true; // published after the tables are complete; never modified again
}

}

void BuildAsync(const std::wstring& pcbanks)
{
    if (g_started.exchange(true)) return;
    std::thread(Build, pcbanks).detach();
}

bool Ready() { return g_ready; }

bool Exists(const std::string& path)
{
    return g_ready && g_index.count(Norm(path)) != 0;
}

std::vector<uint8_t> Read(const std::string& path, size_t limit)
{
    if (!g_ready) return {};
    auto it = g_index.find(Norm(path));
    if (it == g_index.end()) return {};
    return g_paks[it->second.pak]->Read(it->second.entry, limit);
}

}
