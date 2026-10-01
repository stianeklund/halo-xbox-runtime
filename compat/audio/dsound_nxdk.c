/* Xbox DirectSound ABI adapter over the nxdk AC97 stereo transport.
 * PCM and Xbox IMA ADPCM, software volume/pitch, streams and static buffers.
 * Packet callbacks run only from DirectSoundDoWork on the game thread.
 * 3D sources get distance, cone, occlusion and stereo pan gains; HRTF,
 * Doppler, reverb, DSP effects and surround mix bins remain unsupported. */
#include "xbox.h"
#include "mixer.h"
#include "backend.h"
#include "pcm_stream.h"

#define DS_OK 0
#define DS_INVALID ((int)0x80070057)
#define DS_NOMEM ((int)0x8007000e)
#define DS_UNSUPPORTED ((int)0x80004001)
#define DS_FAILED ((int)0x80004005)
#define DS_CTRL3D 0x10
#define DS3DMODE_HEADRELATIVE 1
#define DS3DMODE_DISABLE 2

void *__cdecl audio_calloc(size_t count, size_t size) __asm__("_calloc");
void __cdecl audio_free(void *memory) __asm__("_free");
int __cdecl audio_signal_event(void *event) __asm__("_SetEvent");

typedef void (__stdcall *packet_callback)(void *, void *, unsigned int);
#pragma pack(push, 4)
struct media_packet {
  const unsigned char *data;
  unsigned int size;
  unsigned int *completed_size, *status;
  void *context;
  void *timestamp;
};
struct stream_desc {
  unsigned int flags, max_packets;
  const struct audio_format *format;
  packet_callback callback;
  void *context;
  unsigned int mix_bins;
};
struct buffer_desc {
  unsigned int size, flags, bytes;
  const struct audio_format *format;
  unsigned int mix_bins;
  void *input_mix_bin;
};
/* DirectSound 3D state for one stream. Levels are hundredths of a dB. */
struct spatial {
  float position[3], cone[3];
  float min_distance, max_distance;
  unsigned int inside_angle, outside_angle;
  int cone_volume, direct, mode;
};
struct sound_object;
struct sound_inner {
  void **vtable;
  unsigned int reserved;
  volatile unsigned int status;
  struct sound_object *owner;
};
struct sound_object {
  void **vtable;
  unsigned int refs;
  unsigned char reserved[0x1c];
  struct sound_inner *inner; /* +0x24: used by Halo's inlined XDK wrappers */
  struct sound_inner inner_storage;
  struct audio_voice voice;
  packet_callback callback;
  void *context;
  struct sound_object *next;
  int is_stream;
  unsigned int max_packets;
  int volume, bin_volume[2], is_3d;
  unsigned int mix_bins;
  struct spatial spatial;
};
#pragma pack(pop)
typedef char stream_inner_offset_check[__builtin_offsetof(struct sound_object, inner) == 0x24 ? 1 : -1];
typedef char media_packet_size_check[sizeof(struct media_packet) == 0x18 ? 1 : -1];
typedef char stream_desc_size_check[sizeof(struct stream_desc) == 0x18 ? 1 : -1];

static struct sound_object *audio_objects;
static void *stream_vtable[16], *inner_vtable[16];
static unsigned int device_refs;
static int audio_started;
static int device;
static int mixed[1024 * 2];
/* Submitted, rejected, completed, aborted, render calls, nonzero frames,
 * peak sample magnitude, game packet-queue calls. Updated under the lock
 * except for the game-thread queue-call counter. */
unsigned int halo_audio_mixer_stats[8];

/* Integer exponents avoid pulling pdclib's math dependency closure into the
 * game's CRT. Each base represents one DirectSound volume/pitch unit. */
static double unit_power(double base, unsigned int exponent)
{
  double result = 1.0;
  while (exponent) {
    if (exponent & 1) result *= base;
    base *= base;
    exponent >>= 1;
  }
  return result;
}

static void update_status(struct sound_object *object)
{
  object->inner_storage.status = audio_voice_active(&object->voice) ? 2 : 0;
}

/* Caller holds the backend lock; header formats may be packed/unaligned. */
static int copy_format(struct audio_format *dst, const struct audio_format *src)
{
  if (!src) return 0;
  dst->tag = src->tag;
  dst->channels = src->channels;
  dst->rate = src->rate;
  dst->bytes_per_second = src->bytes_per_second;
  dst->block_align = src->block_align;
  dst->bits = src->bits;
  /* Halo's PCM WAVEFORMAT has an uninitialized cbSize. */
  dst->extra_size = src->tag == 0x69 ? src->extra_size : 0;
  dst->samples_per_block = src->tag == 0x69 && dst->extra_size >= 2 ? src->samples_per_block : 0;
  return audio_format_valid(dst);
}

