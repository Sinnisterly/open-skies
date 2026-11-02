#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace os::features {

void InstallWorldFx(); // worker thread, inside Install: tuner, world, settlement and corvette data
void InstallTeleport();
void InstallScout(); // worker thread, inside Install: system scout and report page
void InstallEffects(); // worker thread, inside Install: effect browser, test play, placed effects
void InstallStorage(); // worker thread, inside Install: every container, any-range transfer, sort
void InstallBlueprints(); // worker thread, inside Install: copy a base to a file, paste it into a base, undo
struct SeenBase { std::string name, owner, platform, type; uint64_t ua = 0; double pos[3] = {}; };
std::vector<SeenBase> OtherBasesHere(); // game thread: other players' bases the game has loaded near you
std::vector<SeenBase> MyBases();        // game thread: your own bases
bool PlayerSpot(uint64_t& ua, double pos[3]); // game thread: your address and position in the system
bool CurrentSystemNames(uint64_t& ua, std::string& system, std::vector<std::string>& planets); // game thread
void InstallBases(); // worker thread, inside Install: featured and seen bases, Portal Repository search, warp
bool CurrentAddress(uint64_t& ua); // the player's packed universe address
void WarpToAddress(uint64_t ua, const std::string& name); // UI thread: warp like a typed address
void SaveSystemSpot(uint64_t ua, const std::string& name); // UI thread: a Teleport spot for a system, no ground position
bool GlyphsToAddress(const std::string& glyphs, int galaxy, uint64_t& ua); // 12 portal glyph digits to an address
std::string AddressGlyphs(uint64_t ua);
std::string AddressWhere(uint64_t ua); // "Galaxy N, system S, planet P"
// Corvettes: the parts of every loaded corvette base (Blueprints owns the base list), base-local, in the game's order.
struct CorvettePart { std::string id; uint64_t user = 0, time = 0; float p[3] = {}, up[3] = {}, at[3] = {}; };
struct CorvetteSeen { std::string name, owner; int slot = -1, base = -1; bool mine = false; float dist = 0; std::vector<CorvettePart> parts; };
std::vector<CorvetteSeen> CorvettesLoaded(); // game thread, nearest first
uintptr_t BaseBuffer(int index);             // game thread: the cGcPlayerBasePersistentBuffer at that base list index, or 0
// A ship slot's inventories (Storage owns the stores). which: 0 general, 1 tech, 2 cargo.
struct ShipStack { std::string id; int32_t x = 0, y = 0, amount = 0, max = 0, type = 0; };
bool ShipStacks(int slot, int which, std::vector<ShipStack>& out); // game thread
int ShipClass(int slot);                                         // game thread: 0..3 (C..S), -1 unknown
std::string ShipName(int slot);                                  // game thread
uintptr_t ShipStore(int slot, int which);                        // game thread: the cGcInventoryStore, which as above
uintptr_t ShipResource(int slot);                                // game thread: the slot's cGcResourceElement (Garage)
int OwnedShipSlots(int& total);                                  // game thread: slots in use, and how many the save has
void InstallCorvettes(); // worker thread, inside Install: corvette library, copy nearest corvette
// In-world HUD markers: spots in a system, in the same frame as PlayerSpot.
struct MarkerSpot { std::string name; uint64_t ua = 0; double pos[3] = {}; int kind = 0; }; // kind: 0 teleport, 1 base, 2+ POI group
std::vector<MarkerSpot> TeleportMarkers(); // any thread
std::vector<MarkerSpot> BaseMarkers();     // any thread: bases marked in the Bases tab
void InstallMarkers(); // worker thread, inside Install
void DrawMarkers();    // render thread, every frame, menu open or not
void InstallLogoSkip(); // right after the frame hook: skip the boot logos and movies

}
