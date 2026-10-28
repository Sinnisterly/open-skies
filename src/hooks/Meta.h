#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace os::hooks {

// The game's own reflection data: class records and their members, looked up by libMBIN name.
namespace meta {

enum Type : uint32_t
{
    Bool = 0x1, Class = 0x3, DynArray = 0x7, String = 0x8, Enum = 0xB, VarString = 0xC, Float = 0xE, TkID = 0x10,
    Int32 = 0x15, StaticArray = 0x1A, FixedString = 0x1D, UInt64 = 0x25, Vector4 = 0x29,
};

struct Member
{
    std::string name;
    uint32_t type = 0, size = 0, count = 0, offset = 0;
    uintptr_t cls = 0; // class record of a nested struct, or 0
    std::vector<std::pair<int, std::string>> enums; // value names of an Enum member
};

struct ClassInfo
{
    std::string name;
    uint32_t size = 0;
    std::vector<Member> members;
    const Member* Find(const std::string& n) const;
};

const ClassInfo* Get(const std::string& cls);   // cached, nullptr if unknown
const ClassInfo* FromRecord(uintptr_t record);  // nested class by record address

// Value names of an enum field, also through the one-member wrapper classes (Type -> cGcProductCategory).
std::vector<std::pair<int, std::string>> EnumNames(const std::string& cls, const std::string& field);

// Dotted path like "Control.SpaceEngine.MaxSpeed"; returns the final member with its absolute offset.
std::optional<Member> Path(const std::string& cls, const std::string& path);

}

}
