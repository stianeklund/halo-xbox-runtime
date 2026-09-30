/* Host driver for the Bink codec compat files (compat/bink/video.c,
 * audio.c, blit.c). Walks a .bik container the way BinkOpen/BinkDoFrame
 * do and writes the decoded frames as raw yuv420p and the first audio track
 * as s16le, so tools/test_bink.py can compare them against ffmpeg. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../compat/bink/internal.h"

void *FUN_002314b0(unsigned long size) { return calloc(1, size); }
void FUN_00231520(void *allocation) { free(allocation); }

static uint32_t rl32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char **argv)
{
  FILE *f, *video_out, *audio_out, *rgb_out = 0;
  unsigned char *rgb = 0;
  unsigned char *file, *mask;
  long file_size;
  uint32_t frames, width, height, bink_type, tracks, open_flags = 0, i, t;
  uint32_t *offsets;
  unsigned long d4, d8, dirty_w, dirty_h, plane_size, sizes[BINK_BUNDLE_COUNT], scratch[BINK_BUNDLE_COUNT];
  unsigned char *planes[2];
  unsigned char *alpha[2] = { 0, 0 };
  void *audio = 0;
  unsigned long max_frames = ~0ul;
  int cur = 0;

  if (argc < 4) {
    fprintf(stderr, "usage: %s file.bik video.yuv audio.s16 [frames.bgra [max_frames]]\n", argv[0]);
    return 2;
  }
  if (argc > 5) {
    max_frames = strtoul(argv[5], 0, 0);
  }
  f = fopen(argv[1], "rb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  file_size = ftell(f);
  fseek(f, 0, SEEK_SET);
  file = malloc(file_size + 64);
  if (fread(file, 1, file_size, f) != (size_t)file_size) {
    return 1;
  }
  memset(file + file_size, 0, 64);
  fclose(f);

  if (file[3] == 'f' || file[3] == 'g') {
    open_flags |= 0x18000;
  } else if (file[3] == 'h') {
    open_flags |= 0x8000;
  }
  frames = rl32(file + 8);
  width = rl32(file + 20);
  height = rl32(file + 24);
  bink_type = rl32(file + 36);
  tracks = rl32(file + 40);
  offsets = malloc((frames + 1) * 4);
  for (i = 0; i <= frames; i++) {
    offsets[i] = rl32(file + 44 + tracks * 12 + i * 4);
  }

  d4 = (((width + 1) >> 1) + 7) & ~7ul;
  d8 = (((height + 1) >> 1) + 7) & ~7ul;
  dirty_w = d4 * 2;
  dirty_h = d8 * 2;
  plane_size = dirty_w * dirty_h + d4 * d8 * 2;
  planes[0] = calloc(1, plane_size);
  planes[1] = calloc(1, plane_size);
  if (bink_type & BINK_TYPE_ALPHA) {
    alpha[0] = calloc(1, dirty_w * dirty_h);
    alpha[1] = calloc(1, dirty_w * dirty_h);
  }
  mask = calloc(1, (dirty_w >> 4) * (dirty_h >> 4) + 16);
  FUN_00236210(sizes, dirty_w);
  for (i = 0; i < BINK_BUNDLE_COUNT; i++) {
    scratch[i] = (unsigned long)malloc(sizes[i]);
  }
  if (tracks) {
    uint32_t type = rl32(file + 44 + tracks * 4);
    audio = FUN_0023cd10(type & 0xffff, (type >> 29) & 1 ? 2 : 1, type >> 16);
    if (!audio) {
      fprintf(stderr, "audio open failed\n");
      return 1;
    }
  }

  if (argc > 4) {
    rgb_out = fopen(argv[4], "wb");
    rgb = malloc(width * height * 4);
    FUN_00234110(3);
  }
  video_out = fopen(argv[2], "wb");
  audio_out = fopen(argv[3], "wb");
  for (i = 0; i < frames && i < max_frames; i++) {
    const unsigned char *comp = file + (offsets[i] & ~1u);
    unsigned long y;

    for (t = 0; t < tracks; t++) {
      uint32_t size = rl32(comp);
      if (t == 0 && size && audio) {
        /* BinkDoFrame: first dword is the decoded byte count */
        unsigned long left = rl32(comp + 4);
        const unsigned char *in = comp + 8;
        while (left) {
          void *output;
          unsigned long bytes;
          FUN_0023ec50(audio, &output, &bytes, in, &in);
          if (bytes > left) {
            bytes = left;
          }
          left -= bytes;
          fwrite(output, 1, bytes, audio_out);
        }
      }
      comp += 4 + size;
    }

    memset(mask, 0, (dirty_w >> 4) * (dirty_h >> 4));
    FUN_00238070(planes[cur ^ 1], planes[cur], alpha[cur ^ 1], alpha[cur], mask, width, height, dirty_w,
                 dirty_h, comp, offsets[i] & 1, scratch, open_flags, bink_type);
    cur ^= 1;
    if (rgb_out) {
      /* BinkCopyToBuffer's unmasked X8R8G8B8 path, as Halo requests it */
      FUN_00244b40(rgb, 0, 0, width * 4, planes[cur], 0, 0, width, height, dirty_w, dirty_h, 0x80000003);
      fwrite(rgb, 1, width * height * 4, rgb_out);
    }
    for (y = 0; y < height; y++) {
      fwrite(planes[cur] + y * dirty_w, 1, width, video_out);
    }
    for (t = 0; t < 2; t++) {
      for (y = 0; y < height / 2; y++) {
        fwrite(planes[cur] + dirty_w * dirty_h + t * d4 * d8 + y * d4, 1, width / 2, video_out);
      }
    }
  }
  fclose(video_out);
  if (rgb_out) {
    fclose(rgb_out);
    free(rgb);
  }
  fclose(audio_out);
  if (audio) {
    FUN_0023ce50(audio);
  }
  for (i = 0; i < BINK_BUNDLE_COUNT; i++) {
    free((void *)scratch[i]);
  }
  free(mask);
  free(alpha[0]);
  free(alpha[1]);
  free(planes[0]);
  free(planes[1]);
  free(offsets);
  free(file);
  return 0;
}
