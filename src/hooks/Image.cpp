#include "hooks/Image.h"
#include <Zydis/Zydis.h>
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <set>

namespace os::hooks {

std::optional<Pattern> Pattern::Parse(const std::string& text)
{
    Pattern p;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        size_t j = text.find(' ', i);
        std::string tok = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? text.size() : j;
        if (tok.empty()) continue;
        if (tok == "?" || tok == "??" || tok == "*" || tok == "**") { p.bytes.push_back(-1); continue; }
        if (tok.size() != 2 || !isxdigit((unsigned char)tok[0]) || !isxdigit((unsigned char)tok[1])) return std::nullopt;
        p.bytes.push_back((int)strtoul(tok.c_str(), nullptr, 16));
    }
    if (p.bytes.empty()) return std::nullopt;
    return p;
}

static uintptr_t g_module = 0;

void Image::UseModule(uintptr_t base) { g_module = base; }

bool Image::Load()
{
    m_base = g_module ? g_module : (uintptr_t)GetModuleHandleW(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)m_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(m_base + dos->e_lfanew);
    m_size = nt->OptionalHeader.SizeOfImage;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9] = {};
        memcpy(name, sec->Name, 8);
        uint32_t a = sec->VirtualAddress, b = a + std::max<uint32_t>(sec->Misc.VirtualSize, sec->SizeOfRawData);
        m_sections.push_back({ name, a, b, sec->Characteristics });
        if (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) m_code.push_back({ a, b });
        else if (sec->Characteristics & IMAGE_SCN_MEM_READ) m_rdata.push_back({ a, b });
    }
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    m_rf = (const RF*)(m_base + dir.VirtualAddress);
    m_rfCount = dir.Size / sizeof(RF);
    return m_rfCount > 0 && !m_code.empty();
}

bool Image::InCode(uint32_t rva) const
{
    for (auto& [a, b] : m_code) if (rva >= a && rva < b) return true;
    return false;
}

std::string Image::SectionOf(uint32_t rva) const
{
    for (auto& s : m_sections) if (rva >= s.a && rva < s.b) return s.name;
    return {};
}

// ---------------------------------------------------------------- decoding

static const ZydisDecoder& Decoder()
{
    static ZydisDecoder d = [] { ZydisDecoder x; ZydisDecoderInit(&x, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64); return x; }();
    return d;
}

// Capstone style names, as targets.json uses them.
static std::string Mnemonic(ZydisMnemonic m)
{
    std::string s = ZydisMnemonicGetString(m);
    static const std::pair<const char*, const char*> alias[] = {
        { "jz", "je" }, { "jnz", "jne" }, { "jnbe", "ja" }, { "jnb", "jae" }, { "jnle", "jg" }, { "jnl", "jge" },
        { "cmovz", "cmove" }, { "cmovnz", "cmovne" }, { "cmovnle", "cmovg" }, { "cmovnl", "cmovge" },
        { "cmovnbe", "cmova" }, { "cmovnb", "cmovae" }, { "setz", "sete" }, { "setnz", "setne" },
    };
    for (auto& [z, c] : alias) if (s == z) return c;
    return s;
}

std::optional<Insn> Image::Decode(uint32_t rva) const
{
    if (rva + 1 > m_size) return std::nullopt;
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    size_t avail = std::min<size_t>(15, m_size - rva);
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), At(rva), avail, &in, ops))) return std::nullopt;
    Insn out;
    out.rva = rva;
    out.len = in.length;
    out.mnemonic = Mnemonic(in.mnemonic);
    out.isCall = in.mnemonic == ZYDIS_MNEMONIC_CALL;
    out.isJmp = in.mnemonic == ZYDIS_MNEMONIC_JMP;
    uintptr_t rt = m_base + rva;
    for (int i = 0; i < in.operand_count_visible; ++i) {
        const auto& o = ops[i];
        Operand op;
        switch (o.type) {
        case ZYDIS_OPERAND_TYPE_REGISTER: op.kind = Operand::Reg; op.reg = o.reg.value; break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            op.kind = Operand::Mem;
            op.base = o.mem.base;
            op.disp = o.mem.disp.value;
            if (o.mem.base == ZYDIS_REGISTER_RIP) {
                ZyanU64 abs;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &o, rt, &abs))) { op.ripRel = true; out.ripTarget = (uint32_t)(abs - m_base); }
            }
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            op.kind = Operand::Imm;
            op.imm = o.imm.value.s;
            if (o.imm.is_relative) {
                ZyanU64 abs;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &o, rt, &abs))) out.branchTarget = (uint32_t)(abs - m_base);
            }
            break;
        default: break;
        }
        out.ops.push_back(op);
    }
    if (in.raw.disp.size == 32) out.dispOffset = in.raw.disp.offset;
    return out;
}

