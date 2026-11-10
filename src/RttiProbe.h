#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// Read-only questions about an object's dynamic type, answered from MSVC's x64
// RTTI without calling anything through the object. Pure Windows + C++ (no
// CommonLib), so the host tests can hold it to classes they define themselves:
// the layout of the locator is the compiler's, whoever wrote the class.
//
// Why it exists (BUGREPORT 2026-10-18, Baki): the scene walks CEF makes check a
// child pointer's VALUE (PlausibleObjectPtr) and then read its name. A pointer
// to real heap memory passes the value check whatever that memory has become -
// and the crash was exactly that: a CEF holder whose first 0x18 bytes had been
// overwritten with a NiTObjectArray (vtable, data, {6,6,6,1}), so "name" was
// the array's size fields and strlen read through them. Asking what the vtable
// belongs to catches it before anything is read through the object.
namespace CostumeFW::rtti
{
    // The 8 bytes at a_addr, or false (a_out untouched) when they cannot be
    // read. Never faults.
    bool ReadPointer(const void* a_addr, std::uintptr_t& a_out);

    // Whether a_vtable is the primary vtable of a class that is, or derives
    // from, a_class (the plain class name, e.g. "NiAVObject"). The Complete
    // Object Locator at vtable[-1] is only believed when the image the vtable
    // sits in is a loaded module and the locator's self-RVA puts that image
    // exactly where the loader has it - a value that merely looks like a
    // pointer cannot pass. Reads only; every read is guarded, so junk answers
    // false instead of faulting.
    bool VtableIsA(std::uintptr_t a_vtable, std::string_view a_class);

    // The most-derived type's name as MSVC mangles it (".?AVNiNode@@"), or ""
    // when a_vtable does not lead to a well-formed locator. For logs.
    std::string VtableTypeName(std::uintptr_t a_vtable);
}
