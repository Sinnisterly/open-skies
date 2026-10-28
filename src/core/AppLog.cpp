#include "core/AppLog.h"
#include <windows.h>
#include <cstdio>
#include <deque>
#include <mutex>

namespace os::log {

static std::mutex g_mx;
static FILE* g_file;
static bool g_verbose;
static std::deque<std::string> g_recent;

void Init(const std::wstring& path)
{
    std::lock_guard lk(g_mx);
    if (g_file) return;
    g_file = _wfopen(path.c_str(), L"w");
}

void Shutdown()
{
    std::lock_guard lk(g_mx);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void SetVerbose(bool on) { g_verbose = on; }
bool Verbose() { return g_verbose; }

void Write(Level lvl, const char* cat, const std::string& msg)
{
    static const char* names[] = { "DBG", "INF", "WRN", "ERR" };
    SYSTEMTIME t; GetLocalTime(&t);
    std::string line = std::format("{:02}:{:02}:{:02}.{:03} [{}] {}: {}", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                                   names[(int)lvl], cat, msg);
    std::lock_guard lk(g_mx);
    g_recent.push_back(line);
    if (g_recent.size() > 400) g_recent.pop_front();
    if (g_file) { fputs(line.c_str(), g_file); fputc('\n', g_file); fflush(g_file); }
    OutputDebugStringA((line + "\n").c_str());
}

std::vector<std::string> Recent(size_t max)
{
    std::lock_guard lk(g_mx);
    size_t n = g_recent.size() < max ? g_recent.size() : max;
    return { g_recent.end() - (ptrdiff_t)n, g_recent.end() };
}

}