std::vector<Insn> Image::DecodeRange(uint32_t a, uint32_t b) const
{
    std::vector<Insn> out;
    uint32_t cur = a;
    while (cur < b) {
        auto in = Decode(cur);
        if (!in) { ++cur; continue; }
        cur += in->len;
        out.push_back(std::move(*in));
    }
    return out;
}

// ---------------------------------------------------------------- scanning

std::vector<uint32_t> Image::Scan(const Pattern& p, const std::vector<Range>& ranges, size_t limit) const
{
    std::vector<uint32_t> hits;
    const auto& pb = p.bytes;
    size_t lead = 0;
    while (lead < pb.size() && pb[lead] < 0) ++lead;
    for (auto& [a, b] : ranges) {
        if (b - a < pb.size()) continue;
        const uint8_t* base = At(0);
        uint32_t last = b - (uint32_t)pb.size();
        for (uint32_t pos = a; pos <= last && hits.size() < limit;) {
            if (lead < pb.size()) {
                const void* f = memchr(base + pos + lead, pb[lead], last - pos + 1);
                if (!f) break;
                pos = (uint32_t)((const uint8_t*)f - base) - (uint32_t)lead;
            }
            bool ok = true;
            for (size_t k = 0; k < pb.size() && ok; ++k)
                if (pb[k] >= 0 && base[pos + k] != (uint8_t)pb[k]) ok = false;
            if (ok) hits.push_back(pos);
            ++pos;
        }
    }
    return hits;
}

std::vector<uint32_t> Image::FindString(const std::string& s) const
{
    std::vector<uint32_t> out;
    Pattern p;
    for (char c : s) p.bytes.push_back((uint8_t)c);
    p.bytes.push_back(0);
    for (uint32_t h : Scan(p, m_rdata, 64))
        if (h > 0 && *At(h - 1) == 0) out.push_back(h);
    return out;
}

// ---------------------------------------------------------------- functions

int Image::RfIndex(uint32_t rva) const
{
    size_t lo = 0, hi = m_rfCount;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (m_rf[mid].begin <= rva) lo = mid + 1; else hi = mid; }
    if (lo == 0) return -1;
    int i = (int)lo - 1;
    return rva < m_rf[i].end ? i : -1;
}

int Image::PrimaryOf(int i) const
{
    for (int seen = 0; seen < 32; ++seen) {
        uint32_t u = m_rf[i].unwind;
        if (u & 1) break;
        uint8_t flags = *At(u) >> 3;
        if (!(flags & 4)) break; // UNW_FLAG_CHAININFO
        uint8_t count = *At(u + 2);
        uint32_t begin = Read<uint32_t>(u + 4 + ((count + 1) & ~1) * 2);
        int j = RfIndex(begin);
        if (j < 0 || j == i) break;
        i = j;
    }
    return i;
}

