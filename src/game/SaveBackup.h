#pragma once
#include <string>

namespace os::game {

// Copies the most recently used save folder (%APPDATA%\HelloGames\NMS\st_* on Steam, DefaultUser on GOG) once per session,
// before the first change that ends up in the save. Runs on a background thread.
void BackupSaveOnce();
std::wstring BackupFolder();

}
