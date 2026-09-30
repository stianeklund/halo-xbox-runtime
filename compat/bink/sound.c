/*
 * Standalone compat: RAD Bink DirectSound backend (BinkOpenDirectSound).
 *
 * The January 2002 binkxbox backend streamed through a looping
 * DirectSoundBuffer read by play/write cursors. This one keeps its contract
 * (BestSizeIn16 chunks, a priming fill before playback, SoundDroppedOut
 * after a starved buffer or a 540ms service gap, the same volume curve) over
 * a queue of PCM packets (compat/audio/pcm_stream.h).
 */
#include "internal.h"
#include "../audio/pcm_stream.h"

#define CHUNK_COUNT 4
#define CHUNK_PENDING 0x8000000au
#define DROPOUT_TIMEOUT_MS 540

struct bink_pcm_state {
  struct pcm_stream *stream;
  unsigned char *memory;
  unsigned long chunk_size;
  unsigned long block_align;
  unsigned long next_chunk;
  volatile unsigned int status[CHUNK_COUNT];
  unsigned long prime_bytes;
  unsigned long primed_bytes;
  unsigned long last_timer;
  long started;
  long paused;
  long volume;
};
typedef char bink_pcm_state_fits[sizeof(struct bink_pcm_state) <= 0x80 ? 1 : -1];

#define PCM_STATE(sound) ((struct bink_pcm_state *)(sound)->pad_44)

/* binkxbox's ds_volume_table: 0..32767 in 128 steps, hundredths of a dB. */
static const short volume_table[128] = {
  -10000, -4208, -3606, -3253, -3003, -2810, -2651, -2517, -2401, -2299, -2208, -2125, -2049,
  -1980,  -1915, -1855, -1799, -1747, -1697, -1650, -1606, -1563, -1523, -1484, -1447, -1412,
  -1378,  -1345, -1313, -1283, -1253, -1225, -1197, -1171, -1145, -1119, -1095, -1071, -1048,
  -1025,  -1003, -982,  -961,  -941,  -921,  -901,  -882,  -863,  -845,  -827,  -810,  -792,
  -776,   -759,  -743,  -727,  -711,  -696,  -681,  -666,  -651,  -637,  -623,  -609,  -595,
  -582,   -569,  -555,  -543,  -530,  -517,  -505,  -493,  -481,  -469,  -457,  -446,  -435,
  -423,   -412,  -401,  -391,  -380,  -369,  -359,  -349,  -339,  -329,  -319,  -309,  -299,
  -290,   -280,  -271,  -261,  -252,  -243,  -234,  -225,  -216,  -208,  -199,  -190,  -182,
  -174,   -165,  -157,  -149,  -141,  -133,  -125,  -117,  -109,  -101,  -94,   -86,   -79,
  -71,    -64,   -57,   -49,   -42,   -35,   -28,   -21,   -14,   -7,    0,
};

static void start_play(struct bink_pcm_state *state)
{
  state->started = 1;
  pcm_stream_pause(state->stream, 0);
  state->last_timer = FUN_002328c0();
}

static void stop_and_flush(struct bink_pcm_state *state)
{
  pcm_stream_pause(state->stream, 1);
  pcm_stream_flush(state->stream);
  state->started = 0;
  state->primed_bytes = 0;
}

static long __stdcall bink_ds_ready(BINKSND *sound)
{
  struct bink_pcm_state *state = PCM_STATE(sound);
  unsigned long now;

  if (state->paused || !sound->OnOff) {
    return 0;
  }
  pcm_stream_retire(state->stream);
  if (state->started) {
    now = FUN_002328c0();
    if (now - state->last_timer >= DROPOUT_TIMEOUT_MS || !pcm_stream_active(state->stream)) {
      /* Starved: prime again before playing the next packets. */
      sound->SoundDroppedOut = 1;
      pcm_stream_pause(state->stream, 1);
      state->started = 0;
      state->primed_bytes = 0;
    }
    state->last_timer = now;
  }
  return state->status[state->next_chunk] != CHUNK_PENDING;
}

static long __stdcall bink_ds_lock(BINKSND *sound, unsigned char **destination, unsigned long *bytes)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  if (state->status[state->next_chunk] == CHUNK_PENDING) {
    return 0;
  }
  *destination = state->memory + state->next_chunk * state->chunk_size;
  *bytes = state->chunk_size;
  return 1;
}

static long __stdcall bink_ds_unlock(BINKSND *sound, unsigned long bytes)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  bytes -= bytes % state->block_align;
  if (!bytes) {
    return 1;
  }
  if (!pcm_stream_submit(state->stream, state->memory + state->next_chunk * state->chunk_size,
                         bytes, &state->status[state->next_chunk])) {
    return 0;
  }
  state->next_chunk = (state->next_chunk + 1) % CHUNK_COUNT;
  if (!state->started) {
    state->primed_bytes += bytes;
    /* A short final packet ends the movie's audio before the prime fills. */
    if (!state->paused &&
        (state->primed_bytes >= state->prime_bytes || bytes < state->chunk_size ||
         state->status[state->next_chunk] == CHUNK_PENDING)) {
      start_play(state);
    }
  }
  return 1;
}