static unsigned int __stdcall object_addref(void *stream)
{
  struct sound_object *o = stream;
  unsigned int refs;
  standalone_audio_lock();
  refs = ++o->refs;
  standalone_audio_unlock();
  return refs;
}

static unsigned int __stdcall object_release(void *stream)
{
  struct sound_object *o = stream;
  unsigned int refs;
  standalone_audio_lock();
  refs = o->refs ? --o->refs : 0;
  /* Keep ownership if the hardware has not stopped; its DMA may still read
   * the cache data. Never free those packets on a stop timeout. */
  if (!refs && !audio_output_flush(&o->voice)) refs = o->refs = 1;
  update_status(o);
  standalone_audio_unlock();
  /* Halo resets the channel count immediately after releasing its streams.
   * Dispatch aborts now, while callbacks can still access those channels. */
  if (!refs) DirectSoundDoWork();
  return refs;
}

static int __stdcall stream_status(void *stream, unsigned int *status)
{
  struct sound_object *o = stream;
  unsigned int i, count = 0;
  if (!status) return DS_INVALID;
  standalone_audio_lock();
  for (i = 0; i < AUDIO_PACKET_COUNT; i++) if (o->voice.packets[i].state) count++;
  *status = (count < o->max_packets && audio_output_ready(&o->voice) ? 1 : 0) |
            (audio_voice_active(&o->voice) ? 0x10000 : 0) | (o->voice.paused ? 0x20000 : 0);
  standalone_audio_unlock();
  return DS_OK;
}

static int __stdcall stream_process(void *stream, const struct media_packet *input,
                                    const struct media_packet *output)
{
  struct sound_object *o = stream;
  struct audio_packet packet;
  unsigned int i, count = 0;
  int accepted;
  if (!input || output) return DS_INVALID;
  csmemset(&packet, 0, sizeof(packet));
  packet.data = input->data;
  packet.size = input->size;
  packet.completed_size = input->completed_size;
  packet.status = input->status;
  packet.context = input->context;
  standalone_audio_lock();
  for (i = 0; i < AUDIO_PACKET_COUNT; i++) if (o->voice.packets[i].state) count++;
  accepted = o->refs && count < o->max_packets && audio_output_submit(&o->voice, &packet);
  halo_audio_mixer_stats[accepted ? 0 : 1]++;
  update_status(o);
  standalone_audio_unlock();
  return accepted ? DS_OK : DS_INVALID;
}

static int __stdcall stream_flush(void *stream)
{
  struct sound_object *o = stream;
  int stopped;
  standalone_audio_lock();
  stopped = audio_output_flush(&o->voice);
  update_status(o);
  standalone_audio_unlock();
  /* Halo can reuse an idle channel immediately after Flush, resetting its
   * pending count. Finish abort callbacks before returning so they cannot
   * decrement the next sound's count or retain its predecessor's cache refs.
   * Dispatch outside the lock: callbacks may reenter the audio adapter. */
  if (stopped) DirectSoundDoWork();
  return stopped ? DS_OK : DS_FAILED;
}

static void __attribute__((thiscall)) inner_flush(void *inner, int arg0, int arg1)
{
  (void)arg0;
  (void)arg1;
  stream_flush(((struct sound_inner *)inner)->owner);
}

static int __stdcall stream_info(void *stream, unsigned int *info)
{
  struct sound_object *o = stream;
  if (!info) return DS_INVALID;
  info[0] = 7; /* fixed sample size/alignment, asynchronous input */
  info[1] = o->voice.format.block_align;
  info[2] = 0;
  info[3] = 0;
  return DS_OK;
}

static int __stdcall stream_discontinuity(void *stream)
{
  (void)stream;
  /* There is no decoder history across Xbox ADPCM blocks to drain. */
  return DS_OK;
}

