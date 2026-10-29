/* Included ahead of every C++ file of the game (CMakeLists.txt).
 *
 * Upstream's u7port.h ends with #pragma pack(1), left on for the rest of each file. GCC lays out
 * its own type_info descriptors (class, single and multiple inheritance) when a file first needs
 * them, under whatever packing is on then, so under pack(1) each class's type_info gets
 * alignment 1 and lands wherever its odd-length name string ends. Its pointers then need
 * relocating at odd addresses, and the TOS loader refuses such a program: Pexec returns EPLFMT
 * (-66, "TOS error #35" on the desktop), while Hatari's own loader runs it. Asking for typeids
 * here, before any packing, lays the descriptors out with their natural alignment. A class with
 * more bases than below would still get a packed one; tools/prg-relocs.py, run after each link,
 * would say so. Nothing is
 * emitted for these classes: the functions are inline and never called.
 */
#ifndef ATARI_RTTI_ALIGN_H
#define ATARI_RTTI_ALIGN_H

#ifdef __cplusplus
#include <typeinfo>

/* GCC has a descriptor for a class with no base, one with a single public base, and one per
 * count of bases for any other inheritance (a virtual or non-public base, or several). */
namespace u7_rtti_align {
struct Base { virtual ~Base(); };
struct Single : Base { virtual ~Single(); };
struct Second { virtual ~Second(); };
struct Third { virtual ~Third(); };
struct One : virtual Base { virtual ~One(); };
struct Two : Base, Second { virtual ~Two(); };
struct Three : Base, Second, Third { virtual ~Three(); };
inline const std::type_info &base() { return typeid(Base); }
inline const std::type_info &single() { return typeid(Single); }
inline const std::type_info &one() { return typeid(One); }
inline const std::type_info &two() { return typeid(Two); }
inline const std::type_info &three() { return typeid(Three); }
}
#endif

#endif
