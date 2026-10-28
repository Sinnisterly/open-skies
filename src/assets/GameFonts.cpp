#include "assets/GameFonts.h"
#include "assets/HgPak.h"
#include "core/AppLog.h"
#include "core/AppPaths.h"
#include <fstream>

namespace os::assets {

static std::wstring PakPath(const std::wstring& root) { return root + L"\\GAMEDATA\\PCBANKS\\NMSARC.fonts.pak"; }

static FontBlob FromFile(const wchar_t* path, const char* name)
{
    FontBlob b;
    std::ifstream f(path, std::ios::binary);
    if (f) { b.data.assign(std::istreambuf_iterator<char>(f), {}); b.name = name; }
    return b;
}

FontSet LoadFonts(const std::wstring& gameRoot)
{
    // Ordered preferences per role, matched against the font's full name.
    const std::vector<std::string> bodyWant    = { "Roboto", "San Francisco Display Medium" };
    const std::vector<std::string> headingWant = { "Futura Pro Book", "Keep Calm Medium" };
    const std::vector<std::string> iconWant    = { "Font Awesome 6 Free Solid" };
    const std::vector<std::string> monoWant    = { "Roboto Mono", "Azeret Mono" };

    FontSet set;
    HgPak pak;
    if (pak.Open(PakPath(gameRoot)))
    {
        struct Found { std::string name; size_t index; };
        std::vector<Found> fonts;
        for (size_t i = 0; i < pak.Count(); ++i) {
            auto data = pak.Read(i);
            std::string fam = FontFamily(data);
            if (!fam.empty()) fonts.push_back({ fam, i });
        }
        // Exact name first so "Roboto" does not grab "Roboto Black".
        auto pick = [&](const std::vector<std::string>& want, FontBlob& out) {
            for (auto& w : want) {
                for (auto& f : fonts)
                    if (f.name == w) { out.name = f.name; out.data = pak.Read(f.index); return; }
                for (auto& f : fonts)
                    if (f.name.find(w) == 0) { out.name = f.name; out.data = pak.Read(f.index); return; }
            }
        };
        pick(bodyWant, set.body);
        pick(headingWant, set.heading);
        pick(iconWant, set.icons);
        pick(monoWant, set.mono);
        log::Info("Fonts", "Game fonts: title '{}', body '{}', icons '{}'", set.heading.name, set.body.name, set.icons.name);
    }
    else log::Warn("Fonts", "Could not read {}, using Windows fonts", paths::Utf8(PakPath(gameRoot)));

    if (!set.body.Ok()) set.body = FromFile(L"C:\\Windows\\Fonts\\segoeui.ttf", "Segoe UI");
    if (!set.heading.Ok()) set.heading = FromFile(L"C:\\Windows\\Fonts\\seguisb.ttf", "Segoe UI Semibold");
    if (!set.mono.Ok()) set.mono = FromFile(L"C:\\Windows\\Fonts\\consola.ttf", "Consolas");
    return set;
}

std::vector<std::string> ListPakFonts(const std::wstring& gameRoot)
{
    std::vector<std::string> out;
    HgPak pak;
    if (!pak.Open(PakPath(gameRoot))) return out;
    for (size_t i = 0; i < pak.Count(); ++i) {
        std::string fam = FontFamily(pak.Read(i));
        if (!fam.empty()) out.push_back(fam);
    }
    return out;
}

}
