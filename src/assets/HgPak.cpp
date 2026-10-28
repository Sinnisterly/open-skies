#include "assets/HgPak.h"
#include <windows.h>
#include <zstd.h>
#include <algorithm>
#include <cstring>

namespace os::assets {

static constexpr uint64_t kChunk = 0x10000;

HgPak::~HgPak() { Close(); }

void HgPak::Close()
{
    if (m_data) UnmapViewOfFile(m_data);
    if (m_map) CloseHandle(m_map);
    if (m_fileHandle) CloseHandle(m_fileHandle);
    m_data = nullptr;
    m_map = nullptr;
    m_fileHandle = nullptr;
    m_size = 0;
    m_entries.clear();
    m_chunkPos.clear();
    m_chunkSize.clear();
}

template <class T> T HgPak::Rd(uint64_t off) const
{
    T v{};
    if (off + sizeof(T) <= m_size) memcpy(&v, m_data + off, sizeof(T));
    return v;
}

bool HgPak::Open(const std::wstring& path)
{
    Close();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    m_fileHandle = f;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    m_size = (uint64_t)sz.QuadPart;
    m_map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m_map) { Close(); return false; }
    m_data = (const uint8_t*)MapViewOfFile(m_map, FILE_MAP_READ, 0, 0, 0);
    if (!m_data || m_size < 0x30 || memcmp(m_data, "HGPAK", 5) != 0 || Rd<uint64_t>(8) != 2) { Close(); return false; }
    if (Rd<uint64_t>(0x20) != 1) { Close(); return false; } // the uncompressed audio paks use another layout

    uint64_t files = Rd<uint64_t>(0x10);
    uint64_t chunks = Rd<uint64_t>(0x18);
    m_dataOffset = Rd<uint64_t>(0x28);

    uint64_t off = 0x30;
    for (uint64_t i = 0; i < files; ++i, off += 32)
        m_entries.push_back({ Rd<uint64_t>(off + 16), Rd<uint64_t>(off + 24) });

    uint64_t pos = m_dataOffset;
    for (uint64_t i = 0; i < chunks; ++i, off += 8) {
        uint64_t csz = Rd<uint64_t>(off);
        m_chunkPos.push_back(pos);
        m_chunkSize.push_back(csz);
        pos = (pos + csz + 15) & ~15ull;
    }
    if (pos > m_size + 16) { Close(); return false; }
    return true;
}

std::vector<uint8_t> HgPak::Read(size_t index, size_t limit) const
{
    if (index >= m_entries.size() || !m_data) return {};
    uint64_t start = m_entries[index].offset - m_dataOffset;
    uint64_t size = m_entries[index].size;
    if (limit && limit < size) size = limit;
    std::vector<uint8_t> out;
    out.reserve(size);

    std::vector<uint8_t> tmp(kChunk);
    for (uint64_t c = start / kChunk; out.size() < size && c < m_chunkPos.size(); ++c) {
        if (m_chunkPos[c] + m_chunkSize[c] > m_size) return {};
        const uint8_t* src = m_data + m_chunkPos[c];
        size_t got;
        if (m_chunkSize[c] == kChunk) { memcpy(tmp.data(), src, kChunk); got = kChunk; }
        else {
            got = ZSTD_decompress(tmp.data(), kChunk, src, m_chunkSize[c]);
            if (ZSTD_isError(got)) return {};
        }
        size_t from = out.empty() ? (size_t)(start % kChunk) : 0;
        if (from >= got) return {};
        size_t take = (size_t)std::min<uint64_t>(got - from, size - out.size());
        out.insert(out.end(), tmp.begin() + from, tmp.begin() + from + take);
    }
    if (out.size() != size) out.clear();
    return out;
}

std::vector<std::string> HgPak::Manifest() const
{
    std::vector<std::string> names;
    std::string cur;
    for (uint8_t c : Read(0)) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) names.push_back(std::move(cur));
            cur.clear();
        }
        else cur += (char)c;
    }
    if (!cur.empty()) names.push_back(std::move(cur));
    return names;
}

static uint16_t Be16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? (uint16_t)(b[o] << 8 | b[o + 1]) : 0; }
static uint32_t Be32(const std::vector<uint8_t>& b, size_t o) { return (uint32_t)Be16(b, o) << 16 | Be16(b, o + 2); }

std::string FontFamily(const std::vector<uint8_t>& t)
{
    if (t.size() < 12 || Be32(t, 0) != 0x00010000) return {};
    uint16_t tables = Be16(t, 4);
    for (uint16_t i = 0; i < tables; ++i) {
        size_t rec = 12 + 16 * (size_t)i;
        if (Be32(t, rec) != 0x6E616D65) continue; // 'name'
        size_t base = Be32(t, rec + 8);
        uint16_t count = Be16(t, base + 2), strOff = Be16(t, base + 4);
        for (uint16_t r = 0; r < count; ++r) {
            size_t nr = base + 6 + 12 * (size_t)r;
            if (Be16(t, nr) != 3 || Be16(t, nr + 6) != 4) continue; // Windows platform, full name
            uint16_t len = Be16(t, nr + 8), o = Be16(t, nr + 10);
            std::string s;
            for (size_t k = 0; k + 1 < len; k += 2) {
                uint16_t ch = Be16(t, base + strOff + o + k);
                s += ch < 128 ? (char)ch : '?';
            }
            return s;
        }
    }
    return {};
}

}
