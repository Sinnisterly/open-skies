#include "game/SaveBackup.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <thread>

namespace os::game {

namespace fs = std::filesystem;

namespace {
std::atomic<bool> g_done{ false };

fs::path SaveRoot()
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    return n ? fs::path(buf) / L"HelloGames" / L"NMS" : fs::path();
}
}

std::wstring BackupFolder() { return (fs::path(paths::Root()) / L"OpenSkies.backups").wstring(); }

void BackupSaveOnce()
{
    if (g_done.exchange(true)) return;
    std::thread([] {
        try {
            fs::path root = SaveRoot(), newest;
            fs::file_time_type best{};
            std::error_code ec;
            for (auto& d : fs::directory_iterator(root, ec)) {
                std::wstring name = d.path().filename().wstring();
                if (!d.is_directory() || (name.rfind(L"st_", 0) != 0 && name != L"DefaultUser")) continue; // Steam, GOG
                for (auto& f : fs::directory_iterator(d.path(), ec)) {
                    auto t = f.last_write_time(ec);
                    if (!ec && t > best) { best = t; newest = d.path(); }
                }
            }
            if (newest.empty()) { log::Warn("Backup", "No save folder found under {}", root.string()); return; }
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t stamp[32];
            swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fs::path dst = fs::path(BackupFolder()) / (newest.filename().wstring() + L"-" + stamp);
            fs::create_directories(dst);
            fs::copy(newest, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            log::Info("Backup", "Save backed up to {}", dst.string());
        }
        catch (const std::exception& e) { log::Error("Backup", "Save backup failed: {}", e.what()); }
    }).detach();
}

}
