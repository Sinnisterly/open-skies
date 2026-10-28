#pragma once
#include "hooks/Locator.h"
#include <windows.h>
#include <safetyhook.hpp>
#include <Zydis/Zydis.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Shared helpers for the feature bindings.
namespace os::features {

using safetyhook::Context;
using hooks::Insn;
using hooks::Target;

inline hooks::Locator& Loc() { return hooks::Locator::Get(); }
inline const hooks::Image& Img() { return Loc().Img(); }
inline uintptr_t Abs(uint32_t rva) { return Img().Base() + rva; }

template <class T> bool Rd(uintptr_t a, T& v)
{
    __try { v = *reinterpret_cast<volatile T*>(a); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <class T> bool Wr(uintptr_t a, T v)
{
    __try { *reinterpret_cast<volatile T*>(a) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

safetyhook::Xmm* XmmOf(Context& c, int reg);
uintptr_t* GprOf(Context& c, int reg);
bool IsGpr(int reg);
bool IsXmm(int reg);

// One feature's binding result, applied on the render thread by Publish().
struct Bound
{
    std::string id;
    bool ok = false;
    std::string reason;
    std::function<bool(bool)> toggle;
    std::function<void(int, float)> slider;
    std::function<bool(int64_t&)> get;
    std::function<bool(int64_t)> set;
    std::function<void()> action;
};

void Fail(const char* id, const std::string& why);
void Ok(Bound b);
const Target* Need(const char* name, std::initializer_list<const char*> ids);

std::optional<Insn> Dec(uint32_t rva);
std::optional<Insn> Next(const Insn& i);
std::optional<uint32_t> ScanFunc(uint32_t func, const char* pat);

safetyhook::MidHook* Mid(uint32_t rva, safetyhook::MidHookFn fn);
std::function<bool(bool)> HookToggle(safetyhook::MidHook* h);
bool WriteCode(uintptr_t at, const uint8_t* src, size_t n);
bool MakeRet();
uintptr_t RetGadget();

void BindMid(const char* id, uint32_t rva, safetyhook::MidHookFn fn);
void BindNop(const char* id, uint32_t rva, int insns); // NOPs whole instructions

// Globals edits scaled by sliders (render thread only).
// type: 0 float, 1 int32, 2 uint64, 3 bool. Value is orig * slider * k (orig / slider when inverse),
// or map(slider) when map is set. slider -1 means no slider. Features sharing a field stack.
struct Field
{
    uintptr_t at = 0;
    int slider = 0;
    uint8_t type = 0;
    double (*map)(float) = nullptr;
    bool inverse = false;
    double k = 1;
};
bool ReadField(const Field& f, double& v);
bool WriteField(const Field& f, double v);
struct DataFx { std::vector<Field> fields; std::vector<float> mult; bool on = false; };
void BindData(const char* id, std::vector<Field> fields, size_t sliders);
void DataTick();

void Publish();

}
