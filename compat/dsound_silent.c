/*
 * Standalone compat: DirectSound (Xbox) silent backend.
 *
 * nxdk has no DirectSound. Until a mixer on the AC97 / APU lands, every
 * object creation succeeds with an inert object, every setter succeeds
 * (the generated stubs already return DS_OK), and buffers report that they
 * are not playing, so the sound system runs its normal control flow without
 * producing audio.
 */
#include "xbox.h"

#define DS_OK 0
#define E_OUTOFMEMORY 0x8007000e
#define DSOUND_OBJECT_SIZE 0x100

void *__cdecl compat_calloc(size_t count, size_t size) __asm__("_calloc");

/* decl.h carries `void f(void)` placeholders for these kb entries; bind the
 * real __stdcall signatures to the decorated names explicitly. */
long __stdcall standalone_DirectSoundCreate(void *device_id, void **out_dsound, void *outer)
    __asm__("_DirectSoundCreate@12");
long __stdcall standalone_DirectSoundCreateStream(void *desc, void **out_stream, void *outer)
    __asm__("_DirectSoundCreateStream@12");
long __stdcall standalone_IDirectSoundBuffer_GetStatus(void *buffer, unsigned long *status)
    __asm__("_IDirectSoundBuffer_GetStatus@8");

static unsigned char dsound_device[DSOUND_OBJECT_SIZE];

/* XDK streams point at an inner object from +0x24. Halo's inlined stream
 * wrappers reach into it: dsound_stream_is_active (0x20f069) reads its
 * status dword at +0x8, FUN_0020f081 calls vtable slot 4 as a thiscall with
 * two stack arguments. The shared inert object reports no pending work;
 * other slots stay NULL so a new use faults visibly instead of popping the
 * wrong number of bytes. */
#define STREAM_INNER_OBJECT_OFFSET 0x24
#define STREAM_INNER_VTABLE_SLOTS 16
#define STREAM_INNER_FLUSH_SLOT 4

struct stream_inner_object {
  void **vtable;
  unsigned long field_04;
  unsigned long status;
};

static void __attribute__((thiscall)) stream_inner_flush(void *object, int arg0, int arg1)
{
  (void)object;
  (void)arg0;
  (void)arg1;
}

static void *stream_inner_vtable[STREAM_INNER_VTABLE_SLOTS];
static struct stream_inner_object stream_inner = {stream_inner_vtable, 0, 0};

/* The stream's own IDirectSoundStream vtable at +0. Halo calls Flush
 * (slot 6, __stdcall this-only) from sound_dsound_channel_stop_check and the
 * channel stop paths; AddRef/Release (slots 0/1) share the signature. Other
 * slots stay NULL for the same reason as the inner object's. */
#define STREAM_VTABLE_SLOTS 16
#define STREAM_ADDREF_SLOT 0
#define STREAM_RELEASE_SLOT 1
#define STREAM_GET_STATUS_SLOT 3
#define STREAM_FLUSH_SLOT 6

static unsigned long __stdcall stream_this_only(void *stream)
{
  (void)stream;
  return DS_OK;
}

/* GetStatus (slot 3, __stdcall this + DWORD*): FUN_001ca900 queues packets
 * while DSSTREAMSTATUS_READY (bit 0) is set; 0 keeps a silent stream idle. */
static unsigned long __stdcall stream_get_status(void *stream, unsigned long *status)
{
  (void)stream;
  *status = 0;
  return DS_OK;
}

static void *stream_vtable[STREAM_VTABLE_SLOTS];

static long new_object(void **out)
{
  *out = compat_calloc(1, DSOUND_OBJECT_SIZE);
  return *out ? DS_OK : (long)E_OUTOFMEMORY;
}

static long new_stream(void **out)
{
  long result;

  result = new_object(out);
  if (result == DS_OK) {
    stream_inner_vtable[STREAM_INNER_FLUSH_SLOT] = (void *)stream_inner_flush;
    stream_vtable[STREAM_ADDREF_SLOT] = (void *)stream_this_only;
    stream_vtable[STREAM_RELEASE_SLOT] = (void *)stream_this_only;
    stream_vtable[STREAM_GET_STATUS_SLOT] = (void *)stream_get_status;
    stream_vtable[STREAM_FLUSH_SLOT] = (void *)stream_this_only;
    *(void ***)*out = stream_vtable;
    *(struct stream_inner_object **)((unsigned char *)*out + STREAM_INNER_OBJECT_OFFSET) =
        &stream_inner;
  }
  return result;
}

long __stdcall standalone_DirectSoundCreate(void *device_id, void **out_dsound, void *outer)
{
  (void)device_id;
  (void)outer;
  *out_dsound = dsound_device;
  return DS_OK;
}

long __stdcall standalone_DirectSoundCreateStream(void *desc, void **out_stream, void *outer)
{
  (void)desc;
  (void)outer;
  return new_stream(out_stream);
}

long __stdcall standalone_IDirectSoundBuffer_GetStatus(void *buffer, unsigned long *status)
{
  (void)buffer;
  *status = 0;
  return DS_OK;
}

int __stdcall DirectSoundCreateBuffer(void *desc, void **out_buffer)
{
  (void)desc;
  return (int)new_object(out_buffer);
}

int __stdcall IDirectSound_CreateSoundBuffer(void *dsound, void *desc, void **buffer,
                                             void *outer)
{
  (void)dsound;
  (void)desc;
  (void)outer;
  return (int)new_object(buffer);
}

/* DSSPEAKER_STEREO; no DSSPEAKER_ENABLE_AC3 (0x10000), so channels take the
 * two-speaker mix-bin path in dsound_initialize_channel. */
#define DSSPEAKER_STEREO 0x2
#define DSCAPS_SIZE 0x10

long __stdcall standalone_IDirectSound_GetCaps(void *dsound, void *caps)
    __asm__("_IDirectSound_GetCaps@8");
long __stdcall standalone_IDirectSound_GetSpeakerConfig(void *dsound, unsigned int *config)
    __asm__("_IDirectSound_GetSpeakerConfig@8");
long __stdcall standalone_IDirectSound_CreateSoundStream(void *dsound, const void *desc,
                                                         void **out_stream, void *outer)
    __asm__("_IDirectSound_CreateSoundStream@16");

/* 0x204c5d: dsound_initialize copies the 16-byte DSCAPS into its globals. */
long __stdcall standalone_IDirectSound_GetCaps(void *dsound, void *caps)
{
  unsigned char *bytes = (unsigned char *)caps;
  int i;

  (void)dsound;
  for (i = 0; i < DSCAPS_SIZE; i++)
    bytes[i] = 0;
  return DS_OK;
}

/* 0x204c79 */
long __stdcall standalone_IDirectSound_GetSpeakerConfig(void *dsound, unsigned int *config)
{
  (void)dsound;
  *config = DSSPEAKER_STEREO;
  return DS_OK;
}

/* 0x2060f8: sound_dsound_update_channel_properties asserts channel->stream. */
long __stdcall standalone_IDirectSound_CreateSoundStream(void *dsound, const void *desc,
                                                         void **out_stream, void *outer)
{
  (void)dsound;
  (void)desc;
  (void)outer;
  return new_stream(out_stream);
}