static int create_object(const struct audio_format *format, int is_stream,
                         unsigned int flags, struct sound_object **out)
{
  struct audio_format copy;
  struct sound_object *o;
  if (!out) return DS_INVALID;
  *out = NULL;
  if (!copy_format(&copy, format)) return DS_INVALID;
  if (!standalone_audio_init()) return DS_NOMEM;
  audio_started = 1;
  o = audio_calloc(1, sizeof(*o));
  if (!o) return DS_NOMEM;
  audio_voice_init(&o->voice, &copy);
  o->refs = 1;
  o->is_stream = is_stream;
  o->inner = &o->inner_storage;
  o->inner_storage.owner = o;
  o->inner_storage.vtable = inner_vtable;
  o->vtable = stream_vtable;
  o->max_packets = AUDIO_PACKET_COUNT;
  o->mix_bins = 3;
  o->is_3d = (flags & DS_CTRL3D) != 0;
  /* cachebeta's voice-settings constructor (0x2054a8) defaults to
   * 600 mB headroom for 2D voices and zero for DS_CTRL3D voices. */
  if (!o->is_3d) o->voice.gain_left = o->voice.gain_right = 16422;
  o->spatial.min_distance = 1.0f;
  o->spatial.max_distance = 1e9f;
  o->spatial.inside_angle = o->spatial.outside_angle = 360;
  o->spatial.cone[2] = 1.0f;
  standalone_audio_lock();
  if (!audio_output_create(&o->voice, is_stream)) {
    standalone_audio_unlock();
    audio_free(o);
    return DS_NOMEM;
  }
  inner_vtable[4] = (void *)inner_flush;
  stream_vtable[0] = (void *)object_addref;
  stream_vtable[1] = (void *)object_release;
  stream_vtable[2] = (void *)stream_info;
  stream_vtable[3] = (void *)stream_status;
  stream_vtable[4] = (void *)stream_process;
  stream_vtable[5] = (void *)stream_discontinuity;
  stream_vtable[6] = (void *)stream_flush;
  o->next = audio_objects;
  audio_objects = o;
  standalone_audio_unlock();
  *out = o;
  return DS_OK;
}

/* Hardware completion only updates metadata here; game callbacks stay in
 * DirectSoundDoWork. Caller holds the audio lock. */
void standalone_audio_poll(void)
{
  struct sound_object *o;
  for (o = audio_objects; o; o = o->next) {
    audio_output_poll(&o->voice);
    update_status(o);
  }
}

/* Called under the lock by the transport's worker, never from a DPC. */
void standalone_audio_render(short *output, unsigned int frames)
{
  struct sound_object *o;
  unsigned int i, peak;
  if (frames > 1024) return;
  csmemset(mixed, 0, frames * 2 * sizeof(int));
  for (o = audio_objects; o; o = o->next) {
    if (o->refs) audio_voice_mix(&o->voice, mixed, frames);
    update_status(o);
  }
  audio_mix_clip(mixed, output, frames);
  halo_audio_mixer_stats[4]++;
  for (i = 0; i < frames; i++) {
    if (output[i * 2] || output[i * 2 + 1]) halo_audio_mixer_stats[5]++;
    peak = (unsigned int)(output[i * 2] < 0 ? -output[i * 2] : output[i * 2]);
    if (peak > halo_audio_mixer_stats[6]) halo_audio_mixer_stats[6] = peak;
    peak = (unsigned int)(output[i * 2 + 1] < 0 ? -output[i * 2 + 1] : output[i * 2 + 1]);
    if (peak > halo_audio_mixer_stats[6]) halo_audio_mixer_stats[6] = peak;
  }
}

void DirectSoundDoWork(void)
{
  struct sound_object *o, **link;
  struct audio_packet packet;
  packet_callback callback;
  void *context;
  unsigned int budget = 1024;
  if (!audio_started) return;
  standalone_audio_lock();
  standalone_audio_poll();
  standalone_audio_unlock();
  /* One completion per lock acquisition. Callbacks can submit, flush, or
   * release objects. Never retain a list iterator across a game callback. */
  while (budget--) {
    standalone_audio_lock();
    for (o = audio_objects; o; o = o->next)
      if (audio_voice_completion(&o->voice, &packet)) break;
    if (!o) {
      for (link = &audio_objects; *link;) {
        o = *link;
        if (!o->refs) { *link = o->next; audio_output_destroy(&o->voice); audio_free(o); }
        else link = &o->next;
      }
      standalone_audio_unlock();
      return;
    }
    callback = o->callback;
    context = o->context;
    halo_audio_mixer_stats[packet.result ? 3 : 2]++;
    standalone_audio_unlock();
    if (callback) callback(context, packet.context, packet.result);
    else if (packet.context) audio_signal_event(packet.context);
  }
}

/* Real decorated exports also cover kb.json's void(void) placeholders. */
int __stdcall nxds_create(void *guid, void **out, void *outer) __asm__("_DirectSoundCreate@12");
int __stdcall nxds_create(void *guid, void **out, void *outer)
{
  (void)guid;
  if (!out || outer) return DS_INVALID;
  *out = NULL;
  if (!standalone_audio_init()) return DS_NOMEM;
  audio_started = 1;
  device_refs++;
  *out = &device;
  return DS_OK;
}

unsigned int __stdcall nxds_release(void *ds) __asm__("_IDirectSound_Release@4");
unsigned int __stdcall nxds_release(void *ds)
{
  (void)ds;
  if (device_refs) device_refs--;
  DirectSoundDoWork();
  /* The process owns the HAL worker until title exit; it outputs silence
   * after all voices have been released and can be reused on reinitialization. */
  return device_refs;
}

