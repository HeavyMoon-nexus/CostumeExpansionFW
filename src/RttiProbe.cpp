#include "RttiProbe.h"

#include <Windows.h>

#include <cstring>

namespace CostumeFW::rtti
{
    namespace
    {
        // MSVC x64 RTTI records. Every cross-reference is a 32-bit offset from
        // the image base, which the locator carries for itself (self).
        struct CompleteObjectLocator
        {
            std::uint32_t signature;  // 1 on x64
            std::uint32_t offset;     // where this vtable's subobject sits in the object
            std::uint32_t cdOffset;
            std::int32_t typeDescriptor;
            std::int32_t classDescriptor;
            std::int32_t self;
        };

        struct ClassHierarchyDescriptor
        {
            std::uint32_t signature;
            std::uint32_t attributes;
            std::uint32_t numBaseClasses;  // the class itself is entry 0
            std::int32_t baseClassArray;
        };

        struct BaseClassDescriptor
        {
            std::int32_t typeDescriptor;
            std::uint32_t numContainedBases;
            std::int32_t mdisp, pdisp, vdisp;
            std::uint32_t attributes;
            std::int32_t classDescriptor;
        };

        // TypeDescriptor: a vtable pointer, a spare pointer, then the name.
        constexpr std::size_t kTypeNameOffset = 16;
        constexpr std::size_t kMaxName = 255;
        // Engine classes sit a dozen deep at most; past this the record is not
        // a class hierarchy.
        constexpr std::uint32_t kMaxBases = 64;

        // The only places that touch memory: plain copies under SEH, with no
        // object that needs unwinding in the frame (C2712).
        bool CopySafe(void* a_dst, const void* a_src, std::size_t a_size)
        {
            __try {
                std::memcpy(a_dst, a_src, a_size);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        // A NUL-terminated name of at most kMaxName bytes, one byte at a time:
        // a fixed-size copy could run off the end of the section the name
        // sits at the end of.
        bool ReadName(const char* a_src, char (&a_buf)[kMaxName + 1])
        {
            __try {
                std::size_t i = 0;
                for (; i < kMaxName && a_src[i] != '\0'; ++i) {
                    a_buf[i] = a_src[i];
                }
                a_buf[i] = '\0';
                return i < kMaxName;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        std::uintptr_t At(std::uintptr_t a_base, std::int32_t a_rva)
        {
            return a_base + static_cast<std::uint32_t>(a_rva);
        }

        // The base of the loaded image a_addr lies in, or 0.
        std::uintptr_t ModuleBaseOf(std::uintptr_t a_addr)
        {
            HMODULE mod = nullptr;
            if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(a_addr), &mod)) {
                return 0;
            }
            return reinterpret_cast<std::uintptr_t>(mod);
        }

        bool LocatorFor(std::uintptr_t a_vtable, CompleteObjectLocator& a_col,
            std::uintptr_t& a_base)
        {
            if (a_vtable < 0x10000 || (a_vtable & 7) != 0) {
                return false;
            }
            const std::uintptr_t base = ModuleBaseOf(a_vtable);
            if (base == 0) {
                return false;  // heap, stack, or nothing at all
            }
            std::uintptr_t colAddr = 0;
            if (!CopySafe(&colAddr, reinterpret_cast<const void*>(a_vtable - 8), sizeof(colAddr)) ||
                !CopySafe(&a_col, reinterpret_cast<const void*>(colAddr), sizeof(a_col))) {
                return false;
            }
            // The self-check is what makes a random image address fail: the
            // locator must name its own position relative to the real base.
            if (a_col.signature != 1 || At(base, a_col.self) != colAddr) {
                return false;
            }
            a_base = base;
            return true;
        }

        bool NameAt(std::uintptr_t a_base, std::int32_t a_typeDescriptor,
            char (&a_buf)[kMaxName + 1])
        {
            return ReadName(
                reinterpret_cast<const char*>(At(a_base, a_typeDescriptor) + kTypeNameOffset), a_buf);
        }

        // ".?AVNiAVObject@@" (class) or ".?AUNiAVObject@@" (struct).
        bool NameIs(const char* a_mangled, std::string_view a_class)
        {
            const std::string_view m{ a_mangled };
            return m.size() == a_class.size() + 6 && m.starts_with(".?A") &&
                   (m[3] == 'V' || m[3] == 'U') && m.substr(4, a_class.size()) == a_class &&
                   m.ends_with("@@");
        }
    }

    bool ReadPointer(const void* a_addr, std::uintptr_t& a_out)
    {
        std::uintptr_t v = 0;
        if (!CopySafe(&v, a_addr, sizeof(v))) {
            return false;
        }
        a_out = v;
        return true;
    }

    bool VtableIsA(std::uintptr_t a_vtable, std::string_view a_class)
    {
        CompleteObjectLocator col{};
        std::uintptr_t base = 0;
        if (!LocatorFor(a_vtable, col, base)) {
            return false;
        }
        ClassHierarchyDescriptor chd{};
        if (!CopySafe(&chd, reinterpret_cast<const void*>(At(base, col.classDescriptor)), sizeof(chd)) ||
            chd.numBaseClasses == 0 || chd.numBaseClasses > kMaxBases) {
            return false;
        }
        for (std::uint32_t i = 0; i < chd.numBaseClasses; ++i) {
            std::int32_t bcdRva = 0;
            BaseClassDescriptor bcd{};
            if (!CopySafe(&bcdRva,
                    reinterpret_cast<const void*>(At(base, chd.baseClassArray) + i * sizeof(bcdRva)),
                    sizeof(bcdRva)) ||
                !CopySafe(&bcd, reinterpret_cast<const void*>(At(base, bcdRva)), sizeof(bcd))) {
                return false;
            }
            char name[kMaxName + 1]{};
            if (NameAt(base, bcd.typeDescriptor, name) && NameIs(name, a_class)) {
                return true;
            }
        }
        return false;
    }

    std::string VtableTypeName(std::uintptr_t a_vtable)
    {
        CompleteObjectLocator col{};
        std::uintptr_t base = 0;
        char name[kMaxName + 1]{};
        if (!LocatorFor(a_vtable, col, base) || !NameAt(base, col.typeDescriptor, name)) {
            return {};
        }
        return name;
    }
}
