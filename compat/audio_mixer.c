#include "audio_mixer.h"

static const int ima_steps[89] = {
  7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,
  66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,
  371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,
  1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,
  5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,
  16818,18500,20350,22385,24623,27086,29794,32767
};
static const int ima_indices[8] = {-1,-1,-1,-1,2,4,6,8};

/* AC97 CIV/LVI/SR, read in one supported 32-bit MMIO access. CIV names the
 * current descriptor; when DMA halts at LVI that descriptor is also done.
 * Count all completions, including ones whose IRQ/DPC signals coalesced. */
unsigned int audio_dma_retired(unsigned int registers, unsigned int oldest, unsigned int queued)
{
  unsigned int current = registers & 31;
  unsigned int last = (registers >> 8) & 31;
  unsigned int status = registers >> 16;
  unsigned int retired = (current - oldest) & 31;
  if ((status & 1) && current == last) retired++;
  return retired <= queued ? retired : 0;
}

static int clamp_sample(int sample)
{
  return sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample;
}

static int read_short(const unsigned char *p)
{
  unsigned int value = p[0] | ((unsigned int)p[1] << 8);
  return value >= 32768 ? (int)value - 65536 : (int)value;
}

/* Xbox IMA: 36 bytes/channel, predictor is sample 0, 63 decoded nibbles.
 * The final nibble is padding, NOT a 65th sample. Stereo interleaves four
 * encoded bytes/channel, after the two four-byte headers.
 * Format reference: vgmstream src/coding/ima_decoder.c decode_xbox_ima. */
int audio_decode_adpcm(const unsigned char *block, unsigned int channels, short *output)
{
  unsigned int channel, i, nibble, offset;
  int predictor, index, step, delta;
  if (channels != 1 && channels != 2) return 0;
  for (channel = 0; channel < channels; channel++) {
    index = block[channel * 4 + 2];
    if (index > 88 || block[channel * 4 + 3] != 0) return 0;
    predictor = read_short(block + channel * 4);
    output[channel] = (short)predictor;
    for (i = 0; i < 63; i++) {
      offset = channels * 4 + (i / 8) * channels * 4 + channel * 4 + (i % 8) / 2;
      nibble = (block[offset] >> ((i & 1) * 4)) & 15;
      step = ima_steps[index];
      delta = step >> 3;
      if (nibble & 1) delta += step >> 2;
      if (nibble & 2) delta += step >> 1;
      if (nibble & 4) delta += step;
      predictor = clamp_sample(predictor + ((nibble & 8) ? -delta : delta));
      index += ima_indices[nibble & 7];
      if (index < 0) index = 0;
      if (index > 88) index = 88;
      output[(i + 1) * channels + channel] = (short)predictor;
    }
  }
  return 1;
}

int audio_format_valid(const struct audio_format *f)
{
  if (!f || (f->channels != 1 && f->channels != 2) || !f->rate || f->rate > 192000)
    return 0;
  if (f->tag == 1)
    return (f->bits == 8 || f->bits == 16) && f->block_align == f->channels * (f->bits / 8);
  return f->tag == 0x69 && f->bits == 4 && f->block_align == 36 * f->channels &&
         f->extra_size >= 2 && f->samples_per_block == 64;
}

int audio_data_valid(const struct audio_format *f, const unsigned char *data, unsigned int size)
{
  unsigned int block, channel;
  if (!audio_format_valid(f) || !data || !size || size % f->block_align || size > 0x4000000)
    return 0;
  if (f->tag == 0x69) {
    for (block = 0; block < size; block += f->block_align)
      for (channel = 0; channel < f->channels; channel++)
        if (data[block + channel * 4 + 2] > 88 || data[block + channel * 4 + 3] != 0)
          return 0;
  }
  return 1;
}

void audio_voice_init(struct audio_voice *v, const struct audio_format *format)
{
  unsigned int i;
  unsigned char *bytes = (unsigned char *)v;
  for (i = 0; i < sizeof(*v); i++) bytes[i] = 0;
  v->format = *format;
  v->gain_left = v->gain_right = 32768;
  v->decoded_block = ~0u;
  audio_voice_frequency(v, format->rate);
}

int audio_voice_frequency(struct audio_voice *v, unsigned int frequency)
{
  if (!frequency || frequency > 192000) return 0;
  v->step = (unsigned int)(((unsigned long long)frequency << 16) / AUDIO_RATE);
  return 1;
}

int audio_voice_ready(const struct audio_voice *v)
{
  return v->packets[v->write_packet].state == 0;
}

int audio_voice_active(const struct audio_voice *v)
{
  unsigned int i;
  if (v->playing) return 1;
  for (i = 0; i < AUDIO_PACKET_COUNT; i++)
    if (v->packets[i].state == 1) return 1;
  return 0;
}