static void __stdcall bink_ds_volume(BINKSND *sound, long volume)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  if (volume > 32767) {
    volume = 32767;
  } else if (volume < 0) {
    volume = 0;
  }
  state->volume = volume;
  pcm_stream_volume(state->stream, volume_table[volume * 127 / 32767]);
}

/* A stereo stream has no pan control on this mixer; mix bins likewise. */
static void __stdcall bink_ds_pan(BINKSND *sound, long pan)
{
  (void)sound;
  (void)pan;
}

static void __stdcall bink_ds_mix_bins(BINKSND *sound, unsigned long bins)
{
  (void)sound;
  (void)bins;
}

static long __stdcall bink_ds_on_off(BINKSND *sound, long on)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  if (on) {
    if (!sound->OnOff) {
      state->started = 0;
      state->primed_bytes = 0;
      sound->OnOff = 1;
    }
    return sound->OnOff;
  }
  if (sound->OnOff) {
    stop_and_flush(state);
    sound->OnOff = 0;
  }
  return 0;
}

static long __stdcall bink_ds_pause(BINKSND *sound, long pause)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  if (pause) {
    if (state->started) {
      pcm_stream_pause(state->stream, 1);
    }
    state->paused = 1;
    return 1;
  }
  if (state->started || state->primed_bytes >= state->prime_bytes) {
    start_play(state);
  }
  state->paused = 0;
  return 0;
}

static void __stdcall bink_ds_close(BINKSND *sound)
{
  struct bink_pcm_state *state = PCM_STATE(sound);

  if (!state->stream) {
    return;
  }
  pcm_stream_pause(state->stream, 1);
  /* Keep the packet memory if playback could not be stopped. */
  if (pcm_stream_close(state->stream)) {
    FUN_00231520(state->memory);
  }
  state->stream = 0;
}

static long __stdcall bink_ds_open(BINKSND *sound, unsigned long frequency, long bits, long channels,
                                 unsigned long flags, BINK *bink)
{
  struct bink_pcm_state *state = PCM_STATE(sound);
  unsigned long bytes_per_second, movie_bytes, i;

  (void)flags;
  csmemset(sound, 0, sizeof(*sound));
  if ((bits != 8 && bits != 16) || channels < 1 || channels > 2 || !frequency) {
    return 0;
  }
  state->block_align = (unsigned long)(bits >> 3) * (unsigned long)channels;
  bytes_per_second = state->block_align * frequency;
  /* Four ~120ms packets: about the 600ms of the original ring. */
  state->chunk_size = (bytes_per_second * 120 / 1000 + 255) & ~255ul;
  state->chunk_size -= state->chunk_size % state->block_align;
  state->stream = pcm_stream_open(frequency, (unsigned int)channels, (unsigned int)bits);
  if (!state->stream) {
    return 0;
  }
  state->memory = FUN_002314b0(state->chunk_size * CHUNK_COUNT);
  if (!state->memory) {
    pcm_stream_close(state->stream);
    state->stream = 0;
    return 0;
  }
  for (i = 0; i < CHUNK_COUNT; i++) {
    state->status[i] = 0;
  }
  movie_bytes = bink->FrameRate ?
    bink_mul_div(bink->Frames * bink->FrameRateDiv, bytes_per_second, bink->FrameRate) : 0;
  state->prime_bytes = state->chunk_size * CHUNK_COUNT;
  if (movie_bytes && movie_bytes < state->prime_bytes) {
    state->prime_bytes = movie_bytes;
  }
  sound->Bits = bits;
  sound->Channels = channels;
  sound->Frequency = frequency;
  sound->Latency = 0;
  sound->BestSizeIn16 = state->chunk_size * (bits == 8 ? 2 : 1);
  sound->OnOff = 1;
  sound->Ready = bink_ds_ready;
  sound->Lock = bink_ds_lock;
  sound->Unlock = bink_ds_unlock;
  sound->Volume = bink_ds_volume;
  sound->Pan = bink_ds_pan;
  sound->Pause = bink_ds_pause;
  sound->SetOnOff = bink_ds_on_off;
  sound->Close = bink_ds_close;
  sound->MixBins = bink_ds_mix_bins;
  bink_ds_volume(sound, 32767);
  return 1;
}

/* 0x231e80. Halo passes its DirectSound device; the mixer is process-wide. */
bink_sound_open_proc __stdcall BinkOpenDirectSound(unsigned long direct_sound)
{
  (void)direct_sound;
  return bink_ds_open;
}
