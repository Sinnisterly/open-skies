#include "input/Input.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Hotkeys.h"
#include "core/Settings.h"
#include "ui/Menu.h"
#include <imgui.h>
#include <safetyhook.hpp>
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <vector>

// NEXTRAWINPUTBLOCK needs this and windows.h does not define it.
typedef unsigned __int64 QWORD;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace os::input {

namespace {

HWND g_hwnd;
WNDPROC g_orig;
uintptr_t g_selfLo, g_selfHi;
std::atomic<bool> g_menuOpen{ false };

struct Msg { UINT m; WPARAM w; LPARAM l; };
std::mutex g_mx;
std::vector<Msg> g_queue;

safetyhook::InlineHook h_rawData, h_rawBuffer, h_setCursorPos, h_clipCursor, h_asyncKey, h_xinput;

std::atomic<bool (*)(uint16_t)> g_swallow{ nullptr };
std::atomic<int> g_wheel{ 0 };
std::atomic<bool> g_rawWheel{ false }; // the game reads raw mouse input, so the wheel is counted there

bool Swallows(uint16_t vk)
{
    auto f = g_swallow.load();
    return f && !g_menuOpen.load() && f(vk);
}

// Raw mouse buttons and wheel the preview wants are taken out; movement stays so you can look around.
void SwallowMouse(RAWMOUSE& m)
{
    if ((m.usButtonFlags & RI_MOUSE_WHEEL) && Swallows(kWheel)) {
        g_wheel += (short)m.usButtonData;
        g_rawWheel = true;
        m.usButtonFlags &= ~RI_MOUSE_WHEEL;
        m.usButtonData = 0;
    }
    if (Swallows(VK_LBUTTON)) m.usButtonFlags &= ~(RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP);
    if (Swallows(VK_RBUTTON)) m.usButtonFlags &= ~(RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_UP);
}

bool FromSelf(void* ret) { auto r = (uintptr_t)ret; return r >= g_selfLo && r < g_selfHi; }

bool IsInputMsg(UINT m)
{
    return (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || (m >= WM_KEYFIRST && m <= WM_KEYLAST) ||
           m == WM_INPUT || m == WM_MOUSEHOVER || m == WM_MOUSELEAVE || m == WM_NCMOUSEMOVE;
}

uint16_t VkFromMouse(UINT m, WPARAM w)
{
    switch (m) {
    case WM_MBUTTONDOWN: return VK_MBUTTON;
    case WM_XBUTTONDOWN: return GET_XBUTTON_WPARAM(w) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default: return 0;
    }
}

void Queue(UINT m, WPARAM w, LPARAM l)
{
    std::lock_guard lk(g_mx);
    if (g_queue.size() < 512) g_queue.push_back({ m, w, l });
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    bool open = g_menuOpen.load();

    // Hotkeys: key down without auto repeat, plus side and middle mouse buttons.
    uint16_t vk = 0;
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && !(l & (1 << 30))) vk = (uint16_t)w;
    else if (m == WM_MBUTTONDOWN || m == WM_XBUTTONDOWN) vk = VkFromMouse(m, w);
    if (vk) {
        bool typing = open && ImGui::GetCurrentContext() && ImGui::GetIO().WantTextInput;
        auto& hk = Hotkeys::Get();
        if (!hk.Capturing().empty() || !typing) {
            bool consumed = false;
            std::string id = hk.OnKeyDown(vk, consumed);
            if (!id.empty()) Registry::Get().Post([id] { ui::TriggerBinding(id); });
            else if (open && vk == VK_ESCAPE && !consumed) Registry::Get().Post([] { ui::SetOpen(false); });
            if (consumed || (open && vk == VK_ESCAPE)) return 0;
        }
    }

    if (!open && g_swallow.load()) {
        if ((m == WM_KEYDOWN || m == WM_KEYUP || m == WM_SYSKEYDOWN || m == WM_SYSKEYUP) && Swallows((uint16_t)w)) return 0;
        if ((m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_LBUTTONDBLCLK) && Swallows(VK_LBUTTON)) return 0;
        if ((m == WM_RBUTTONDOWN || m == WM_RBUTTONUP || m == WM_RBUTTONDBLCLK) && Swallows(VK_RBUTTON)) return 0;
        if (m == WM_MOUSEWHEEL && Swallows(kWheel)) {
            if (!g_rawWheel) g_wheel += GET_WHEEL_DELTA_WPARAM(w);
            return 0;
        }
    }

    if (open) {
        if (IsInputMsg(m) || m == WM_CHAR || m == WM_SETFOCUS || m == WM_KILLFOCUS) Queue(m, w, l);
        if (Settings::Get().Data().blockGameInputs) {
            if (m == WM_INPUT) return DefWindowProcW(h, m, w, l);
            if (IsInputMsg(m) || m == WM_CHAR) return 0;
            if (m == WM_SETCURSOR) return TRUE;
        }
    }
    return CallWindowProcW(g_orig, h, m, w, l);
}

// Game polling paths. Calls from our own module always pass.
UINT WINAPI Hk_GetRawInputData(HRAWINPUT ri, UINT cmd, LPVOID data, PUINT size, UINT hdr)
{
    UINT r = h_rawData.stdcall<UINT>(ri, cmd, data, size, hdr);
    if (Blocking() && data && cmd == RID_INPUT && r != (UINT)-1 && !FromSelf(_ReturnAddress())) {
        auto* in = (RAWINPUT*)data;
        if (in->header.dwType == RIM_TYPEMOUSE) { in->data.mouse.lLastX = in->data.mouse.lLastY = 0; in->data.mouse.usButtonFlags = 0; in->data.mouse.usButtonData = 0; }
        else if (in->header.dwType == RIM_TYPEKEYBOARD) { in->data.keyboard.VKey = 0xFF; in->data.keyboard.Flags = RI_KEY_BREAK; }
    }
    else if (g_swallow.load() && data && cmd == RID_INPUT && r != (UINT)-1 && !FromSelf(_ReturnAddress())) {
        auto* in = (RAWINPUT*)data;
        if (in->header.dwType == RIM_TYPEMOUSE) SwallowMouse(in->data.mouse);
        else if (in->header.dwType == RIM_TYPEKEYBOARD && Swallows(in->data.keyboard.VKey)) { in->data.keyboard.VKey = 0xFF; in->data.keyboard.Flags = RI_KEY_BREAK; }
    }
    return r;
}

UINT WINAPI Hk_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr)
{
    UINT n = h_rawBuffer.stdcall<UINT>(data, size, hdr);
    if (Blocking() && data && n != (UINT)-1 && n > 0) {
        RAWINPUT* in = data;
        for (UINT i = 0; i < n; ++i) {
            if (in->header.dwType == RIM_TYPEMOUSE) { in->data.mouse.lLastX = in->data.mouse.lLastY = 0; in->data.mouse.usButtonFlags = 0; }
            else if (in->header.dwType == RIM_TYPEKEYBOARD) { in->data.keyboard.VKey = 0xFF; in->data.keyboard.Flags = RI_KEY_BREAK; }
            in = NEXTRAWINPUTBLOCK(in);
        }
    }
    else if (g_swallow.load() && data && n != (UINT)-1 && n > 0) {
        RAWINPUT* in = data;
        for (UINT i = 0; i < n; ++i) {
            if (in->header.dwType == RIM_TYPEMOUSE) SwallowMouse(in->data.mouse);
            else if (in->header.dwType == RIM_TYPEKEYBOARD && Swallows(in->data.keyboard.VKey)) { in->data.keyboard.VKey = 0xFF; in->data.keyboard.Flags = RI_KEY_BREAK; }
            in = NEXTRAWINPUTBLOCK(in);
        }
    }
    return n;
}

BOOL WINAPI Hk_SetCursorPos(int x, int y)
{
    if (Blocking() && !FromSelf(_ReturnAddress())) return TRUE;
    return h_setCursorPos.stdcall<BOOL>(x, y);
}

BOOL WINAPI Hk_ClipCursor(const RECT* r)
{
    if (Blocking() && !FromSelf(_ReturnAddress())) return h_clipCursor.stdcall<BOOL>(nullptr);
    return h_clipCursor.stdcall<BOOL>(r);
}

SHORT WINAPI Hk_GetAsyncKeyState(int vk)
{
    if ((Blocking() || Swallows((uint16_t)vk)) && !FromSelf(_ReturnAddress())) return 0;
    return h_asyncKey.stdcall<SHORT>(vk);
}

struct XState { DWORD packet; uint8_t pad[12]; };
DWORD WINAPI Hk_XInputGetState(DWORD idx, void* state)
{
    DWORD r = h_xinput.stdcall<DWORD>(idx, state);
    if (r == ERROR_SUCCESS && state && Blocking() && !FromSelf(_ReturnAddress()))
        memset((uint8_t*)state + sizeof(DWORD), 0, 12);
    return r;
}

template <class Fn> void HookExport(safetyhook::InlineHook& h, const wchar_t* mod, const char* name, Fn fn)
{
    HMODULE m = GetModuleHandleW(mod);
    if (!m) return;
    void* target = (void*)GetProcAddress(m, name);
    if (!target) return;
    auto r = safetyhook::InlineHook::create(target, (void*)fn);
    if (r) { h = std::move(*r); log::Debug("Input", "Hooked {}", name); }
    else log::Warn("Input", "Could not hook {}", name);
}

}

