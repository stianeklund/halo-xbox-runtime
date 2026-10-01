/* Shared metadata remains in audio_voice. Backend operations run under the
 * title's audio lock, on threads that may use the FPU, never from a DPC. */
#ifndef HALO_AUDIO_BACKEND_H
#define HALO_AUDIO_BACKEND_H
#include "mixer.h"
#ifdef HALO_AUDIO_APU
int audio_output_create(struct audio_voice *v, int stream);
void audio_output_destroy(struct audio_voice *v);
int audio_output_submit(struct audio_voice *v, const struct audio_packet *packet);
int audio_output_flush(struct audio_voice *v);
/* Like flush, but a stream may keep playing its next packet on another voice. */
int audio_output_flush_async(struct audio_voice *v);
int audio_output_ready(const struct audio_voice *v);
void audio_output_poll(struct audio_voice *v);
int audio_output_volume(struct audio_voice *v);
int audio_output_frequency(struct audio_voice *v, unsigned int frequency);
int audio_output_pause(struct audio_voice *v, int pause);
int audio_output_buffer_play(struct audio_voice *v);
int audio_output_buffer_seek(struct audio_voice *v, unsigned int position);
int audio_output_buffer_loop(struct audio_voice *v);
int audio_output_mixbin_headroom(unsigned int mix_bins, unsigned int headroom);
#else
static inline int audio_output_create(struct audio_voice *v, int stream) { (void)v; (void)stream; return 1; }
static inline void audio_output_destroy(struct audio_voice *v) { (void)v; }
static inline int audio_output_submit(struct audio_voice *v, const struct audio_packet *p) { return audio_voice_submit(v, p); }
static inline int audio_output_flush(struct audio_voice *v) { audio_voice_flush(v); return 1; }
static inline int audio_output_flush_async(struct audio_voice *v) { return audio_output_flush(v); }
static inline int audio_output_ready(const struct audio_voice *v) { return audio_voice_ready(v); }
static inline void audio_output_poll(struct audio_voice *v) { (void)v; }
static inline int audio_output_volume(struct audio_voice *v) { (void)v; return 1; }
static inline int audio_output_frequency(struct audio_voice *v, unsigned int frequency) { (void)v; (void)frequency; return 1; }
static inline int audio_output_pause(struct audio_voice *v, int pause) { (void)v; (void)pause; return 1; }
static inline int audio_output_buffer_play(struct audio_voice *v) { (void)v; return 1; }
static inline int audio_output_buffer_seek(struct audio_voice *v, unsigned int position) { (void)v; (void)position; return 1; }
static inline int audio_output_buffer_loop(struct audio_voice *v) { (void)v; return 1; }
/* The software mixer has no submix stage; it behaves as headroom 0. */
static inline int audio_output_mixbin_headroom(unsigned int mix_bins, unsigned int headroom) { (void)mix_bins; (void)headroom; return 1; }
#endif
#endif