std::optional<Range> Image::LeafBounds(uint32_t rva) const
{
    if (!InCode(rva)) return std::nullopt;
    size_t lo = 0, hi = m_rfCount;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (m_rf[mid].begin <= rva) lo = mid + 1; else hi = mid; }
    int i = (int)lo - 1;
    uint32_t lower = i >= 0 ? m_rf[i].end : rva - 0x1000;
    uint32_t upper = i + 1 < (int)m_rfCount ? m_rf[i + 1].begin : rva + 0x1000;
    uint32_t a = rva, b = rva;
    while (a > lower && a > rva - 0x1000 && *At(a - 1) != 0xCC) --a;
    while (b < upper && b < rva + 0x1000 && !(*At(b) == 0xCC && *At(b + 1) == 0xCC)) ++b;
    uint32_t cur = a;
    while (cur < rva) {
        auto in = Decode(cur);
        if (!in) return std::nullopt;
        cur += in->len;
    }
    if (cur != rva) return std::nullopt;
    return Range{ a, b };
}

std::optional<uint32_t> Image::FuncOf(uint32_t rva) const
{
    int i = RfIndex(rva);
    if (i < 0) {
        auto lb = LeafBounds(rva);
        if (!lb) return std::nullopt;
        return lb->first;
    }
    return m_rf[PrimaryOf(i)].begin;
}

void Image::BuildFragments() const
{
    for (size_t i = 0; i < m_rfCount; ++i) {
        uint32_t u = m_rf[i].unwind;
        if (u & 1 || !((*At(u) >> 3) & 4)) continue;
        int j = PrimaryOf((int)i);
        if (j != (int)i) m_frags.push_back({ m_rf[j].begin, { m_rf[i].begin, m_rf[i].end } });
    }
    std::sort(m_frags.begin(), m_frags.end());
    m_fragsBuilt = true;
}

