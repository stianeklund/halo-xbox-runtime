/* Stereo software mixer shared by the nxdk backend and host tests.
 * All calls require the backend lock. No allocation or game callbacks here. */
#ifndef HALO_AUDIO_MIXER_H
#define HALO_AUDIO_MIXER_H

#define AUDIO_PACKET_COUNT 4
#define AUDIO_RATE 48000
#define AUDIO_ABORTED ((unsigned int)0x80004004)
#define AUDIO_PENDING ((unsigned int)0x8000000a)

#pragma pack(push, 4)
struct audio_format {
  unsigned short tag, channels;
  unsigned int rate, bytes_per_second;
  unsigned short block_align, bits, extra_size, samples_per_block;
};

struct audio_packet {
  const unsigned char *data;
  unsigned int size;
  unsigned int *completed_size, *status;
  void *context;
  unsigned int frames, cursor, result;
  int state; /* 0 free, 1 playing, 2 waiting for game-thread completion */
};

struct audio_voice {
  struct audio_format format;
  struct audio_packet packets[AUDIO_PACKET_COUNT];
  unsigned int read_packet, write_packet, phase, step;
  unsigned int decoded_block;
  short decoded[64 * 2];
  int gain_left, gain_right; /* Q15; mono is copied to both outputs */
  int paused;
  const unsigned char *buffer;
  unsigned int buffer_size, buffer_cursor, loop_start, loop_end;
  int playing, looping;
  void *backend; /* Opaque nxdk-audio state; NULL for the software backend. */
  /* Retail DirectSound's results for a mono DS_CTRL3D voice, which the APU
   * backend plays on a hardware 3D voice: levels in mB for mix bins 6-10
   * (front, rear, front, rear, I3DL2 send), TAR_FCB, the pseudo-angles that
   * select its HRTF pair, and the Doppler frequency ratio. */
  int is_3d;
  int level[5];
  unsigned int filter;
  float azimuth, elevation, doppler;
};
#pragma pack(pop)

int audio_format_valid(const struct audio_format *format);
int audio_data_valid(const struct audio_format *format, const unsigned char *data, unsigned int size);
void audio_voice_init(struct audio_voice *voice, const struct audio_format *format);
int audio_voice_frequency(struct audio_voice *voice, unsigned int frequency);
int audio_voice_ready(const struct audio_voice *voice);
int audio_voice_active(const struct audio_voice *voice);
int audio_voice_submit(struct audio_voice *voice, const struct audio_packet *packet);
void audio_voice_flush(struct audio_voice *voice);
int audio_voice_completion(struct audio_voice *voice, struct audio_packet *packet);
void audio_voice_mix(struct audio_voice *voice, int *stereo, unsigned int frames);
void audio_mix_clip(const int *mixed, short *output, unsigned int frames);
int audio_decode_adpcm(const unsigned char *block, unsigned int channels, short *output);
unsigned int audio_dma_retired(unsigned int registers, unsigned int oldest, unsigned int queued);

/* Implemented with nxdk headers in crt/nxdk_audio.c. */
int standalone_audio_init(void);
void standalone_audio_lock(void);
void standalone_audio_unlock(void);
void standalone_audio_render(short *output, unsigned int frames);
void standalone_audio_poll(void);
#endif