bool Blocking() { return g_menuOpen.load() && Settings::Get().Data().blockGameInputs; }

void SetSwallow(bool (*wants)(uint16_t vk))
{
    g_swallow = wants;
    g_wheel = 0;
}

int TakeWheel() { return g_wheel.exchange(0); }

void OnMenuOpen(bool open)
{
    g_menuOpen = open;
    if (open) ClipCursor(nullptr);
}

bool Install(HWND hwnd, HMODULE self)
{
    if (g_orig) return true;
    auto* dos = (IMAGE_DOS_HEADER*)self;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)self + dos->e_lfanew);
    g_selfLo = (uintptr_t)self;
    g_selfHi = g_selfLo + nt->OptionalHeader.SizeOfImage;

    g_hwnd = hwnd;
    g_orig = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (!g_orig) { log::Error("Input", "Window subclass failed ({})", GetLastError()); return false; }

    HookExport(h_rawData, L"user32.dll", "GetRawInputData", &Hk_GetRawInputData);
    HookExport(h_rawBuffer, L"user32.dll", "GetRawInputBuffer", &Hk_GetRawInputBuffer);
    HookExport(h_setCursorPos, L"user32.dll", "SetCursorPos", &Hk_SetCursorPos);
    HookExport(h_clipCursor, L"user32.dll", "ClipCursor", &Hk_ClipCursor);
    HookExport(h_asyncKey, L"user32.dll", "GetAsyncKeyState", &Hk_GetAsyncKeyState);
    for (const wchar_t* x : { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" })
        if (GetModuleHandleW(x)) { HookExport(h_xinput, x, "XInputGetState", &Hk_XInputGetState); break; }
    log::Info("Input", "Window input hooked");
    return true;
}

void Uninstall()
{
    if (g_orig && g_hwnd) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_orig);
    h_rawData = {}; h_rawBuffer = {}; h_setCursorPos = {}; h_clipCursor = {}; h_asyncKey = {}; h_xinput = {};
}

void Drain()
{
    std::vector<Msg> q;
    {
        std::lock_guard lk(g_mx);
        q.swap(g_queue);
    }
    for (auto& m : q) ImGui_ImplWin32_WndProcHandler(g_hwnd, m.m, m.w, m.l);
}

}
