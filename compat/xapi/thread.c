/*
 * Standalone compat: XAPILIB routines kb.json only carries `void f(void)`
 * placeholders for.
 *
 * The originals are __stdcall (RaiseException 0x1cfb12 ends in `ret 0x10`,
 * PulseEvent 0x1cfee8 in `ret 4`); nxdk's winapi versions are cdecl. Bind
 * the real signatures to the decorated names so the legacy trampolines reach
 * them with the callee-pops ABI the game's callers expect.
 */
#include "xbox.h"

void __cdecl nxdk_RaiseException(unsigned long code, unsigned long flags, unsigned long count,
                                 const unsigned long *arguments) __asm__("_RaiseException");
int __cdecl nxdk_PulseEvent(void *event) __asm__("_PulseEvent");

void __stdcall standalone_RaiseException(unsigned long code, unsigned long flags,
                                         unsigned long count, const unsigned long *arguments)
    __asm__("_RaiseException@16");
int __stdcall standalone_PulseEvent(void *event) __asm__("_PulseEvent@4");

void __stdcall standalone_RaiseException(unsigned long code, unsigned long flags,
                                         unsigned long count, const unsigned long *arguments)
{
  nxdk_RaiseException(code, flags, count, arguments);
}

int __stdcall standalone_PulseEvent(void *event)
{
  return nxdk_PulseEvent(event);
}
