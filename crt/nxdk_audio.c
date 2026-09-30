/* nxdk HAL transport. The DPC only signals; mixing runs on an FPU-safe thread.
 * Keep nxdk headers/ABI isolated from Halo's force-included XDK declarations. */
#include <windows.h>
#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>
#include "../compat/audio/mixer.h"

#define AUDIO_FRAMES 1024
#define AUDIO_BUFFERS 3
static CRITICAL_SECTION audio_lock;
static HANDLE audio_semaphore;
static short *audio_buffers[AUDIO_BUFFERS];
static volatile int initialized;
/* Read these ordinary RAM counters through XBDM; never getmem the MMIO range.
 * IRQs, rendered buffers, queued buffers, PCM CIV/LVI/SR, S/PDIF CIV/LVI/SR,
 * underruns, retired PCM descriptors, failed waits. */
volatile unsigned int halo_audio_transport_stats[8];

void standalone_audio_lock(void) { EnterCriticalSection(&audio_lock); }
void standalone_audio_unlock(void) { LeaveCriticalSection(&audio_lock); }

static void audio_consumed(void *device, void *context)
{
  (void)device;
  (void)context;
  halo_audio_transport_stats[0]++;
  ReleaseSemaphore(audio_semaphore, 1, NULL);
}

static DWORD WINAPI audio_worker(void *context)
{
  unsigned int next = 0;
  unsigned int oldest = 0, queued = AUDIO_BUFFERS - 1;
  unsigned int registers, retired, i;
  volatile const unsigned int *ac97 = (volatile const unsigned int *)0xfec00000;
  (void)context;
  while (!initialized) Sleep(1);
  for (;;) {
    /* nxdk's DPC waits for BOTH analog and S/PDIF completion. xemu currently
     * consumes only analog DMA, so that signal never arrives. Use it as a
     * wake hint with a bounded wait, and retire buffers from PCM progress.
     * This also handles two IRQs coalescing into one DPC. */
    if (WaitForSingleObject(audio_semaphore, 1) == WAIT_FAILED) {
      halo_audio_transport_stats[7]++;
      Sleep(1);
    }
    registers = ac97[0x114 / 4];
    halo_audio_transport_stats[3] = registers;
    halo_audio_transport_stats[4] = ac97[0x174 / 4];
    retired = audio_dma_retired(registers, oldest, queued);
    if (!retired) continue;
    if (registers & 0x10000) halo_audio_transport_stats[5]++;
    halo_audio_transport_stats[6] += retired;
    oldest = (oldest + retired) & 31;
    queued -= retired;
    for (i = 0; i < retired; i++) {
      standalone_audio_lock();
      standalone_audio_render(audio_buffers[next], AUDIO_FRAMES);
      standalone_audio_unlock();
      halo_audio_transport_stats[1]++;
      XAudioProvideSamples((unsigned char *)audio_buffers[next], AUDIO_FRAMES * 4, 0);
      halo_audio_transport_stats[2]++;
      queued++;
      next = (next + 1) % AUDIO_BUFFERS;
    }
  }
  return 0;
}

int standalone_audio_init(void)
{
  unsigned int i, j;
  HANDLE thread;
  if (initialized) return 1;
  InitializeCriticalSection(&audio_lock);
  audio_semaphore = CreateSemaphore(NULL, 0, AUDIO_BUFFERS, NULL);
  if (!audio_semaphore) { DeleteCriticalSection(&audio_lock); return 0; }
  for (i = 0; i < AUDIO_BUFFERS; i++) {
    audio_buffers[i] = MmAllocateContiguousMemory(AUDIO_FRAMES * 4);
    if (!audio_buffers[i]) goto failed;
    for (j = 0; j < AUDIO_FRAMES * 2; j++) audio_buffers[i][j] = 0;
  }
  /* Worker waits for initialization before reading device registers. */
  thread = CreateThread(NULL, 0x10000, audio_worker, NULL, 0, NULL);
  if (!thread) goto failed;
  SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
  CloseHandle(thread);
  XAudioInit(16, 2, audio_consumed, NULL);
  /* Like nxdk's SDL backend, leave buffer 0 free and prime 1 and 2. */
  for (i = 1; i < AUDIO_BUFFERS; i++)
    XAudioProvideSamples((unsigned char *)audio_buffers[i], AUDIO_FRAMES * 4, 0);
  halo_audio_transport_stats[2] = AUDIO_BUFFERS - 1;
  XAudioPlay();
  initialized = 1;
  return 1;
failed:
  for (i = 0; i < AUDIO_BUFFERS; i++) {
    if (audio_buffers[i]) MmFreeContiguousMemory(audio_buffers[i]);
    audio_buffers[i] = NULL;
  }
  CloseHandle(audio_semaphore);
  audio_semaphore = NULL;
  DeleteCriticalSection(&audio_lock);
  return 0;
}
