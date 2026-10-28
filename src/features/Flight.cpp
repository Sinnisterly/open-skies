#include "features/Flight.h"
#include "core/AppLog.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "ui/Menu.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstring>
#include <vector>

// Fly and no-clip on foot. The player's character controller is held at a target
// position each frame and moved by the keys, so gravity and the game's own motion do nothing.
namespace os::features {

namespace {

struct alignas(16) V4 { float x = 0, y = 0, z = 0, w = 0; };
struct alignas(16) BigPos { V4 local, offset; };

V4 operator+(V4 a, V4 b) { return { a.x + b.x, a.y + b.y, a.z + b.z, 0 }; }
V4 operator-(V4 a, V4 b) { return { a.x - b.x, a.y - b.y, a.z - b.z, 0 }; }
V4 operator*(V4 a, float k) { return { a.x * k, a.y * k, a.z * k, 0 }; }
float Dot(V4 a, V4 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V4 a) { return std::sqrt(Dot(a, a)); }
bool Finite(V4 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

bool RdV(uintptr_t a, V4& v)
{
    __try { memcpy(&v, reinterpret_cast<const void*>(a), sizeof v); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Unit vector of a along the plane with normal n, or zero.
V4 Flatten(V4 a, V4 n)
{
    V4 f = a - n * Dot(a, n);
    float l = Len(f);
    return l > 1e-3f ? f * (1.f / l) : V4{};
}

using GetPosFn = BigPos* (*)(uintptr_t ctrl, BigPos* out);
using SetPosFn = void (*)(uintptr_t ctrl, const BigPos* pos, bool);
using SetVelFn = void (*)(uintptr_t ctrl, const V4* vel);
using GetBodyFn = uintptr_t (*)(uintptr_t ctrl);
using BodyVelFn = void (*)(uintptr_t body, const V4* vel, bool);
using SetLayerFn = void (*)(uintptr_t body, int layer);
using AttachedFn = bool (*)(uintptr_t);
using PlayerBoolFn = bool (*)(uintptr_t);
using CamMatrixFn = const V4* (*)(uintptr_t cam);

struct
{
    GetPosFn getPos = nullptr;
    SetPosFn setPos = nullptr;
    SetVelFn setVel = nullptr;
    GetBodyFn getBody = nullptr;
    BodyVelFn bodyVel = nullptr;
    SetLayerFn setLayer = nullptr;
    int64_t player = 0x72C6F0, ctrl = 0x160, up = 0x5F30, cam = -1;
    uintptr_t camVt = 0;
    // The game's own "on foot" checks, copied from CheckFallenThroughFloor.
    int64_t busy = -1, attach = -1, mode = -1;
    AttachedFn attached = nullptr;
} F;

safetyhook::MidHook* g_fallHk = nullptr;
safetyhook::MidHook* g_layerHk = nullptr;
std::atomic<bool> g_fly{ false }, g_noclip{ false };
std::atomic<float> g_speed{ 3.f };

struct
{
    bool active = false, layered = false;
    BigPos target;
    LARGE_INTEGER last{};
} g;

uintptr_t Player()
{
    uintptr_t d = game::Data();
    return d ? d + F.player : 0;
}

uintptr_t Controller()
{
    uintptr_t p = Player(), c = 0;
    return p && Rd(p + F.ctrl, c) ? c : 0;
}

uintptr_t Body(uintptr_t ctrl)
{
    uintptr_t b = 0;
    return ctrl && F.getBody ? F.getBody(ctrl) : (Rd(ctrl + 0xC0, b) ? b : 0);
}

bool OnFoot()
{
    uintptr_t d = game::Data(), p = Player();
    if (!d || !p) return false;
    int32_t v = 0;
    if (F.busy >= 0 && Rd(d + F.busy, v) && v != 0) return false;
    if (F.mode >= 0 && Rd(d + F.mode, v) && v >= 0xA && v <= 0xC) return false;
    if (F.attached && F.attach >= 0 && F.attached(d + F.attach)) return false;
    uintptr_t vt = 0, fn = 0;
    if (F.attached && Rd(p, vt) && vt && Rd(vt, fn) && fn && reinterpret_cast<PlayerBoolFn>(fn)(p)) return false;
    return true;
}

// Unit vectors from the camera, flattened against the player's up.
bool Basis(V4& fwd, V4& right, V4& up)
{
    uintptr_t d = game::Data(), vt = 0;
    if (!d || F.cam < 0 || !Rd(d + F.cam, vt) || vt != F.camVt) return false;
    uintptr_t fn = 0;
    if (!Rd(vt + 0x48, fn) || !fn) return false;
    const V4* m = reinterpret_cast<CamMatrixFn>(fn)(d + F.cam);
    V4 r, u, at;
    if (!m || !RdV(uintptr_t(m), r) || !RdV(uintptr_t(m + 1), u) || !RdV(uintptr_t(m + 2), at)) return false;
    if (!RdV(Player() + F.up, up) || !Finite(up) || std::fabs(Len(up) - 1.f) > 0.2f) up = u;
    up = up * (1.f / std::max(Len(up), 1e-3f));
    fwd = Flatten(at * -1.f, up); // the camera looks down -at
    right = Flatten(r, up);
    return Finite(fwd) && Finite(right) && Len(fwd) > 0.5f && Len(right) > 0.5f;
}

bool GameFocused()
{
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void Zero(uintptr_t ctrl)
{
    V4 z{};
    F.setVel(ctrl, &z);
    if (uintptr_t b = Body(ctrl)) F.bodyVel(b, &z, false);
}

void SetLayer(bool noclip)
{
    uintptr_t ctrl = Controller(), b = Body(ctrl);
    if (!b || !F.setLayer) return;
    Wr<float>(ctrl + 0x114, noclip ? 0.f : 1.f);
    F.setLayer(b, noclip ? 0x1D : 8);
}

// Game thread, every frame.
void Tick()
{
    LARGE_INTEGER now, hz;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&hz);
    float dt = g.last.QuadPart ? float(now.QuadPart - g.last.QuadPart) / float(hz.QuadPart) : 0.f;
    g.last = now;
    dt = std::clamp(dt, 0.f, 0.1f);

    bool noclip = g_noclip;
    if (noclip != g.layered || (noclip && game::Frame() % 30 == 0)) { SetLayer(noclip); g.layered = noclip; }

    uintptr_t ctrl = Controller();
    if (!(g_fly || noclip) || !ctrl || !F.setPos || !OnFoot()) {
        if (g.active && ctrl) Zero(ctrl);
        g.active = false;
        return;
    }

    BigPos cur;
    if (!F.getPos(ctrl, &cur) || !Finite(cur.local) || !Finite(cur.offset)) return;
    // Follow the controller when something else moved it (teleport, a wall, the game).
    V4 gap = (cur.local + cur.offset) - (g.target.local + g.target.offset);
    if (!g.active || Len(gap) > 1.5f) g.target = cur;
    else g.target = { g.target.local + (g.target.offset - cur.offset), cur.offset };
    g.active = true;

    V4 fwd, right, up;
    if (GameFocused() && !ui::IsOpen() && Basis(fwd, right, up)) {
        float f = float(Key('W') - Key('S')), s = float(Key('D') - Key('A'));
        float v = float(Key(VK_SPACE) - (Key(VK_LCONTROL) || Key('C')));
        V4 dir = fwd * f + right * s + up * v;
        float l = Len(dir);
        if (l > 1e-3f) {
            float speed = 5.f * g_speed * (Key(VK_LSHIFT) ? 4.f : 1.f);
            g.target.local = g.target.local + dir * (speed * dt / std::max(l, 1.f));
        }
    }
    F.setPos(ctrl, &g.target, false);
    Zero(ctrl);
}

// While no-clip is on, keep the layer from being reset and skip the fall-through rescue.
void LayerCb(Context& c)
{
    uintptr_t ctrl = Controller(), b = 0;
    if (ctrl && Rd(ctrl + 0xC0, b) && b && c.rcx == b) c.rdx = 0x1D;
}

void SkipRet0(Context& c) { c.rax = 0; c.rip = RetGadget(); }

// Calls in order inside fn: (register the call's rcx was loaded from, call target).
struct Call { int64_t from; int fromReg; bool fromRax; uint32_t target; };
std::vector<Call> CallsOf(uint32_t fn, size_t limit)
{
    std::vector<Call> out;
    int64_t from = -1;
    int reg = 0;
    bool rax = false;
    auto ranges = Img().FuncRanges(fn);
    std::sort(ranges.begin(), ranges.end());
    for (auto& r : ranges) {
        for (auto& i : Img().DecodeRange(r.first, r.second)) {
            bool rcxDst = !i.ops.empty() && i.ops[0].kind == hooks::Operand::Reg && ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, (ZydisRegister)i.ops[0].reg) == ZYDIS_REGISTER_RCX;
            if (i.isCall && i.branchTarget) {
                out.push_back({ from, reg, rax, *i.branchTarget });
                if (out.size() >= limit) return out;
                from = -1; rax = false;
            }
            else if (rcxDst) {
                from = -1; rax = false;
                if (i.mnemonic == "mov" && i.ops.size() == 2 && i.ops[1].kind == hooks::Operand::Mem && !i.ops[1].ripRel) { from = i.ops[1].disp; reg = i.ops[1].base; }
                else if (i.mnemonic == "mov" && i.ops.size() == 2 && i.ops[1].kind == hooks::Operand::Reg && i.ops[1].reg == ZYDIS_REGISTER_RAX) rax = true;
            }
        }
    }
    return out;
}

void Fail2(const std::string& why) { Fail("tp.fly", why); Fail("move.noclip", why); }

// Land anywhere: the ship's safe-landing check answers "can land" (0), but only inside the player's
// landing update. Ship summoning and AI ships landing next to the player use the same check.
safetyhook::InlineHook g_land;
thread_local bool t_playerLanding = false;
int32_t LandHk(uint32_t ship, void* mat, bool a, bool b, bool c, void* corvette, float f)
{
    int32_t r = g_land.call<int32_t>(ship, mat, a, b, c, corvette, f);
    return t_playerLanding ? 0 : r;
}

// The landing update's own slope test exits with TooSteep before the check above runs.
uintptr_t g_slopeNext = 0;
void SlopeCb(Context& c)
{
    if (g_land.enabled()) c.rip = g_slopeNext;
}

// After the press, landing only begins when the target is not above the ship along its axis.
safetyhook::InlineHook g_canBegin;
bool CanBeginHk(void* ship)
{
    return g_canBegin.call<bool>(ship) || g_land.enabled();
}

// Two gates before the update show "landing unavailable": the altitude test in space, and in
// atmosphere the in-range test right after it. Both skip to the update.
uintptr_t g_gateNext = 0;
void GateCb(Context& c)
{
    if (g_land.enabled()) c.rip = g_gateNext;
}

using ShipXformFn = void (*)(void* ship, void* out); // the ship's current transform, as a landing target
ShipXformFn g_shipXform = nullptr;

// The ship's landing update, which calls the check above once its ground ray hits.
safetyhook::InlineHook g_landUpd;
int32_t LandUpdHk(void* ship, void* out, uintptr_t flag)
{
    t_playerLanding = g_land.enabled();
    int32_t r = g_landUpd.call<int32_t>(ship, out, flag);
    t_playerLanding = false;
    // No ground under the ship (space, or too high): land right where it is.
    if (r != 0 && g_land.enabled() && g_shipXform) {
        g_shipXform(ship, out);
        return 0;
    }
    return r;
}

}

bool CameraRay(float pos[3], float dir[3])
{
    uintptr_t d = game::Data(), vt = 0, fn = 0;
    if (!d || F.cam < 0 || !Rd(d + F.cam, vt) || vt != F.camVt || !Rd(vt + 0x48, fn) || !fn) return false;
    const V4* m = reinterpret_cast<CamMatrixFn>(fn)(d + F.cam);
    V4 at, local, offset;
    if (!m || !RdV(uintptr_t(m + 2), at) || !RdV(uintptr_t(m + 3), local) || !RdV(uintptr_t(m + 4), offset)) return false;
    V4 p = local + offset, f = at * -1.f;
    float l = Len(f);
    if (!Finite(p) || !Finite(f) || l < 0.5f) return false;
    f = f * (1.f / l);
    pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
    dir[0] = f.x; dir[1] = f.y; dir[2] = f.z;
    return true;
}

void InstallFlight()
{
    if (auto* t = Need("Ship.CheckSafeLand", { "ship.land_anywhere" })) {
        auto h = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(t->func)), reinterpret_cast<void*>(&LandHk),
                                                safetyhook::InlineHook::StartDisabled);
        if (!h) Fail("ship.land_anywhere", "hook could not be placed");
        else {
            g_land = std::move(*h);
            if (const Target& cb = Loc().Resolve("Ship.CanBeginLanding"); cb.ok) {
                if (auto u = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(cb.func)), reinterpret_cast<void*>(&CanBeginHk))) g_canBegin = std::move(*u);
            }
            if (!g_canBegin) log::Warn("Flight", "Landing begin check not hooked: atmosphere landing stays blocked");
            for (uint32_t c : Img().Callers(t->func)) { // the update starts mov rax, rsp; mov [rax+18h], r8b; mov [rax+10h], rdx
                const uint8_t* b = Img().At(c);
                static const uint8_t kHead[] = { 0x48, 0x8B, 0xC4, 0x44, 0x88, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10 };
                if (memcmp(b, kHead, sizeof kHead)) continue;
                if (auto u = safetyhook::InlineHook::create(reinterpret_cast<void*>(Abs(c)), reinterpret_cast<void*>(&LandUpdHk))) g_landUpd = std::move(*u);
                for (auto in = Dec(c); in; in = Next(*in)) // its first call reads the ship transform into the target
                    if (in->isCall && in->branchTarget) { g_shipXform = (ShipXformFn)Abs(*in->branchTarget); break; }
                // The space gate in the caller: jbe T; test r8, r8; je T, just before the update call.
                for (uint32_t up : Img().Callers(c)) {
                    std::vector<Insn> uc;
                    for (auto& r : Img().FuncRanges(up)) for (auto& x : Img().DecodeRange(r.first, r.second)) uc.push_back(x);
                    for (size_t k = 0; k + 2 < uc.size(); ++k) {
                        auto &a = uc[k], &t = uc[k + 1], &j = uc[k + 2];
                        if (a.mnemonic != "jbe" || t.mnemonic != "test" || j.mnemonic != "je" || !a.branchTarget || a.branchTarget != j.branchTarget)
                            continue;
                        bool callsUpdate = false;
                        for (size_t q = k + 3; q < uc.size() && q < k + 14; ++q) callsUpdate = callsUpdate || (uc[q].isCall && uc[q].branchTarget == c);
                        if (!callsUpdate) continue;
                        g_gateNext = Abs(j.rva + j.len);
                        auto* h1 = Mid(a.rva, GateCb);
                        auto* h2 = Mid(t.rva, GateCb);
                        if (h1 && h1->enable() && h2 && h2->enable()) log::Info("Flight", "Landing gates at +0x{:X} and +0x{:X}", a.rva, t.rva);
                        break;
                    }
                }
                // mov esi, 1 is the TooHigh exit; the slope test is the ja to just after it.
                std::vector<Insn> code;
                for (auto& r : Img().FuncRanges(c)) for (auto& x : Img().DecodeRange(r.first, r.second)) code.push_back(x);
                uint32_t exit = 0;
                for (auto& x : code)
                    if (x.mnemonic == "mov" && x.ops.size() == 2 && x.ops[0].kind == hooks::Operand::Reg && x.ops[0].reg == ZYDIS_REGISTER_ESI &&
                        x.ops[1].kind == hooks::Operand::Imm && x.ops[1].imm == 1) exit = x.rva + x.len;
                for (auto& x : code)
                    if (exit && x.mnemonic == "ja" && x.branchTarget == exit) {
                        g_slopeNext = Abs(x.rva + x.len);
                        if (auto* h = Mid(x.rva, SlopeCb); h && h->enable()) log::Info("Flight", "Landing slope test at +0x{:X}", x.rva);
                        break;
                    }
                break;
            }
            Bound b;
            b.id = "ship.land_anywhere";
            b.toggle = [](bool on) {
                return (on ? g_land.enable() : g_land.disable()).has_value();
            };
            Ok(std::move(b));
        }
    }

    if (const Target& t = Loc().Resolve("Player.InstanceOffset"); t.ok && t.offset) F.player = *t.offset;
    if (const Target& t = Loc().Resolve("Player.Up"); t.ok && t.offset) F.up = *t.offset;
    if (const Target& t = Loc().Resolve("Camera.Manager"); t.ok && t.offset) {
        auto vts = Img().Vtables("cGcCameraManager");
        if (vts.size() == 1) { F.cam = *t.offset; F.camVt = Abs(vts[0]); }
    }
    if (F.cam < 0) log::Warn("Flight", "Camera not found, flight keys are off");

    const Target& setT = Loc().Resolve("Player.SetToPosition");
    const Target& fallT = Loc().Resolve("Player.CheckFallenThroughFloor");
    const Target& layerT = Loc().Resolve("Physics.SetCollisionLayer");
    if (!setT.ok || !fallT.ok) return Fail2(!setT.ok ? setT.reason : fallT.reason);

    // SetToPosition: ctrl->SetPosition, ctrl->SetTargetVelocity, ctrl->GetBody, body->SetLinearVelocity.
    auto calls = CallsOf(setT.func, 24);
    std::vector<uint32_t> onCtrl;
    for (size_t i = 0; i < calls.size(); ++i) {
        if (calls[i].from >= 0x100 && calls[i].from < 0x400 && onCtrl.size() < 3) {
            if (onCtrl.empty()) F.ctrl = calls[i].from;
            if (calls[i].from == F.ctrl) {
                onCtrl.push_back(calls[i].target);
                if (onCtrl.size() == 3 && i + 1 < calls.size() && calls[i + 1].fromRax) F.bodyVel = (BodyVelFn)Abs(calls[i + 1].target);
            }
        }
    }
    auto starts = [](uint32_t rva, const char* bytes) { return memcmp(Img().At(rva), bytes, strlen(bytes)) == 0; };
    // SetTargetVelocity is movaps xmm0,[rdx]; GetBody is mov rax,[rcx+disp32].
    if (onCtrl.size() == 3 && F.bodyVel && starts(onCtrl[1], "\x0F\x28\x02") && starts(onCtrl[2], "\x48\x8B\x81")) {
        F.setPos = (SetPosFn)Abs(onCtrl[0]);
        F.setVel = (SetVelFn)Abs(onCtrl[1]);
        F.getBody = (GetBodyFn)Abs(onCtrl[2]);
    }
    // CheckFallenThroughFloor: the first controller call that is not GetBody reads the position.
    for (auto& c : CallsOf(fallT.func, 12)) {
        if (c.from == F.ctrl && F.getBody && Abs(c.target) != (uintptr_t)F.getBody) { F.getPos = (GetPosFn)Abs(c.target); break; }
    }
    if (!F.setPos || !F.getPos) return Fail2("controller calls not found");

    // The early-out checks at the top of CheckFallenThroughFloor.
    auto head = Img().DecodeRange(fallT.func, fallT.func + 0x70);
    for (size_t i = 0; i < head.size(); ++i) {
        const auto& in = head[i];
        if (in.ops.size() != 2) continue;
        bool memRax = in.ops[0].kind == hooks::Operand::Mem && in.ops[0].base == ZYDIS_REGISTER_RAX;
        if (F.busy < 0 && in.mnemonic == "cmp" && memRax && in.ops[1].kind == hooks::Operand::Imm && in.ops[1].imm == 0) F.busy = in.ops[0].disp;
        else if (F.attach < 0 && in.mnemonic == "lea" && in.ops[1].kind == hooks::Operand::Mem && in.ops[1].base == ZYDIS_REGISTER_RAX) {
            F.attach = in.ops[1].disp;
            for (size_t j = i + 1; j < head.size() && j < i + 10; ++j)
                if (head[j].isCall && head[j].branchTarget) { F.attached = (AttachedFn)Abs(*head[j].branchTarget); break; }
        }
        else if (F.mode < 0 && in.mnemonic == "mov" && in.ops[0].kind == hooks::Operand::Reg && in.ops[1].kind == hooks::Operand::Mem && in.ops[1].base == ZYDIS_REGISTER_RAX && !in.ops[1].ripRel && in.ops[1].disp > 0x1000) F.mode = in.ops[1].disp;
    }
    if (F.busy < 0 || F.attach < 0 || F.mode < 0 || !F.attached) {
        log::Warn("Flight", "On-foot check not found, flight also runs in vehicles");
        F.busy = F.attach = F.mode = -1;
        F.attached = nullptr;
    }

    game::EveryFrame(Tick);

    Bound fly;
    fly.id = "tp.fly";
    fly.toggle = [](bool on) { g_fly = on; return true; };
    fly.slider = [](int i, float v) { if (i == 0) g_speed = std::clamp(v, 0.2f, 20.f); };
    Ok(std::move(fly));

    if (!layerT.ok) return Fail("move.noclip", layerT.reason);
    F.setLayer = (SetLayerFn)Abs(layerT.func);
    g_fallHk = Mid(fallT.func, SkipRet0);
    g_layerHk = Mid(layerT.func, LayerCb);
    if (!g_fallHk || !g_layerHk) return Fail("move.noclip", "hook could not be placed");
    Bound nc;
    nc.id = "move.noclip";
    nc.toggle = [](bool on) {
        bool ok1 = (on ? g_fallHk->enable() : g_fallHk->disable()).has_value();
        bool ok2 = (on ? g_layerHk->enable() : g_layerHk->disable()).has_value();
        g_noclip = on;
        return ok1 && ok2;
    };
    Ok(std::move(nc));

    log::Info("Flight", "Ready: player +0x{:X}, controller +0x{:X}, camera +0x{:X}, up +0x{:X}", F.player, F.ctrl, F.cam, F.up);
}

}
