// pf2-nextbot: call a virtual function on a real game object by vtable slot (MSVC x86 thiscall).
// Slots come from tools/vtdump.py (Linux index - 1) and are listed in game/pf2_offsets.h.
#pragma once

namespace pf2 {

class VEmpty {};   // complete, single-inheritance: member-function pointers are 4 bytes

template <typename R, typename... A>
inline R VCall(const void *thisptr, int index, A... args)
{
	typedef R (VEmpty::*Fn)(A...);
	static_assert(sizeof(Fn) == sizeof(void *), "unexpected member-function-pointer size");
	void **vt = *(void ***)thisptr;
	union { void *addr; Fn fn; } u;
	u.addr = vt[index];
	return (((VEmpty *)thisptr)->*u.fn)(args...);
}

} // namespace pf2
