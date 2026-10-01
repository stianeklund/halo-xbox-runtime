/*
 * Standalone compat: RAD Bink playback internals called by Halo's
 * decompiled front end (halo/src/halo/bink/bink.c).
 *
 * The sound ring (service_sound, pad_sound), BinkPause, BinkSetSoundOnOff,
 * set_frame, the frame timers and the RADCB priority/dispatch callbacks for
 * BINKIO read-ahead and sound servicing (cachebeta.xbe 0x22e000-0x2302d0).
 */
#include "internal.h"

/* BINK+0x3e0/+0x3e4: last RADCB iteration that serviced IO / sound. */
#define BINK_IO_ITERATION(bink) (*(unsigned long *)((bink)->pad_3dc + 4))
#define BINK_SOUND_ITERATION(bink) (*(unsigned long *)((bink)->pad_3dc + 8))

/* S16 -> U8 for 8-bit tracks; returns the advanced destination. */
static unsigned char *convert_sound_to_u8(unsigned char *destination, const unsigned char *source,
                                          unsigned long bytes)
{
  for (bytes >>= 1; bytes--; source += 2) {
    *destination++ = (unsigned char)(source[1] + 128);
  }
  return destination;
}

static unsigned char *copy_sound(BINK *bink, unsigned char *destination,
                                 const unsigned char *source, unsigned long bytes)
{
  if (bink->sndconvert8) {
    return convert_sound_to_u8(destination, source, bytes);
  }
  memcpy(destination, source, bytes);
  return destination + bytes;
}

/* 0x22e0c0. service_sound: move buffered PCM from the ring into the sound
 * backend while it has room. Runs on the game thread and the sound thread. */
void __stdcall FUN_0022e0c0(BINK *bink)
{
  BINKSND *sound = &bink->sound;
  unsigned char *destination;
  unsigned long bytes, tail;

  if (!sound->Ready || !FUN_00232430(sound_handler, &bink->sound_callback)) {
    return;
  }
  while (sound->Ready(sound)) {
    if (bink->sndamt <= sound->BestSizeIn16 &&
        ((long)bink->FrameNum <= bink->sndendframe || !bink->sndamt)) {
      break;
    }
    if (!sound->Lock(sound, &destination, &bytes)) {
      break;
    }
    if (bink->sndconvert8) {
      bytes *= 2;
    }
    if (bytes > bink->sndamt) {
      bytes = bink->sndamt;
    }
    bink->sndamt -= bytes;
    tail = (unsigned long)(bink->sndend - bink->sndreadpos);
    if (tail < bytes) {
      destination = copy_sound(bink, destination, bink->sndreadpos, tail);
      copy_sound(bink, destination, bink->sndbuf, bytes - tail);
      bink->sndreadpos = bink->sndbuf + (bytes - tail);
    } else {
      copy_sound(bink, destination, bink->sndreadpos, bytes);
      bink->sndreadpos += bytes;
    }
    if (bink->sndconvert8) {
      bytes >>= 1;
    }
    sound->Unlock(sound, bytes);
  }
  FUN_002324f0(sound_handler, &bink->sound_callback);
}

/* 0x22e000. pad_sound: prepend silence up to the priming threshold; returns
 * its duration in ms. */
unsigned long dosilence(BINK *bink)
{
  unsigned long missing, duration, position, tail, rate;

  if (bink->sndamt >= bink->sndprime) {
    return 0;
  }
  missing = bink->sndprime - bink->sndamt;
  rate = ((unsigned long)bink->sound.Channels * (unsigned long)bink->sound.Bits *
          bink->sound.Frequency) >> 3;
  duration = rate ? missing * 1000 / rate : 0;
  position = (unsigned long)(bink->sndreadpos - bink->sndbuf);
  if (missing > position) {
    tail = missing - position;
    bink->sndreadpos = bink->sndbuf + (bink->sndbufsize - tail);
    csmemset(bink->sndbuf, 0, position);
    csmemset(bink->sndreadpos, 0, tail);
  } else {
    bink->sndreadpos -= missing;
    csmemset(bink->sndreadpos, 0, missing);
  }
  bink->sndamt += missing;
  return duration;
}

