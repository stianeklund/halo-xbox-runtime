/* Shared declarations for the Bink compatibility layer (compat/bink/).
 * In the title build the FUN_ routines are RAD internals declared by kb.json
 * (decl.h, force-included); HALO_BINK_HOST_TEST builds the codec files
 * natively for tools/test_bink.py. */
#ifndef HALO_BINK_INTERNAL_H
#define HALO_BINK_INTERNAL_H

#ifdef HALO_BINK_HOST_TEST
#include <string.h>
#define __stdcall
#define csmemset memset
void FUN_00236210(unsigned long *sizes, unsigned long width);
void FUN_00238070(void *plane_prev, void *plane_cur, void *aplane_prev, void *aplane_cur,
                  unsigned char *mask, unsigned long width, unsigned long height,
                  unsigned long mask_width, unsigned long mask_height, const unsigned char *input,
                  unsigned long keyframe, void *field_3ac, unsigned long open_flags,
                  unsigned long bink_type);
void *FUN_0023cd10(unsigned long frequency, unsigned long channels, unsigned long flags);
void FUN_0023ce50(void *decoder);
void FUN_0023ec50(void *decoder, void **output, unsigned long *output_bytes,
                  const unsigned char *input, const unsigned char **next_input);
void FUN_00234110(unsigned long surface_type);
void FUN_00244b40(void *dest, int x, int y, int pitch, void *y_plane, int src_x, int src_y,
                  unsigned long width, unsigned long height, long plane_width, long plane_height,
                  unsigned long flags);
void FUN_00244b90(void *dest, int x, int y, int pitch, const unsigned char *mask, long mask_pitch,
                  void *y_plane, unsigned long width, unsigned long height, long plane_width,
                  long plane_height, unsigned long flags);
/* radmalloc/radfree (0x2314b0/0x231520); the host test supplies them. */
void *FUN_002314b0(unsigned long size);
void FUN_00231520(void *allocation);
#else
/* nxdk's winapi is cdecl; private names keep decl.h's stdcall
 * declarations of the same functions out of the way. */
typedef unsigned long(__stdcall *rad_thread_proc)(void *);

void *__cdecl rad_CreateThread(void *security, unsigned long stack, rad_thread_proc start, void *param,
                               unsigned long flags, unsigned long *id) __asm__("_CreateThread");
unsigned long __cdecl rad_ResumeThread(void *thread) __asm__("_ResumeThread");
void *__cdecl rad_CreateMutexA(void *security, int initial_owner, const char *name)
  __asm__("_CreateMutexA");
int __cdecl rad_ReleaseMutex(void *mutex) __asm__("_ReleaseMutex");
void *__cdecl rad_CreateEventA(void *security, int manual_reset, int initial_state,
                               const char *name) __asm__("_CreateEventA");
int __cdecl rad_SetEvent(void *event) __asm__("_SetEvent");
unsigned long __cdecl rad_WaitForSingleObject(void *handle, unsigned long timeout)
  __asm__("_WaitForSingleObject");
unsigned long __cdecl rad_WaitForMultipleObjects(unsigned long count, void *const *handles,
                                                 int wait_all, unsigned long timeout)
  __asm__("_WaitForMultipleObjects");
int __cdecl rad_CloseHandle(void *handle) __asm__("_CloseHandle");
void __cdecl rad_Sleep(unsigned long milliseconds) __asm__("_Sleep");
unsigned long __cdecl rad_GetTickCount(void) __asm__("_GetTickCount");
void *__cdecl rad_CreateFileA(const char *name, unsigned long access, unsigned long share,
                              void *security, unsigned long disposition, unsigned long flags,
                              void *template_file) __asm__("_CreateFileA");
int __cdecl rad_ReadFile(void *handle, void *buffer, unsigned long size, unsigned long *read,
                         void *overlapped) __asm__("_ReadFile");
unsigned long __cdecl rad_SetFilePointer(void *handle, long distance, long *distance_high,
                                         unsigned long method) __asm__("_SetFilePointer");
void *__cdecl rad_malloc(unsigned long size) __asm__("_malloc");
void __cdecl rad_free(void *memory) __asm__("_free");
#endif

/* Header flags: BinkOpen folds the file revision into OpenFlags
 * ('f'/'g' add 0x18000, 'h' adds 0x8000, 'i' adds neither). */
#define BINK_OPEN_PRE_H 0x10000ul
#define BINK_OPEN_PRE_I 0x8000ul
#define BINK_TYPE_GRAY 0x20000ul
#define BINK_TYPE_ALPHA 0x100000ul

/* Scratch bundles live in BINK.field_3ac (nine pushmalloc'd buffers). */
#define BINK_BUNDLE_COUNT 9

/* (a * b) / c with a 64-bit product; the title links no __aulldiv. */
unsigned long bink_mul_div(unsigned long a, unsigned long b, unsigned long c);

typedef unsigned int bink_u32 __attribute__((may_alias, aligned(1)));
typedef unsigned short bink_u16 __attribute__((may_alias, aligned(1)));
typedef short bink_s16 __attribute__((may_alias, aligned(1)));

#endif