int __stdcall nxds_stream(void *ds, const struct stream_desc *desc, void **out, void *outer)
    __asm__("_IDirectSound_CreateSoundStream@16");
int __stdcall nxds_stream(void *ds, const struct stream_desc *desc, void **out, void *outer)
{
  struct sound_object *o;
  int result;
  (void)ds;
  if (!out) return DS_INVALID;
  *out = NULL;
  if (!desc || outer || !desc->max_packets || desc->max_packets > AUDIO_PACKET_COUNT) return DS_INVALID;
  result = create_object(desc->format, 1, desc->flags, &o);
  if (result) return result;
  standalone_audio_lock();
  o->callback = desc->callback;
  o->context = desc->context;
  o->max_packets = desc->max_packets;
  standalone_audio_unlock();
  *out = o;
  return DS_OK;
}

int __stdcall nxds_create_stream(const struct stream_desc *desc, void **out, void *outer)
    __asm__("_DirectSoundCreateStream@12");
int __stdcall nxds_create_stream(const struct stream_desc *desc, void **out, void *outer)
{
  return nxds_stream(&device, desc, out, outer);
}

int __stdcall IDirectSound_CreateSoundBuffer(void *ds, void *desc_ptr, void **out, void *outer)
{
  const struct buffer_desc *desc = desc_ptr;
  struct sound_object *o;
  int result;
  (void)ds;
  if (!out) return DS_INVALID;
  *out = NULL;
  if (!desc || desc->size != sizeof(*desc) || outer) return DS_INVALID;
  result = create_object(desc->format, 0, desc->flags, &o);
  if (!result) *out = o;
  return result;
}

int __stdcall DirectSoundCreateBuffer(void *desc, void **out)
{
  return IDirectSound_CreateSoundBuffer(&device, desc, out, NULL);
}

int __stdcall nxds_caps(void *ds, void *caps) __asm__("_IDirectSound_GetCaps@8");
int __stdcall nxds_caps(void *ds, void *caps)
{
  (void)ds;
  if (!caps) return DS_INVALID;
  csmemset(caps, 0, 0x10);
  return DS_OK;
}

int __stdcall nxds_speakers(void *ds, unsigned int *config) __asm__("_IDirectSound_GetSpeakerConfig@8");
int __stdcall nxds_speakers(void *ds, unsigned int *config)
{
  (void)ds;
  if (!config) return DS_INVALID;
  *config = 2;
  return DS_OK;
}

/* Listener state, already in DirectSound's Y-up axes (Halo swaps Y/Z). */
static float listener_position[3], listener_front[3] = {0, 0, 1}, listener_top[3] = {0, 1, 0};
static float listener_rolloff = 1.0f;

/* x87 instructions keep pdclib's math closure out, as unit_power does. */
static double root(double x)
{
  __asm__("fsqrt" : "+t"(x));
  return x;
}

static double cosine(double x)
{
  __asm__("fcos" : "+t"(x));
  return x;
}

static double level_gain(int level)
{
  if (level <= -10000) return 0.0;
  return level >= 0 ? 1.0 : unit_power(0.9988493699365052, (unsigned int)-level);
}

/* Cone level at the listener, interpolated on the cosine of the angle
 * between the cone orientation and the source-to-listener direction. */
static int cone_level(const struct spatial *s, const double *to_listener)
{
  double length = root((double)s->cone[0] * s->cone[0] + (double)s->cone[1] * s->cone[1] +
                       (double)s->cone[2] * s->cone[2]), c, inside, outside;
  if (s->inside_angle >= 360 && s->outside_angle >= 360) return 0;
  if (length <= 0.0) return 0;
  c = (to_listener[0] * s->cone[0] + to_listener[1] * s->cone[1] + to_listener[2] * s->cone[2]) / length;
  inside = cosine(s->inside_angle * 3.14159265358979 / 360.0);
  outside = cosine(s->outside_angle * 3.14159265358979 / 360.0);
  if (c >= inside) return 0;
  if (c <= outside || inside <= outside) return s->cone_volume;
  return (int)(s->cone_volume * (inside - c) / (inside - outside));
}

/* Combine volume, mix-bin levels and the 3D model into the voice's L/R
 * gains. Distance uses DirectSound's inverse rolloff, clamped at max
 * distance. The I3DL2 direct path applies occlusion/obstruction at their
 * low-frequency ratio; their high-frequency filtering is not modelled.
 * Panning approximates HRTF head shadow: the far ear keeps 30% at 90 deg.
 * Setters accept out-of-range values as the stubs did; this clamps them.
 * Caller holds the audio lock. */