/* 0x22f180. update_slowest_frame */
void endframe(BINK *bink, unsigned long now)
{
  if (!bink->longestframetime) {
    return;
  }
  now -= bink->longestframetime;
  bink->longestframetime = 0;
  if (now > bink->slowestframetime) {
    bink->slowest2frametime = bink->slowestframetime;
    bink->slowest2frame = bink->slowestframe;
    bink->slowestframetime = now;
    bink->slowestframe = bink->FrameNum;
  } else if (now > bink->slowest2frametime) {
    bink->slowest2frame = bink->FrameNum;
    bink->slowest2frametime = now;
  }
}

/* 0x22f3e0. BinkPause */
long __stdcall FUN_0022f3e0(BINK *bink, long pause)
{
  unsigned long now;

  if (!bink) {
    return 0;
  }
  now = FUN_002328c0();
  if (bink->startblittime) {
    bink->timeblit += now - bink->startblittime;
    bink->startblittime = 0;
  }
  endframe(bink, now);
  if (!pause && bink->Paused) {
    bink->starttime = 0;
    bink->big_sound_skip_adj = 0;
  }
  bink->Paused = pause;
  if (bink->trackindex != -1) {
    bink->sound.Pause(&bink->sound, pause);
  }
  bink->skippedlastblit = 0;
  if (bink->playedframes && bink->trackindex != -1) {
    FUN_0022e0c0(bink);
  }
  return bink->Paused;
}

/* 0x2301a0. BinkSetSoundOnOff */
long __stdcall FUN_002301a0(BINK *bink, long on)
{
  long result = 0;

  if (!bink || bink->trackindex == -1 || !bink->sound.SetOnOff) {
    return 0;
  }
  result = bink->sound.SetOnOff(&bink->sound, on);
  if (!result) {
    if (bink->SoundOn) {
      FUN_00232460(sound_handler, &bink->sound_callback);
      bink->sndwritepos = bink->sndbuf;
      bink->sndreadpos = bink->sndbuf;
      bink->sndamt = 0;
      bink->SoundOn = 0;
      FUN_002324f0(sound_handler, &bink->sound_callback);
    }
  } else if (!bink->SoundOn) {
    FUN_00232460(sound_handler, &bink->sound_callback);
    if (bink->FrameNum != 1 && bink->FrameNum != bink->InternalFrames) {
      bink->sndamt = bink->sndprime;
      csmemset(bink->sndbuf, 0, bink->sndamt);
      bink->sndwritepos = bink->sndbuf + bink->sndamt;
      bink->sndreadpos = bink->sndbuf;
    }
    bink->SoundOn = 1;
    bink->starttime = 0;
    bink->big_sound_skip_adj = 0;
    FUN_002324f0(sound_handler, &bink->sound_callback);
  }
  return result;
}

/* 0x2302d0. set_frame: frame is one-based; 0 and 1 both select the first.
 * Not in the original: a frame past the frame table (InternalFrames + 1
 * offsets), or one larger than the header's largest frame (the compframe
 * allocation), fails as a read error instead of reading past frameoffsets or
 * overflowing compframe. */
void GotoFrame(BINK *bink, unsigned long frame)
{
  unsigned long size;

  if (frame) {
    frame--;
  }
  if (frame >= bink->InternalFrames) {
    bink->ReadError = 1;
    return;
  }
  size = (bink->frameoffsets[frame + 1] & ~1ul) - (bink->frameoffsets[frame] & ~1ul);
  if (size > bink->field_e8) {
    bink->ReadError = 1;
    return;
  }
  if (bink->trackindex != -1) {
    FUN_0022e0c0(bink);
  }
  bink->LastFrameNum = bink->FrameNum;
  if (!frame) {
    if (bink->SoundOn) {
      FUN_002301a0(bink, 0);
      FUN_002301a0(bink, 1);
    }
    bink->starttime = 0;
    bink->big_sound_skip_adj = 0;
  }
  if (bink->preloadptr) {
    bink->compframe = (unsigned char *)bink->preloadptr +
                      ((bink->frameoffsets[frame] & ~1ul) - (bink->frameoffsets[0] & ~1ul));
  } else {
    bink->io.ReadFrame(&bink->io, frame, (long)(bink->frameoffsets[frame] & ~1ul), bink->compframe,
                       size);
  }
  if (bink->trackindex != -1) {
    FUN_0022e0c0(bink);
  }
  bink->FrameNum = frame + 1;
}

