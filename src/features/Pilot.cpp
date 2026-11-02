#include "features/Flight.h"
#include "core/AppLog.h"
#include "core/Features.h"
#include "core/Locale.h"
#include "features/Bind.h"
#include "game/GameThread.h"
#include "game/World.h"
#include "ui/Icons.h"
#include "ui/Menu.h"

// Boarding: the pilot seat of your own ship from any distance, through the call the game makes
// when you press interact at it.
namespace os::features {

namespace {

using TakeControlFn = void (*)(uintptr_t ship, uintptr_t player, bool interact);
using ShipCompFn = uintptr_t (*)(uintptr_t ownership, int32_t index); // -1 = current ship

struct
{
    TakeControlFn take = nullptr;
    ShipCompFn shipComp = nullptr;
    int64_t player = 0x72CDE0, mode = 0x56AC, blocked = -1, own = 0xD5FC0;
} F;

void Notify(const char* key, unsigned glyph)
{
    Registry::Get().Post([key, glyph] { ui::Toast(L::T(key), "", glyph); });
}

// Game thread.
void BoardMine()
{
    uintptr_t d = game::Data();
    if (!d) return;
    uintptr_t pl = d + F.player;
    int32_t mode = 0;
    uint8_t busy = 0;
    if (Rd(pl + F.mode, mode) && mode == 1) return Notify("toast.pilot_flying", icon::Info);
    if (F.blocked > 0 && Rd(pl + F.blocked, busy) && busy) return Notify("toast.pilot_busy", icon::Info);
    uintptr_t ship = F.shipComp(d + F.own, -1);
    if (!ship) return Notify("toast.board_none", icon::Info);
    log::Info("Pilot", "Board current ship {:X}", ship);
    F.take(ship, pl, true);
}

}

void InstallPilot()
{
    const Target& takeT = Loc().Resolve("Ship.TakeControl");
    const Target& compT = Loc().Resolve("Ship.GetShipComponent");
    if (!takeT.ok) return Fail("ship.board", takeT.reason);
    if (!compT.ok) return Fail("ship.board", compT.reason);
    F.take = (TakeControlFn)Abs(takeT.func);
    F.shipComp = (ShipCompFn)Abs(compT.func);
    if (const Target& t = Loc().Resolve("Player.InstanceOffset"); t.ok && t.offset) F.player = *t.offset;
    if (const Target& t = Loc().Resolve("Player.Mode"); t.ok && t.offset) F.mode = *t.offset;
    if (const Target& t = Loc().Resolve("Player.EnterBlocked"); t.ok && t.offset) F.blocked = *t.offset;
    if (const Target& t = Loc().Resolve("Ship.Ownership"); t.ok && t.offset) F.own = *t.offset;
    log::Info("Pilot", "Ready: player +0x{:X}, busy +0x{:X}", F.player, F.blocked);

    Bound board;
    board.id = "ship.board";
    board.action = [] { if (game::FrameHookReady()) game::Post(BoardMine); };
    Ok(std::move(board));
}

}
