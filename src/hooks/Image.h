#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace os::hooks {

using Range = std::pair<uint32_t, uint32_t>; // [begin, end) as RVAs

struct Pattern
{
    std::vector<int> bytes; // -1 is a wildcard
    static std::optional<Pattern> Parse(const std::string& text);
};

struct Operand
{
    enum Kind { None, Reg, Mem, Imm } kind = None;
    bool ripRel = false;
    int64_t disp = 0;
    int64_t imm = 0;
    int reg = 0;   // ZydisRegister of a Reg operand
    int base = 0;  // ZydisRegister of a Mem operand base
};

struct Insn
{
    uint32_t rva = 0;
    uint8_t len = 0;
    std::string mnemonic;
    std::vector<Operand> ops;
    std::optional<uint32_t> ripTarget;   // memory operand resolved to an RVA
    std::optional<uint32_t> branchTarget; // rel call/jmp target RVA
    bool isCall = false, isJmp = false;
    uint8_t dispOffset = 0; // byte offset of a 32 bit displacement, 0 if none
};

// Read-only view of the running NMS.exe image, used to find code to hook.
class Image
{
public:
    bool Load();
    static void UseModule(uintptr_t base); // read a mapped NMS.exe instead of the host

    uintptr_t Base() const { return m_base; }
    uint32_t Size() const { return m_size; }
    const uint8_t* At(uint32_t rva) const { return reinterpret_cast<const uint8_t*>(m_base + rva); }
    template <class T> T Read(uint32_t rva) const { T v; memcpy(&v, At(rva), sizeof v); return v; }

    const std::vector<Range>& Code() const { return m_code; }
    const std::vector<Range>& ReadOnly() const { return m_rdata; }
    bool InCode(uint32_t rva) const;
    std::string SectionOf(uint32_t rva) const;

    std::optional<Insn> Decode(uint32_t rva) const;
    std::vector<Insn> DecodeRange(uint32_t a, uint32_t b) const;

    std::vector<uint32_t> Scan(const Pattern& p, const std::vector<Range>& ranges, size_t limit = 16) const;
    std::vector<uint32_t> FindString(const std::string& s) const;

    std::optional<uint32_t> FuncOf(uint32_t rva) const;
    std::vector<Range> FuncRanges(uint32_t start) const;
    std::vector<uint32_t> Callees(uint32_t func, int depth) const;
    std::vector<uint32_t> Callers(uint32_t func) const;

    std::vector<uint32_t> RipRefs(uint32_t target) const;   // instruction starts
    std::vector<uint32_t> PtrRefs(uint32_t target) const;   // aligned qwords in data sections
    std::vector<uint32_t> Vtables(const std::string& cls) const;

    void DropIndexes(); // frees the big lookup tables once resolving is done

private:
    struct Section { std::string name; uint32_t a, b, flags; };
    struct RF { uint32_t begin, end, unwind; };
    int RfIndex(uint32_t rva) const;
    int PrimaryOf(int i) const;
    std::optional<Range> LeafBounds(uint32_t rva) const;
    void BuildBranchIndex() const;
    void BuildRipIndex() const;
    void BuildPtrIndex() const;
    void BuildFragments() const;

    uintptr_t m_base = 0;
    uint32_t m_size = 0;
    std::vector<Section> m_sections;
    std::vector<Range> m_code, m_rdata;
    const RF* m_rf = nullptr;
    size_t m_rfCount = 0;

    mutable std::vector<uint64_t> m_branch; // (target << 32) | source
    mutable std::vector<uint64_t> m_rip;    // (target << 32) | disp position
    mutable std::vector<uint64_t> m_ptr;    // (target << 32) | slot
    mutable std::vector<std::pair<uint32_t, Range>> m_frags; // primary begin -> fragment
    mutable bool m_fragsBuilt = false;
};

}
