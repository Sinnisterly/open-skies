#pragma once

namespace os::features {

void InstallWeapons(); // worker thread, inside Install
void WeaponsTick();    // render thread, a few times a second

}
