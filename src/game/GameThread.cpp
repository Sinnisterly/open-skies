#include "game/GameThread.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>

namespace os::game {

namespace {

std::mutex g_mx;
std::vector<std::function<void()>> g_jobs;
std::atomic<uint64_t> g_frame{ 0 };
safetyhook::MidHook* g_hook = nullptr;
std::atomic<void (*)()> g_ticks[32]{}; // one per feature tick; the log warns when full

// Where the last guarded fault happened, for the log.
struct Fault { uint32_t code = 0; uintptr_t at = 0; } g_fault;

int Note(EXCEPTION_POINTERS* ep)
{
    g_fault = { ep->ExceptionRecord->ExceptionCode, (uintptr_t)ep->ExceptionRecord->ExceptionAddress };
    return EXCEPTION_EXECUTE_HANDLER;
}

std::string Where(uintptr_t a)
{
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, name, MAX_PATH);
        const char* base = strrchr(name, '\\');
        return std::format("{}+0x{:X}", base ? base + 1 : name, a - (uintptr_t)m);
    }
    return std::format("0x{:X}", a);
}

bool RunGuarded(std::function<void()>* job)
{
    __try { (*job)(); return true; }
    __except (Note(GetExceptionInformation())) { return false; }
}

bool TickGuarded(void (*fn)())
{
    __try { fn(); return true; }
    __except (Note(GetExceptionInformation())) { return false; }
}

void Run(std::function<void()>& job)
{
    try {
        if (!RunGuarded(&job)) log::Error("Game", "A game-thread job faulted and was skipped: 0x{:X} at {}", g_fault.code, Where(g_fault.at));
    }
    catch (const std::exception& e) { log::Error("Game", "Game-thread job threw: {}", e.what()); }
    catch (...) { log::Error("Game", "Game-thread job threw"); }
}

// Logs stretches of slow frames, with how much of each frame was our own work.
struct { int64_t last = 0, windowStart = 0, ours = 0, worst = 0; int slow = 0, frames = 0; } g_perf;

int64_t Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void Measure(int64_t start, int64_t end)
{
    static const double toMs = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return 1000.0 / double(f.QuadPart); }();
    auto& p = g_perf;
    if (p.last) {
        int64_t gap = start - p.last;
        ++p.frames;
        p.slow += gap * toMs > 50.0;
        p.worst = std::max(p.worst, gap);
    }
    else p.windowStart = start;
    p.last = start;
    p.ours += end - start;
    if ((start - p.windowStart) * toMs < 5000.0) return;
    if (p.slow > 5) log::Info("Perf", "{} of {} frames over 50 ms in 5 s, worst {:.0f} ms, Open Skies used {:.1f} ms", p.slow, p.frames,
                              p.worst * toMs, p.ours * toMs);
    p = { p.last, start, 0, 0, 0, 0 };
}

void FrameCb(safetyhook::Context&)
{
    int64_t t0 = Now();
    struct Done { int64_t t0; ~Done() { Measure(t0, Now()); } } done{ t0 };
    ++g_frame;
    for (auto& t : g_ticks) {
        auto* fn = t.load();
        if (fn && !TickGuarded(fn)) {
            log::Error("Game", "A per-frame task faulted and was stopped: 0x{:X} at {}", g_fault.code, Where(g_fault.at));
            t = nullptr;
        }
    }
    std::vector<std::function<void()>> jobs;
    {
        std::lock_guard lk(g_mx);
        if (g_jobs.empty()) return;
        jobs.swap(g_jobs);
    }
    for (auto& j : jobs) Run(j);
}

}

bool InstallFrameHook()
{
    const auto& t = features::Loc().Resolve("App.Update");
    if (!t.ok) { log::Warn("Game", "Frame hook unavailable: {}", t.reason); return false; }
    g_hook = features::Mid(t.func, FrameCb);
    if (!g_hook || !g_hook->enable()) { log::Warn("Game", "Frame hook could not be placed"); g_hook = nullptr; return false; }
    log::Info("Game", "Frame hook on cGcApplication::Update");
    return true;
}

bool FrameHookReady() { return g_hook != nullptr; }

void Post(std::function<void()> job)
{
    std::lock_guard lk(g_mx);
    g_jobs.push_back(std::move(job));
}

uint64_t Frame() { return g_frame; }

void EveryFrame(void (*fn)())
{
    for (auto& t : g_ticks) {
        void (*empty)() = nullptr;
        if (t.compare_exchange_strong(empty, fn)) return;
    }
    log::Warn("Game", "No room for another per-frame task");
}

}