int audio_voice_submit(struct audio_voice *v, const struct audio_packet *packet)
{
  struct audio_packet *p;
  if (!audio_voice_ready(v) || !audio_data_valid(&v->format, packet->data, packet->size)) return 0;
  /* Validate every header before accepting a packet. Malformed input must
   * fail synchronously instead of generating arbitrary audio later. */
  p = &v->packets[v->write_packet];
  *p = *packet;
  p->cursor = 0;
  p->frames = (packet->size / v->format.block_align) * (v->format.tag == 0x69 ? 64 : 1);
  p->state = 1;
  p->result = 0;
  if (p->status) *p->status = AUDIO_PENDING;
  if (p->completed_size) *p->completed_size = 0;
  v->write_packet = (v->write_packet + 1) % AUDIO_PACKET_COUNT;
  return 1;
}

void audio_voice_flush(struct audio_voice *v)
{
  unsigned int i;
  for (i = 0; i < AUDIO_PACKET_COUNT; i++) {
    struct audio_packet *p = &v->packets[i];
    if (p->state != 0) {
      p->result = AUDIO_ABORTED;
      p->state = 2;
    }
  }
  v->playing = 0;
  v->phase = 0;
  v->decoded_block = ~0u;
}

int audio_voice_completion(struct audio_voice *v, struct audio_packet *packet)
{
  struct audio_packet *p = &v->packets[v->read_packet];
  if (p->state != 2) return 0;
  *packet = *p;
  p->state = 0;
  v->read_packet = (v->read_packet + 1) % AUDIO_PACKET_COUNT;
  if (packet->completed_size)
    *packet->completed_size = packet->result == 0 ? packet->size :
      (packet->cursor / (v->format.tag == 0x69 ? 64 : 1)) * v->format.block_align;
  if (packet->status) *packet->status = packet->result;
  return 1;
}

static void sample_frame(struct audio_voice *v, const unsigned char *data,
                         unsigned int cursor, int *left, int *right)
{
  unsigned int channels = v->format.channels;
  unsigned int block;
  if (v->format.tag == 0x69) {
    block = cursor / 64;
    if (v->decoded_block != block) {
      audio_decode_adpcm(data + block * v->format.block_align, channels, v->decoded);
      v->decoded_block = block;
    }
    *left = v->decoded[(cursor % 64) * channels];
    *right = channels == 2 ? v->decoded[(cursor % 64) * 2 + 1] : *left;
  } else if (v->format.bits == 16) {
    data += cursor * v->format.block_align;
    *left = read_short(data);
    *right = channels == 2 ? read_short(data + 2) : *left;
  } else {
    data += cursor * channels;
    *left = ((int)data[0] - 128) * 256;
    *right = channels == 2 ? ((int)data[1] - 128) * 256 : *left;
  }
}

void audio_voice_mix(struct audio_voice *v, int *stereo, unsigned int frames)
{
  unsigned int i, slot, advance, count, end;
  struct audio_packet *p;
  int left, right;
  if (v->paused) return;
  for (i = 0; i < frames; i++) {
    p = 0;
    if (v->playing) {
      count = (v->buffer_size / v->format.block_align) * (v->format.tag == 0x69 ? 64 : 1);
      end = v->looping && v->loop_end ? v->loop_end : count;
      if (v->buffer_cursor >= end) {
        if (!v->looping || end <= v->loop_start) { v->playing = 0; break; }
        v->buffer_cursor = v->loop_start + (v->buffer_cursor - end) % (end - v->loop_start);
        v->decoded_block = ~0u;
      }
      sample_frame(v, v->buffer, v->buffer_cursor, &left, &right);
    } else {
      /* Completed slots stay owned by the game until DoWork dispatches them.
       * Find the oldest playing packet, skipping those completion slots. */
      for (slot = 0; slot < AUDIO_PACKET_COUNT; slot++) {
        p = &v->packets[(v->read_packet + slot) % AUDIO_PACKET_COUNT];
        if (p->state == 1) break;
      }
      if (slot == AUDIO_PACKET_COUNT) break;
      sample_frame(v, p->data, p->cursor, &left, &right);
    }
    stereo[i * 2] += left * v->gain_left / 32768;
    stereo[i * 2 + 1] += right * v->gain_right / 32768;
    v->phase += v->step;
    advance = v->phase >> 16;
    v->phase &= 65535;
    if (v->playing) {
      v->buffer_cursor += advance;
      count = (v->buffer_size / v->format.block_align) * (v->format.tag == 0x69 ? 64 : 1);
      if (!v->looping && v->buffer_cursor >= count) v->playing = 0;
    } else {
      p->cursor += advance;
      /* Carry resampler overshoot across packet boundaries. */
      while (p && p->cursor >= p->frames) {
        advance = p->cursor - p->frames;
        p->cursor = p->frames;
        p->state = 2;
        v->decoded_block = ~0u;
        p = 0;
        for (slot = 0; slot < AUDIO_PACKET_COUNT; slot++) {
          struct audio_packet *next = &v->packets[(v->read_packet + slot) % AUDIO_PACKET_COUNT];
          if (next->state == 1) { p = next; break; }
        }
        if (p) p->cursor += advance;
      }
      if (!p) v->phase = 0;
    }
  }
}

void audio_mix_clip(const int *mixed, short *output, unsigned int frames)
{
  unsigned int i;
  /* DirectSound voice headroom is already included in each voice gain. */
  for (i = 0; i < frames * 2; i++)
    output[i] = (short)clamp_sample(mixed[i]);
}
