#pragma once

namespace os::features {

void InstallFlight(); // worker thread, inside Install: fly and no-clip on foot
void InstallCreatures(); // worker thread, inside Install: taming
void InstallEncounters(); // worker thread, inside Install: no pirates, no wanted level

// Game thread. Where the camera is (world) and the unit direction it looks.
bool CameraRay(float pos[3], float dir[3]);

}
