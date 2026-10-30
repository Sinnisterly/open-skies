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
void InstallLogoSkip(); // right after the frame hook: skip the boot logos and movies

}
