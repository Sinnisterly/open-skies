#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace os::assets {

// Reader for No Man's Sky HGPAK v2 archives (zstd chunks of 64 KiB). The file is memory mapped.
class HgPak
{
public:
    HgPak() = default;
    ~HgPak();
    HgPak(const HgPak&) = delete;
    HgPak& operator=(const HgPak&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    size_t Count() const { return m_entries.size(); }
    std::vector<uint8_t> Read(size_t index, size_t limit = 0) const; // limit > 0 reads only the first bytes
    std::vector<std::string> Manifest() const;                      // entry 0 names entries 1..n

private:
    template <class T> T Rd(uint64_t off) const;
    struct Entry { uint64_t offset, size; };
    void* m_fileHandle = nullptr;
    void* m_map = nullptr;
    const uint8_t* m_data = nullptr;
    uint64_t m_size = 0;
    std::vector<Entry> m_entries;
    std::vector<uint64_t> m_chunkPos, m_chunkSize;
    uint64_t m_dataOffset = 0;
};

// Family name from a TrueType name table, empty if not a font.
std::string FontFamily(const std::vector<uint8_t>& ttf);

}