std::vector<Range> Image::FuncRanges(uint32_t start) const
{
    int i = RfIndex(start);
    if (i < 0 || m_rf[i].begin != start) {
        if (auto lb = LeafBounds(start)) return { *lb };
        return { { start, start + 0x40 } };
    }
    if (!m_fragsBuilt) BuildFragments();
    std::vector<Range> out{ { m_rf[i].begin, m_rf[i].end } };
    auto it = std::lower_bound(m_frags.begin(), m_frags.end(), std::make_pair(start, Range{ 0, 0 }));
    for (; it != m_frags.end() && it->first == start; ++it) out.push_back(it->second);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<uint32_t> Image::Callees(uint32_t func, int depth) const
{
    std::set<uint32_t> seen;
    std::vector<uint32_t> frontier{ func };
    for (int d = 0; d < depth; ++d) {
        std::vector<uint32_t> next;
        for (uint32_t f : frontier) {
            auto ranges = FuncRanges(f);
            for (auto& [a, b] : ranges)
                for (auto& in : DecodeRange(a, b)) {
                    if (!(in.isCall || in.isJmp) || !in.branchTarget) continue;
                    uint32_t t = *in.branchTarget;
                    if (in.isJmp && std::any_of(ranges.begin(), ranges.end(), [&](const Range& r) { return t >= r.first && t < r.second; })) continue;
                    auto g = FuncOf(t);
                    if (g && *g != func && seen.insert(*g).second) next.push_back(*g);
                }
        }
        frontier = std::move(next);
    }
    return { seen.begin(), seen.end() };
}

// ---------------------------------------------------------------- indexes

void Image::BuildBranchIndex() const
{
    if (!m_branch.empty()) return;
    for (auto& [a, b] : m_code) {
        const uint8_t* m = At(0);
        for (uint32_t p = a; p + 5 < b; ++p) {
            if (m[p] != 0xE8 && m[p] != 0xE9) continue;
            int32_t rel;
            memcpy(&rel, m + p + 1, 4);
            int64_t t = (int64_t)p + 5 + rel;
            if (t >= a && t < b) m_branch.push_back((uint64_t)t << 32 | p);
        }
    }
    std::sort(m_branch.begin(), m_branch.end());
}

std::vector<uint32_t> Image::Callers(uint32_t func) const
{
    BuildBranchIndex();
    std::set<uint32_t> out;
    auto it = std::lower_bound(m_branch.begin(), m_branch.end(), (uint64_t)func << 32);
    for (; it != m_branch.end() && (*it >> 32) == func; ++it) {
        auto f = FuncOf((uint32_t)*it);
        if (f && *f != func) out.insert(*f);
    }
    return { out.begin(), out.end() };
}

void Image::BuildRipIndex() const
{
    if (!m_rip.empty()) return;
    for (auto& [a, b] : m_code) {
        const uint8_t* m = At(0);
        for (uint32_t p = a; p + 5 < b; ++p) {
            if ((m[p] & 0xC7) != 0x05) continue;
            uint32_t d = p + 1;
            int32_t rel;
            memcpy(&rel, m + d, 4);
            int64_t t = (int64_t)d + 4 + rel;
            if (t >= 0x1000 && t < (int64_t)m_size + 8) m_rip.push_back((uint64_t)t << 32 | d);
        }
    }
    std::sort(m_rip.begin(), m_rip.end());
}

std::vector<uint32_t> Image::RipRefs(uint32_t target) const
{
    BuildRipIndex();
    std::set<uint32_t> out;
    uint64_t lo = (uint64_t)(target >= 4 ? target - 4 : 0) << 32, hi = ((uint64_t)target << 32) | 0xFFFFFFFF;
    for (auto it = std::lower_bound(m_rip.begin(), m_rip.end(), lo); it != m_rip.end() && *it <= hi; ++it) {
        uint32_t d = (uint32_t)*it;
        for (int k = 7; k >= 2; --k) {
            auto in = Decode(d - k);
            if (!in || in->len < k + 4 || in->ripTarget != target) continue;
            if (in->dispOffset && in->rva + in->dispOffset == d) { out.insert(d - k); break; }
        }
    }
    return { out.begin(), out.end() };
}

void Image::BuildPtrIndex() const
{
    if (!m_ptr.empty()) return;
    uint64_t lo = m_base, hi = m_base + m_size;
    for (auto& s : m_sections) {
        if ((s.flags & IMAGE_SCN_MEM_EXECUTE) || s.name == ".pdata" || s.name == ".reloc" || s.name == ".rsrc") continue;
        for (uint32_t p = (s.a + 7) & ~7u; p + 8 <= s.b; p += 8) {
            uint64_t v = Read<uint64_t>(p);
            if (v >= lo && v < hi) m_ptr.push_back((v - lo) << 32 | p);
        }
    }
    std::sort(m_ptr.begin(), m_ptr.end());
}

std::vector<uint32_t> Image::PtrRefs(uint32_t target) const
{
    BuildPtrIndex();
    std::vector<uint32_t> out;
    for (auto it = std::lower_bound(m_ptr.begin(), m_ptr.end(), (uint64_t)target << 32); it != m_ptr.end() && (*it >> 32) == target; ++it)
        out.push_back((uint32_t)*it);
    return out;
}

std::vector<uint32_t> Image::Vtables(const std::string& cls) const
{
    std::vector<uint32_t> out;
    std::string name = ".?AV" + cls + "@@";
    Pattern p;
    for (char c : name) p.bytes.push_back((uint8_t)c);
    p.bytes.push_back(0);
    std::vector<Range> all;
    for (auto& s : m_sections) if (!(s.flags & IMAGE_SCN_MEM_EXECUTE)) all.push_back({ s.a, s.b });
    for (uint32_t hit : Scan(p, all, 8)) {
        uint32_t td = hit - 0x10;
        for (auto& [a, b] : m_rdata)
            for (uint32_t c = a; c + 24 <= b; c += 4) {
                if (Read<uint32_t>(c) != 1 || Read<uint32_t>(c + 20) != c || Read<uint32_t>(c + 12) != td) continue;
                if (Read<uint32_t>(c + 4) != 0) continue;
                for (uint32_t slot : PtrRefs(c)) out.push_back(slot + 8);
            }
    }
    return out;
}

void Image::DropIndexes()
{
    std::vector<uint64_t>().swap(m_branch);
    std::vector<uint64_t>().swap(m_rip);
    std::vector<uint64_t>().swap(m_ptr);
}

}
