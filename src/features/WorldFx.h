#pragma once
#include <cstdint>
#include <string>

namespace os::features {

void InstallWorldFx(); // worker thread, inside Install: tuner, world, settlement and corvette data
void InstallTeleport();
void InstallScout(); // worker thread, inside Install: system scout and report page
void InstallEffects(); // worker thread, inside Install: effect browser, test play, placed effects
void InstallStorage(); // worker thread, inside Install: every container, any-range transfer, sort
bool CurrentAddress(uint64_t& ua); // the player's packed universe address
void WarpToAddress(uint64_t ua, const std::string& name); // UI thread: warp like a typed address
void SaveSystemSpot(uint64_t ua, const std::string& name); // UI thread: a Teleport spot for a system, no ground position
void InstallLogoSkip(); // right after the frame hook: skip the boot logos and movies

}
