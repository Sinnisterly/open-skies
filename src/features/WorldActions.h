#pragma once

namespace os::features {

void InstallWorldActions(); // worker thread, inside Install
void WorldActionsTick();    // render thread

}