static int apply_gain(struct sound_object *o)
{
  const struct spatial *s = &o->spatial;
  float min_distance = s->min_distance > 0.0f ? s->min_distance : 0.0f;
  double gain = level_gain(o->volume), left, right;
  double offset[3], distance, clamped, pan, right_axis[3], axis_length;
  int i;
  if (!o->is_3d) gain *= level_gain(-600);
  left = o->mix_bins & 1 ? level_gain(o->bin_volume[0]) : 0.0;
  right = o->mix_bins & 2 ? level_gain(o->bin_volume[1]) : 0.0;
  if (o->is_3d) {
    gain *= level_gain(s->direct);
    if (s->mode != DS3DMODE_DISABLE) {
      for (i = 0; i < 3; i++)
        offset[i] = s->position[i] - (s->mode == DS3DMODE_HEADRELATIVE ? 0.0f : listener_position[i]);
      distance = root(offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
      clamped = distance < s->max_distance ? distance : s->max_distance;
      if (clamped > min_distance && min_distance > 0.0f)
        gain *= min_distance / (min_distance + listener_rolloff * (clamped - min_distance));
      if (distance > 0.0) {
        double to_listener[3];
        for (i = 0; i < 3; i++) to_listener[i] = -offset[i] / distance;
        gain *= level_gain(cone_level(s, to_listener));
        if (s->mode == DS3DMODE_HEADRELATIVE) {
          right_axis[0] = 1.0; right_axis[1] = right_axis[2] = 0.0;
        } else {
          right_axis[0] = (double)listener_top[1] * listener_front[2] - (double)listener_top[2] * listener_front[1];
          right_axis[1] = (double)listener_top[2] * listener_front[0] - (double)listener_top[0] * listener_front[2];
          right_axis[2] = (double)listener_top[0] * listener_front[1] - (double)listener_top[1] * listener_front[0];
        }
        axis_length = root(right_axis[0] * right_axis[0] + right_axis[1] * right_axis[1] + right_axis[2] * right_axis[2]);
        if (axis_length > 0.0) {
          pan = (offset[0] * right_axis[0] + offset[1] * right_axis[1] + offset[2] * right_axis[2]) /
                (distance * axis_length);
          if (pan > 0.0) left *= 1.0 - 0.7 * pan;
          else right *= 1.0 + 0.7 * pan;
        }
      }
    }
  }
  o->voice.gain_left = (int)(32768.0 * gain * left);
  o->voice.gain_right = (int)(32768.0 * gain * right);
  return audio_output_volume(&o->voice);
}

/* DS3D_IMMEDIATE is 0. Deferred changes wait for CommitDeferredSettings,
 * which Halo calls once per frame. */
static int stream_changed(struct sound_object *o, int apply)
{
  int result = apply ? 1 : apply_gain(o);
  standalone_audio_unlock();
  return result ? DS_OK : DS_FAILED;
}

static int apply_all(void)
{
  struct sound_object *o;
  int result = 1;
  for (o = audio_objects; o; o = o->next)
    if (o->refs && !apply_gain(o)) result = 0;
  return result;
}

int __stdcall IDirectSoundStream_SetVolume(void *stream, int volume)
{
  struct sound_object *o = stream;
  if (volume < -10000 || volume > 0) return DS_INVALID;
  standalone_audio_lock();
  o->volume = volume;
  return stream_changed(o, 0);
}

int __stdcall IDirectSoundStream_SetMixBins(void *stream, unsigned int mix_bins)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->mix_bins = mix_bins;
  return stream_changed(o, 0);
}

/* Early XDK form: one volume per set bit of the mask, lowest bin first.
 * Only the front-left and front-right bins reach the stereo output. */
int __stdcall IDirectSoundStream_SetMixBinVolumes_12(void *stream, unsigned int mix_bins,
                                                    const int *volumes)
{
  struct sound_object *o = stream;
  unsigned int bin, index = 0;
  if (!volumes) return DS_INVALID;
  standalone_audio_lock();
  for (bin = 0; bin < 32; bin++) {
    if (!(mix_bins & (1u << bin))) continue;
    if (bin < 2) o->bin_volume[bin] = volumes[index];
    index++;
  }
  return stream_changed(o, 0);
}

/* The XDK export Halo calls here takes a DSI3DL2BUFFER: direct, direct HF,
 * room, room HF, room rolloff, obstruction {HF, LF ratio}, occlusion {HF,
 * LF ratio}. Room/reverb sends have no output path yet. */
