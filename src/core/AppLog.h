#pragma once
#include <string>
#include <vector>
#include <format>

namespace os::log {

enum class Level { Debug, Info, Warn, Error };

void Init(const std::wstring& path);
void Shutdown();
void SetVerbose(bool on);
bool Verbose();
void Write(Level lvl, const char* cat, const std::string& msg);
std::vector<std::string> Recent(size_t max);

template <class... A> void Debug(const char* cat, std::format_string<A...> f, A&&... a) { if (Verbose()) Write(Level::Debug, cat, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Info(const char* cat, std::format_string<A...> f, A&&... a) { Write(Level::Info, cat, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Warn(const char* cat, std::format_string<A...> f, A&&... a) { Write(Level::Warn, cat, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Error(const char* cat, std::format_string<A...> f, A&&... a) { Write(Level::Error, cat, std::format(f, std::forward<A>(a)...)); }

}