/* 0x22e2d0 */
void inittimer(BINK *bink)
{
  if (!bink->starttime) {
    bink->starttime = FUN_002328c0();
    bink->startframe = bink->playedframes - 1;
  }
}

unsigned long bink_mul_div(unsigned long a, unsigned long b, unsigned long c)
{
  unsigned long long numerator = (unsigned long long)a * b, remainder = 0, quotient = 0;
  int bit;

  if (!c) {
    return 0;
  }
  for (bit = 63; bit >= 0; bit--) {
    remainder = (remainder << 1) | ((numerator >> bit) & 1);
    if (remainder >= c) {
      remainder -= c;
      quotient |= 1ull << bit;
    }
  }
  return (unsigned long)quotient;
}

/* 0x22e360. Largest byte span of any runtime_frames-long window (keyframe
 * bits included, as the original does); all_keyframes reports whether every
 * frame is a keyframe. */
unsigned long high1secrate(unsigned long frames, unsigned long runtime_frames,
                           unsigned long *highest_frame, unsigned long *all_keyframes,
                           unsigned long *frameoffsets)
{
  unsigned long best = 0, best_frame = 0, i, span;
  long windows = (long)frames - (long)runtime_frames;

  for (i = 0; (long)i < windows; i++) {
    span = frameoffsets[i + runtime_frames] - frameoffsets[i];
    if (span > best) {
      best = span;
      best_frame = i;
    }
  }
  if (!best) {
    best = bink_mul_div(frameoffsets[frames] - frameoffsets[0], runtime_frames, frames);
  }
  *all_keyframes = 1;
  for (i = 0; i < frames; i++) {
    if (!(frameoffsets[i] & 1)) {
      *all_keyframes = 0;
      break;
    }
  }
  *highest_frame = best_frame;
  return best;
}

static BINK *bink_from_io_callback(radcb_callback *callback)
{
  return (BINK *)((unsigned char *)callback - __builtin_offsetof(BINK, io.callback));
}

static BINK *bink_from_sound_callback(radcb_callback *callback)
{
  return (BINK *)((unsigned char *)callback - __builtin_offsetof(BINK, sound_callback));
}

/* 0x22e3f0. IO read-ahead priority: urgent under half full, otherwise once
 * per handler pass. */
unsigned long __stdcall FUN_0022e3f0(radcb_callback *callback, unsigned long iteration)
{
  BINK *bink = bink_from_io_callback(callback);
  unsigned long percent = (bink->io.CurBufUsed + 1) * 100 / (bink->io.CurBufSize + 1);

  if (percent < 50) {
    return 0xfffffffful - percent;
  }
  if (BINK_IO_ITERATION(bink) != iteration) {
    return 0x80000000ul - BINK_IO_ITERATION(bink);
  }
  return 0;
}

/* 0x22e440 */
unsigned long __stdcall FUN_0022e440(radcb_callback *callback, unsigned long iteration)
{
  BINK *bink = bink_from_io_callback(callback);

  bink->io.Idle(&bink->io);
  BINK_IO_ITERATION(bink) = iteration;
  return 0;
}

/* 0x22e4d0. Sound: once per handler pass. */
unsigned long __stdcall FUN_0022e4d0(radcb_callback *callback, unsigned long iteration)
{
  BINK *bink = bink_from_sound_callback(callback);
  unsigned long last = BINK_SOUND_ITERATION(bink);

  return last != iteration ? 0xfffffffful - last : 0;
}

/* 0x22e4f0 */
unsigned long __stdcall FUN_0022e4f0(radcb_callback *callback, unsigned long iteration)
{
  BINK *bink = bink_from_sound_callback(callback);

  if (bink->trackindex != -1) {
    FUN_0022e0c0(bink);
  }
  BINK_SOUND_ITERATION(bink) = iteration;
  return 0;
}

/* 0x22e460 / 0x22e480 / 0x22e4a0 / 0x22e4c0: BINKIO suspend hooks. */
void __stdcall FUN_0022e460(BINKIO *io)
{
  FUN_00232460(io_handler, &io->callback);
}

long __stdcall FUN_0022e480(BINKIO *io)
{
  return FUN_00232430(io_handler, &io->callback);
}

void __stdcall FUN_0022e4a0(BINKIO *io)
{
  FUN_002324f0(io_handler, &io->callback);
}

void __stdcall FUN_0022e4c0(BINKIO *io)
{
  (void)io;
  FUN_00232600();
}