int __stdcall IDirectSoundStream_SetAllParameters(void *stream, void *params,
                                                 int apply)
{
  struct sound_object *o = stream;
  const int *levels = params;
  const float *ratios = params;
  int direct;
  if (!params) return DS_INVALID;
  direct = levels[0] + (int)(levels[5] * ratios[6]) + (int)(levels[7] * ratios[8]);
  standalone_audio_lock();
  o->spatial.direct = direct < -10000 ? -10000 : direct > 0 ? 0 : direct;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetMode(void *stream, int mode, int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.mode = mode;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetPosition(void *stream, float x, float y, float z,
                                            int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.position[0] = x;
  o->spatial.position[1] = y;
  o->spatial.position[2] = z;
  return stream_changed(o, apply);
}

/* Doppler is not modelled. */
int __stdcall IDirectSoundStream_SetVelocity(void *stream, float x, float y, float z,
                                            int apply)
{
  (void)stream;
  (void)x;
  (void)y;
  (void)z;
  (void)apply;
  return DS_OK;
}

int __stdcall IDirectSoundStream_SetConeOrientation(void *stream, float x, float y, float z,
                                                   int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.cone[0] = x;
  o->spatial.cone[1] = y;
  o->spatial.cone[2] = z;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetMinDistance(void *stream, float distance, int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.min_distance = distance;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetMaxDistance(void *stream, float distance, int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.max_distance = distance;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetConeAngles(void *stream, int inside, int outside, int apply)
{
  struct sound_object *o = stream;
  if (outside < 0 || outside > 360) outside = 360;
  if (inside < 0 || inside > outside) inside = outside;
  standalone_audio_lock();
  o->spatial.inside_angle = (unsigned int)inside;
  o->spatial.outside_angle = (unsigned int)outside;
  return stream_changed(o, apply);
}

int __stdcall IDirectSoundStream_SetConeOutsideVolume(void *stream, int volume, int apply)
{
  struct sound_object *o = stream;
  standalone_audio_lock();
  o->spatial.cone_volume = volume < -10000 ? -10000 : volume > 0 ? 0 : volume;
  return stream_changed(o, apply);
}

static int listener_changed(uint32_t apply)
{
  int result = apply ? 1 : apply_all();
  standalone_audio_unlock();
  return result ? DS_OK : DS_FAILED;
}

int __stdcall IDirectSound_SetPosition(void *ds, float x, float y, float z, unsigned int apply)
{
  (void)ds;
  standalone_audio_lock();
  listener_position[0] = x;
  listener_position[1] = y;
  listener_position[2] = z;
  return listener_changed(apply);
}

int __stdcall IDirectSound_SetOrientation(void *ds, float front_x, float front_y, float front_z,
                                         float top_x, float top_y, float top_z, unsigned int apply)
{
  (void)ds;
  standalone_audio_lock();
  listener_front[0] = front_x;
  listener_front[1] = front_y;
  listener_front[2] = front_z;
  listener_top[0] = top_x;
  listener_top[1] = top_y;
  listener_top[2] = top_z;
  return listener_changed(apply);
}

int __stdcall IDirectSound_SetRolloffFactor(void *ds, float rolloff, unsigned int apply)
{
  (void)ds;
  standalone_audio_lock();
  listener_rolloff = rolloff >= 0.0f ? rolloff : 0.0f;
  return listener_changed(apply);
}

int __stdcall IDirectSound_CommitDeferredSettings(void *ds)
{
  (void)ds;
  standalone_audio_lock();
  return listener_changed(0);
}

int __stdcall IDirectSoundStream_SetFrequency(void *stream, int frequency)
{
  struct sound_object *o = stream;
  int valid;
  standalone_audio_lock();
  valid = audio_voice_frequency(&o->voice, frequency ? (unsigned int)frequency : o->voice.format.rate);
  if (valid) valid = audio_output_frequency(&o->voice, frequency ? (unsigned int)frequency : o->voice.format.rate);
  standalone_audio_unlock();
  return valid ? DS_OK : DS_INVALID;
}

int __stdcall nxds_pause(void *stream, unsigned int pause) __asm__("_IDirectSoundStream_Pause@8");
int __stdcall nxds_pause(void *stream, unsigned int pause)
{
  struct sound_object *o = stream;
  int result;
  if (pause > 1) return DS_UNSUPPORTED;
  standalone_audio_lock();
  result = audio_output_pause(&o->voice, (int)pause);
  if (result) o->voice.paused = (int)pause;
  standalone_audio_unlock();
  return result ? DS_OK : DS_FAILED;
}

int __stdcall nxds_buffer_status(void *buffer, unsigned int *status) __asm__("_IDirectSoundBuffer_GetStatus@8");
int __stdcall nxds_buffer_status(void *buffer, unsigned int *status)
{
  struct sound_object *o = buffer;
  if (!status) return DS_INVALID;
  standalone_audio_lock();
  audio_output_poll(&o->voice);
  update_status(o);
  *status = o->voice.playing ? (1 | (o->voice.looping ? 4 : 0)) : 0;
  standalone_audio_unlock();
  return DS_OK;
}

int __stdcall IDirectSoundBuffer_SetBufferData(void *buffer, void *data, uint32_t size)
{
  struct sound_object *o = buffer;
  if (size && !audio_data_valid(&o->voice.format, data, size)) return DS_INVALID;
  standalone_audio_lock();
  if (!audio_output_flush(&o->voice)) { standalone_audio_unlock(); return DS_FAILED; }
  o->voice.playing = 0;
  o->voice.buffer = data;
  o->voice.buffer_size = size;
  o->voice.buffer_cursor = o->voice.loop_start = o->voice.loop_end = 0;
  o->voice.phase = 0;
  o->voice.decoded_block = ~0u;
  standalone_audio_unlock();
  return DS_OK;
}

int __stdcall IDirectSoundBuffer_Play(void *buffer, uint32_t reserved, uint32_t priority, uint32_t flags)
{
  struct sound_object *o = buffer;
  (void)reserved;
  (void)priority;
  if (flags & ~1u) return DS_UNSUPPORTED;
  standalone_audio_lock();
  if (!o->voice.buffer || !o->voice.buffer_size) { standalone_audio_unlock(); return DS_INVALID; }
  o->voice.looping = flags & 1;
  o->voice.playing = 1;
  if (!audio_output_buffer_play(&o->voice)) {
    o->voice.playing = 0;
    standalone_audio_unlock();
    return DS_FAILED;
  }
  standalone_audio_unlock();
  return DS_OK;
}

int __stdcall IDirectSoundBuffer_Stop(void *buffer) { return stream_flush(buffer); }
int __stdcall IDirectSoundBuffer_Release(void *buffer) { return (int)object_release(buffer); }
int __stdcall IDirectSoundBuffer_SetVolume(void *buffer, int volume) { return IDirectSoundStream_SetVolume(buffer, volume); }
/* XDK pitch is absolute, the APU's voice encoding 4096 * log2(f / 48000),
 * not relative to the buffer's rate: Halo pitches its 22.05 kHz loading-bar
 * sounds near -4597. The range spans DSBFREQUENCY_MIN..MAX (188..191999). */
#define DSBPITCH_MIN (-32768)
#define DSBPITCH_MAX 8191
int __stdcall IDirectSoundBuffer_SetPitch(void *buffer, int pitch)
{
  if (pitch < DSBPITCH_MIN || pitch > DSBPITCH_MAX) return DS_INVALID;
  return IDirectSoundStream_SetFrequency(buffer, (int)(AUDIO_RATE *
    unit_power(pitch < 0 ? 0.9998307889319291 : 1.000169239705302, (unsigned int)(pitch < 0 ? -pitch : pitch)) + 0.5));
}

int __stdcall IDirectSoundBuffer_SetCurrentPosition(void *buffer, uint32_t position)
{
  struct sound_object *o = buffer;
  standalone_audio_lock();
  if (position > o->voice.buffer_size || position % o->voice.format.block_align) {
    standalone_audio_unlock(); return DS_INVALID;
  }
  o->voice.buffer_cursor = position / o->voice.format.block_align * (o->voice.format.tag == 0x69 ? 64 : 1);
  o->voice.phase = 0;
  o->voice.decoded_block = ~0u;
  if (!audio_output_buffer_seek(&o->voice, position)) { standalone_audio_unlock(); return DS_FAILED; }
  standalone_audio_unlock();
  return DS_OK;
}

int __stdcall IDirectSoundBuffer_SetLoopRegion(void *buffer, uint32_t start, uint32_t length)
{
  struct sound_object *o = buffer;
  unsigned int scale = o->voice.format.tag == 0x69 ? 64 : 1;
  standalone_audio_lock();
  if (start > o->voice.buffer_size || length > o->voice.buffer_size - start ||
      start % o->voice.format.block_align || length % o->voice.format.block_align) {
    standalone_audio_unlock(); return DS_INVALID;
  }
  o->voice.loop_start = start / o->voice.format.block_align * scale;
  o->voice.loop_end = length ? (start + length) / o->voice.format.block_align * scale : 0;
  if (!audio_output_buffer_loop(&o->voice)) { standalone_audio_unlock(); return DS_FAILED; }
  standalone_audio_unlock();
  return DS_OK;
}

/* Game-side packet splitting for the missing 2276 routine at 0x1c9670.
 * Keep HALO_ROOT a read-only dependency. */
bool dsound_channel_queue_packet(short channel_index)
{
  unsigned char *channel = sound_dsound_channel_get(channel_index);
  unsigned char *permutation = *(unsigned char **)(channel + 0x68);
  struct sound_object *stream = *(struct sound_object **)(channel + 0x70);
  struct media_packet packet;
  unsigned int size, offset, chunk, align, total, split;
  unsigned short flags = *(unsigned short *)(channel + 0x38);
  const unsigned char *data;
  int result;
  halo_audio_mixer_stats[7]++;
  if (!permutation || !stream) return false;
  data = *(const unsigned char **)(permutation + 0x30);
  total = *(unsigned int *)(permutation + 0x40);
  offset = *(unsigned int *)(channel + 0x64);
  if (!data || offset >= total) return false;
  /* Match the original cache-range check without overflowing an end pointer. */
  if ((unsigned int)data < (unsigned int)FUN_001bdd70() ||
      (unsigned int)data - (unsigned int)FUN_001bdd70() > 0x400000 ||
      total > 0x400000 - ((unsigned int)data - (unsigned int)FUN_001bdd70())) return false;
  size = total - offset;
  align = stream->voice.format.block_align;
  chunk = (flags & 8 ? 0x900 : 0x2000) * (flags & 2 ? 2 : 1) * (flags & 4 ? 2 : 1);
  if (size <= chunk && offset == 0) {
    split = (size / align) / 2;
    chunk = (split ? split : 1) * align;
  } else if (size < chunk) chunk = size;
  if (!chunk || chunk % align) return false;
  csmemset(&packet, 0, sizeof(packet));
  packet.data = data + offset;
  packet.size = chunk;
  packet.context = permutation;
  *(short *)(channel + 8) += 1;
  result = stream_process(stream, &packet, NULL);
  if (result) {
    *(short *)(channel + 8) -= 1;
    sound_dsound_set_last_error(&result, "couldn't queue sound packet.");
    return false;
  }
  FUN_001be100((int)permutation);
  if (offset + chunk == total) {
    *(void **)(channel + 0x68) = *(void **)(channel + 0x6c);
    *(void **)(channel + 0x6c) = NULL;
    *(unsigned int *)(channel + 0x64) = 0;
    if (*(short *)channel == 2) *(short *)channel = 1;
  } else *(unsigned int *)(channel + 0x64) = offset + chunk;
  return true;
}

/* PCM streams for Bink movie audio: an ordinary callback-less stream whose
 * completions its owner retires, so no game callback runs off-thread. */
struct pcm_stream *pcm_stream_open(unsigned int rate, unsigned int channels, unsigned int bits)
{
  struct audio_format format;
  struct sound_object *o;
  csmemset(&format, 0, sizeof(format));
  format.tag = 1;
  format.channels = (unsigned short)channels;
  format.rate = rate;
  format.bits = (unsigned short)bits;
  format.block_align = (unsigned short)(channels * bits / 8);
  format.bytes_per_second = rate * format.block_align;
  if (create_object(&format, 1, 0, &o)) return NULL;
  standalone_audio_lock();
  if (audio_output_pause(&o->voice, 1)) o->voice.paused = 1;
  standalone_audio_unlock();
  return (struct pcm_stream *)o;
}

int pcm_stream_submit(struct pcm_stream *stream, const void *data, unsigned int size,
                      volatile unsigned int *status)
{
  struct media_packet packet;
  csmemset(&packet, 0, sizeof(packet));
  packet.data = data;
  packet.size = size;
  packet.status = (unsigned int *)status;
  return stream_process(stream, &packet, NULL) == DS_OK;
}

void pcm_stream_retire(struct pcm_stream *stream)
{
  struct sound_object *o = (struct sound_object *)stream;
  struct audio_packet packet;
  standalone_audio_lock();
  audio_output_poll(&o->voice);
  while (audio_voice_completion(&o->voice, &packet)) {}
  update_status(o);
  standalone_audio_unlock();
}

int pcm_stream_active(struct pcm_stream *stream)
{
  struct sound_object *o = (struct sound_object *)stream;
  int active;
  standalone_audio_lock();
  active = audio_voice_active(&o->voice);
  standalone_audio_unlock();
  return active;
}

int pcm_stream_pause(struct pcm_stream *stream, int pause)
{
  return nxds_pause(stream, pause ? 1 : 0) == DS_OK;
}

void pcm_stream_volume(struct pcm_stream *stream, int volume)
{
  IDirectSoundStream_SetVolume(stream, volume);
}

int pcm_stream_flush(struct pcm_stream *stream)
{
  struct sound_object *o = (struct sound_object *)stream;
  int stopped;
  standalone_audio_lock();
  stopped = audio_output_flush(&o->voice);
  update_status(o);
  standalone_audio_unlock();
  if (stopped) pcm_stream_retire(stream);
  return stopped;
}

int pcm_stream_close(struct pcm_stream *stream)
{
  /* Release keeps the object, and so its packet memory, if DMA may still
   * read it; otherwise DirectSoundDoWork unlinks and frees it. */
  return object_release(stream) == 0;
}
