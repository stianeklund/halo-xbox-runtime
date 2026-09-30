/* Thread-safe PCM packet stream for RAD Bink movie audio (compat/bink/sound.c).
 * Unlike the XDK stream ABI, completions are retired by the owner, so the
 * Bink sound thread never runs game DirectSound callbacks. The silent
 * backend cannot open one; Bink then plays the movie without sound. */
#ifndef HALO_PCM_STREAM_H
#define HALO_PCM_STREAM_H

struct pcm_stream;

/* Starts paused. Packets are 16- or 8-bit PCM, at most 4 in flight. */
struct pcm_stream *pcm_stream_open(unsigned int rate, unsigned int channels, unsigned int bits);
/* *status is 0x8000000a while queued, then the packet result. */
int pcm_stream_submit(struct pcm_stream *stream, const void *data, unsigned int size,
                      volatile unsigned int *status);
void pcm_stream_retire(struct pcm_stream *stream);
/* Nonzero while any submitted packet is still queued or playing. */
int pcm_stream_active(struct pcm_stream *stream);
int pcm_stream_pause(struct pcm_stream *stream, int pause);
/* Hundredths of a dB, -10000..0. */
void pcm_stream_volume(struct pcm_stream *stream, int volume);
/* Aborts queued packets; returns 0 if the hardware may still read them. */
int pcm_stream_flush(struct pcm_stream *stream);
/* Returns 0 if packet memory must be leaked because playback did not stop. */
int pcm_stream_close(struct pcm_stream *stream);
#endif
