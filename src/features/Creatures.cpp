#include "features/Flight.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/SaveBackup.h"
#include "game/World.h"
#include "ui/Icons.h"
#include "ui/Menu.h"
#include <atomic>
#include <cmath>

// Taming: any creature, through the game's own adoption, the same call its debug "Make Pet" uses.
namespace os::features {

namespace {

using AdoptFn = int32_t (*)(uintptr_t ownership, uintptr_t creature);
using XformFn = const float* (*)(uintptr_t obj, float* out); // 3 rotation rows, then local and offset position

struct
{
    uintptr_t list = 0; // slot holding the creature list pointer
    AdoptFn adopt = nullptr;
    XformFn xform = nullptr;
    int64_t own = 0xE10D0, body = 0x430, bodyXform = 0x60;
} F;

std::atomic<bool> g_watch{ false };

bool WorldPos(uintptr_t c, float out[3])
{
    uintptr_t body = 0;
    if (!Rd(c + F.body, body) || !body) return false;
    alignas(16) float m[20] = {};
    const float* r = F.xform(body + F.bodyXform, m);
    if (!r) return false;
    for (int i = 0; i < 3; ++i) {
        float a = 0, b = 0;
        if (!Rd(uintptr_t(r + 12 + i), a) || !Rd(uintptr_t(r + 16 + i), b)) return false;
        out[i] = a + b;
        if (!std::isfinite(out[i])) return false;
    }
    return out[0] != 0 || out[1] != 0 || out[2] != 0;
}

// The creature closest to the middle of the view, inside the cone and range.
uintptr_t Aimed(float maxDeg, float range)
{
    float cp[3], cd[3];
    uintptr_t lst = 0, arr = 0;
    int32_t n = 0;
    if (!CameraRay(cp, cd) || !F.list || !Rd(F.list, lst) || !lst || !Rd(lst + 4, n) || !Rd(lst + 8, arr) || !arr || n <= 0 || n > 4096) return 0;
    float minCos = std::cos(maxDeg * 3.14159265f / 180.f), best = -2.f;
    uintptr_t pick = 0;
    for (int32_t i = 0; i < n; ++i) {
        uintptr_t c = 0;
        float p[3];
        if (!Rd(arr + 8 * (uintptr_t)i, c) || !c || !WorldPos(c, p)) continue;
        float v[3] = { p[0] - cp[0], p[1] - cp[1], p[2] - cp[2] };
        float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (d < 0.5f || d > range) continue;
        float cosA = (v[0] * cd[0] + v[1] * cd[1] + v[2] * cd[2]) / d;
        float score = cosA - d * 0.0005f; // a near creature wins a tie
        if (cosA >= minCos && score > best) { best = score; pick = c; }
    }
    return pick;
}

void Notify(const char* key, unsigned glyph)
{
    Registry::Get().Post([key, glyph] { ui::Toast(L::T(key), "", glyph); });
}

// Game thread. True when a creature was found, whatever the result.
bool TameAimed(float maxDeg, float range)
{
    uintptr_t d = game::Data(), c = Aimed(maxDeg, range);
    if (!d || !c) return false;
    int32_t slot = F.adopt(d + F.own, c);
    log::Info("Creatures", "Tame: slot {}", slot);
    if (slot >= 0) Notify("toast.tamed", icon::Check);
    else Notify("toast.tame_full", icon::Warning);
    return true;
}

// Game thread, every frame: Tame on Sight waits for a creature in the middle of the view.
void Tick()
{
    if (!g_watch || game::Frame() % 10) return;
    if (!TameAimed(4.f, 60.f)) return;
    g_watch = false;
    Registry::Get().Post([] {
        if (Feature* f = Registry::Get().Find("world.tame_sight"); f && f->on) Registry::Get().SetOn(*f, false);
    });
}

}

void InstallCreatures()
{
    const char* ids[] = { "world.tame", "world.tame_sight" };
    auto fail = [&](const std::string& why) { for (auto* id : ids) Fail(id, why); };
    const Target& listT = Loc().Resolve("Creature.List");
    const Target& adoptT = Loc().Resolve("Creature.Adopt");
    const Target& bodyT = Loc().Resolve("Creature.Body");
    if (!listT.ok || !listT.global) return fail(listT.reason.empty() ? "creature list not found" : listT.reason);
    if (!adoptT.ok) return fail(adoptT.reason);
    if (!bodyT.ok || !bodyT.offset) return fail(bodyT.reason.empty() ? "creature body not found" : bodyT.reason);
    if (const Target& t = Loc().Resolve("Creature.Ownership"); t.ok && t.offset) F.own = *t.offset;

    // The body transform: add rcx, disp and the call that follows the body load.
    auto in = Dec(bodyT.site);
    for (int i = 0; in && i < 24; ++i, in = Next(*in)) {
        if (in->mnemonic == "add" && in->ops.size() == 2 && in->ops[1].kind == hooks::Operand::Imm && in->ops[1].imm > 0 && in->ops[1].imm < 0x400) F.bodyXform = in->ops[1].imm;
        if (in->isCall && in->branchTarget) { F.xform = (XformFn)Abs(*in->branchTarget); break; }
    }
    if (!F.xform) return fail("creature transform not found");
    F.list = listT.global;
    F.body = *bodyT.offset;
    F.adopt = (AdoptFn)Abs(adoptT.func);
    game::EveryFrame(Tick);

    Bound tame;
    tame.id = "world.tame";
    tame.action = [] {
        if (!game::FrameHookReady()) return;
        game::BackupSaveOnce();
        game::Post([] { if (!TameAimed(10.f, 120.f)) Notify("toast.tame_none", icon::Info); });
    };
    Ok(std::move(tame));

    Bound sight;
    sight.id = "world.tame_sight";
    sight.toggle = [](bool on) {
        if (on) game::BackupSaveOnce();
        g_watch = on;
        return true;
    };
    Ok(std::move(sight));
    log::Info("Creatures", "Ready: ownership +0x{:X}, body +0x{:X}", F.own, F.body);
}

}
