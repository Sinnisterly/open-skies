#include "core/AppPaths.h"
#include <windows.h>
#include <shlobj.h>

namespace os::paths {

static std::wstring g_root, g_gameDir;

static bool Writable(const std::wstring& dir)
{
    std::wstring probe = dir + L"\\OpenSkies.probe";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

void Init(const std::wstring& preferredRoot)
{
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_gameDir = buf;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L"\\/"));

    std::wstring want = preferredRoot.empty() ? g_gameDir : preferredRoot;
    if (Writable(want)) { g_root = want; return; }

    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)))
    {
        g_root = std::wstring(appData) + L"\\OpenSkies";
        CoTaskMemFree(appData);
        CreateDirectoryW(g_root.c_str(), nullptr);
    }
    else g_root = want;
}

const std::wstring& Root() { return g_root; }
const std::wstring& GameDir() { return g_gameDir; }
std::wstring File(const wchar_t* name) { return g_root + L"\\" + name; }

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

}
