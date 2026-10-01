/* Optional nxdk-audio transport. The driver owns the APU and AC97; the
 * software XAudio worker must not run at the same time. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxaudio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_internal.h"
#include "../compat/audio/backend.h"

struct apu_voice {
    nxAudioVoice hardware;
    nxAudioBuffer buffers[AUDIO_PACKET_COUNT];
    void *copies[AUDIO_PACKET_COUNT];
    unsigned int capacity[AUDIO_PACKET_COUNT];
    int owners[2]; /* hardware SSL slot -> Halo packet slot */
    struct audio_voice *voice;
    unsigned int frequency;
    int stream, valid, stopping, buffer_attached;
};

static CRITICAL_SECTION audio_lock;
static HANDLE audio_semaphore;
static volatile int initialized;
/* Ready, created voices, destroyed voices, queued hardware buffers,
 * completed hardware buffers, worker polls, stop timeouts, driver errors. */
volatile unsigned int halo_audio_apu_stats[8];

void standalone_audio_lock(void) { EnterCriticalSection(&audio_lock); }
void standalone_audio_unlock(void) { LeaveCriticalSection(&audio_lock); }

static int failed(void)
{
    halo_audio_apu_stats[7]++;
    return 0;
}

static void buffer_consumed(nxAudioVoice *hardware, void *context)
{
    (void)hardware;
    (void)context;
    /* No allocation, FPU work, metadata mutation, or Halo callbacks here. */
    ReleaseSemaphore(audio_semaphore, 1, NULL);
}

static DWORD WINAPI apu_worker(void *context)
{
    (void)context;
    while (!initialized) Sleep(1);
    for (;;) {
        if (WaitForSingleObject(audio_semaphore, 1) == WAIT_FAILED) Sleep(1);
        standalone_audio_lock();
        standalone_audio_poll();
        halo_audio_apu_stats[5]++;
        standalone_audio_unlock();
    }
    return 0;
}

int standalone_audio_init(void)
{
    nxAudioInitParams parameters = {0};
    HANDLE thread;
    if (initialized) return 1;
    InitializeCriticalSection(&audio_lock);
    audio_semaphore = CreateSemaphore(NULL, 0, 1024, NULL);
    if (!audio_semaphore) goto failed_lock;
    if (!nxAudioInit(&parameters)) goto failed_semaphore;
    thread = CreateThread(NULL, 0x10000, apu_worker, NULL, 0, NULL);
    if (!thread) { nxAudioShutdown(); goto failed_semaphore; }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(thread);
    halo_audio_apu_stats[0] = 1;
    initialized = 1;
    DbgPrint("[audio] nxdk-audio APU backend ready (experimental stereo)\n");
    return 1;
failed_semaphore:
    CloseHandle(audio_semaphore);
    audio_semaphore = NULL;
failed_lock:
    DeleteCriticalSection(&audio_lock);
    return failed();
}

/* Master gain stays at the driver's unity default; the adapter's combined
 * volume/headroom/3D level and pan go to the front bins. A stereo voice maps its
 * left and right samples to those bins. */
static int set_gain(struct apu_voice *h)
{
    return nxAudioVoiceSetChannelGain(&h->hardware, (float)h->voice->gain_left / 32768.0f,
                                      (float)h->voice->gain_right / 32768.0f, 0.0f, 0.0f, 0.0f, 0.0f);
}

static int make_voice(struct apu_voice *h)
{
    const struct audio_format *f = &h->voice->format;
    nxAudioFormat format = {0};
    format.sample_rate = f->rate;
    format.channels = (uint8_t)f->channels;
    format.bytes_per_sample = (uint8_t)(f->bits / 8);
    format.codec = f->tag == 0x69 ? NX_AUDIO_CODEC_ADPCM : NX_AUDIO_CODEC_PCM;
    /* Spatial calculation is a subsequent milestone. This first backend
     * uses the driver's 192 2D voice slots and keeps existing stereo routing. */
    format.type = h->stream ? NX_VOICE_TYPE_2D_STREAM : NX_VOICE_TYPE_2D_STATIC;
    if (!nxAudioVoiceCreate(&h->hardware, &format)) return failed();
    h->valid = 1;
    h->stopping = h->buffer_attached = 0;
    h->owners[0] = h->owners[1] = -1;
    halo_audio_apu_stats[1]++;
    nxAudioBufferSetCallback(&h->hardware, buffer_consumed, h);
    if (!set_gain(h) ||
        !nxAudioVoiceSetPitch(&h->hardware, (float)h->frequency / (float)f->rate)) {
        nxAudioVoiceDestroy(&h->hardware);
        h->valid = 0;
        halo_audio_apu_stats[2]++;
        return failed();
    }
    return 1;
}

int audio_output_create(struct audio_voice *v, int stream)
{
    struct apu_voice *h = calloc(1, sizeof(*h));
    if (!h) return 0;
    h->voice = v;
    h->stream = stream;
    h->frequency = v->format.rate;
    if (!make_voice(h)) { free(h); return 0; }
    v->backend = h;
    return 1;
}

static int stop_voice(struct apu_voice *h)
{
    unsigned int attempts;
    if (!h->valid) return 1;
    h->stopping = 1;
    if (!nxAudioVoiceStop(&h->hardware)) return failed();
    /* The driver marks STOPPED only after its DPC unlinks the idle voice.
     * Keep cache/staging ownership until that handshake finishes. */
    for (attempts = 0; attempts < 150; attempts++) {
        if (nxAudioVoiceGetState(&h->hardware) == NX_STOPPED) return 1;
        Sleep(1);
    }
    halo_audio_apu_stats[6]++;
    DbgPrint("[audio] APU stop timed out; retaining DMA buffers\n");
    return 0;
}

static void finish_flush(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    if (h->valid) {
        nxAudioVoiceDestroy(&h->hardware);
        halo_audio_apu_stats[2]++;
        h->valid = 0;
    }
    h->owners[0] = h->owners[1] = -1;
    h->buffer_attached = 0;
    audio_voice_flush(v);
    /* Recreate on the next submission/play. A stopped channel need not own
     * an APU slot, and the old driver descriptors no longer pin cache pages. */
}

int audio_output_flush(struct audio_voice *v)
{
    if (!stop_voice(v->backend)) return 0;
    finish_flush(v);
    return 1;
}

int audio_output_ready(const struct audio_voice *v)
{
    const struct apu_voice *h = v->backend;
    return audio_voice_ready(v) && (!h->valid || !h->stopping);
}

void audio_output_destroy(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    unsigned int i;
    if (!h) return;
    if (h->valid) {
        nxAudioVoiceDestroy(&h->hardware);
        halo_audio_apu_stats[2]++;
    }
    for (i = 0; i < AUDIO_PACKET_COUNT; i++)
        if (h->copies[i]) MmFreeContiguousMemory(h->copies[i]);
    free(h);
    v->backend = NULL;
}

static int owned(const struct apu_voice *h, unsigned int packet)
{
    return h->owners[0] == (int)packet || h->owners[1] == (int)packet;
}

static void pump(struct apu_voice *h)
{
    struct audio_voice *v = h->voice;
    unsigned int i, slot;
    KIRQL irql;
    if (!h->valid) return;
    /* Retire and assign SSL ownership atomically relative to the driver's
     * DPC. It clears hardware slots before waking us; reusing one without
     * retiring its old owner would lose that packet's game callback. */
    irql = KeRaiseIrqlToDpcLevel();
    for (slot = 0; slot < 2; slot++) {
        int packet = h->owners[slot];
        if (packet >= 0 && h->hardware.buffers_hardware[slot] == NULL) {
            struct audio_packet *p = &v->packets[packet];
            p->result = h->stopping ? AUDIO_ABORTED : 0;
            p->cursor = h->stopping ? 0 : p->frames;
            p->state = 2;
            h->owners[slot] = -1;
            halo_audio_apu_stats[4]++;
        }
    }
    if (h->stopping) { KfLowerIrql(irql); return; }
    for (i = 0; i < AUDIO_PACKET_COUNT; i++) {
        unsigned int packet = (v->read_packet + i) % AUDIO_PACKET_COUNT;
        if (v->packets[packet].state != 1 || owned(h, packet)) continue;
        /* Preserve the driver's A/B FIFO order even if the other slot is
         * free. A busy next slot means wait, not abort this Halo packet. */
        slot = h->hardware.next_ssl_index;
        if (h->owners[slot] >= 0) break;
        if (!nxAudioBufferQueue(&h->hardware, &h->buffers[packet])) {
            v->packets[packet].result = AUDIO_ABORTED;
            v->packets[packet].state = 2;
            failed();
            continue;
        }
        /* The driver advances its next SSL only after successful queueing. */
        for (slot = 0; slot < 2; slot++)
            if (h->hardware.buffers_hardware[slot] == &h->buffers[packet]) break;
        if (slot < 2) h->owners[slot] = (int)packet;
        halo_audio_apu_stats[3]++;
    }
    KfLowerIrql(irql);
    if (!v->paused && (h->hardware.buffers_hardware[0] || h->hardware.buffers_hardware[1]) &&
        nxAudioVoiceGetState(&h->hardware) != NX_PLAYING)
        if (!nxAudioVoiceStart(&h->hardware)) failed();
}

int audio_output_submit(struct audio_voice *v, const struct audio_packet *p)
{
    struct apu_voice *h = v->backend;
    unsigned int slot = v->write_packet;
    void *copy;
    if (h->stopping && h->valid) return 0;
    if (!audio_voice_ready(v) || !audio_data_valid(&v->format, p->data, p->size)) return 0;
    if (!h->valid && !make_voice(h)) return 0;
    if (h->capacity[slot] < p->size) {
        copy = MmAllocateContiguousMemory(p->size);
        if (!copy) return failed();
        if (h->copies[slot]) MmFreeContiguousMemory(h->copies[slot]);
        h->copies[slot] = copy;
        h->capacity[slot] = p->size;
    }
    /* SSL ADPCM blocks cannot straddle discontiguous physical pages. Keep
     * a contiguous copy per packet slot, reused after its game callback. */
    memcpy(h->copies[slot], p->data, p->size);
    if (!nxAudioBufferInitialize(&h->buffers[slot], h->copies[slot], p->size)) return failed();
    if (!audio_voice_submit(v, p)) return 0;
    pump(h);
    return 1;
}

void audio_output_poll(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    if (!h || !h->valid) return;
    /* Flush can time out before the idle DPC finishes. Halo waits for the
     * active bit to clear before retrying Flush. Retire every remaining
     * packet once DMA ownership ends, including packets never queued to
     * hardware. Game callbacks still run only from DirectSoundDoWork. */
    if (h->stopping && nxAudioVoiceGetState(&h->hardware) == NX_STOPPED) {
        finish_flush(v);
        return;
    }
    if (!h->stream) {
        if (nxAudioVoiceGetState(&h->hardware) == NX_STOPPED) v->playing = 0;
        return;
    }
    pump(h);
}

int audio_output_volume(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    return !h->valid || set_gain(h) || failed();
}

int audio_output_frequency(struct audio_voice *v, unsigned int frequency)
{
    struct apu_voice *h = v->backend;
    h->frequency = frequency;
    return !h->valid || nxAudioVoiceSetPitch(&h->hardware, (float)frequency / (float)v->format.rate) || failed();
}

int audio_output_pause(struct audio_voice *v, int pause)
{
    struct apu_voice *h = v->backend;
    if (!h->valid) return 1;
    if (pause) return nxAudioVoicePause(&h->hardware) || failed();
    if (!h->hardware.buffers_hardware[0] && !h->hardware.buffers_hardware[1]) return 1;
    return nxAudioVoiceStart(&h->hardware) || failed();
}

/* LBO and EBO count samples (frames) for PCM and ADPCM alike, as the
 * patched driver's CBO and SSL lengths do. */
int audio_output_buffer_loop(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    unsigned int scale = v->format.tag == 0x69 ? 64 : 1;
    unsigned int count = v->buffer_size / v->format.block_align * scale;
    unsigned int end = v->looping && v->loop_end ? v->loop_end : count;
    if (!h->valid || !h->buffer_attached) return 1;
    if (!end) return 0;
    KIRQL irql = KeRaiseIrqlToDpcLevel();
    apu_wait_for_pio(5);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, h->hardware.voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_BUF_LBO, v->loop_start);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_BUF_EBO, end - 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    KfLowerIrql(irql);
    return 1;
}

int audio_output_buffer_seek(struct audio_voice *v, unsigned int position)
{
    struct apu_voice *h = v->backend;
    if (!h->valid || !h->buffer_attached) return 1;
    /* A seek exactly to the end is a valid silent position. */
    if (position == v->buffer_size) return audio_output_flush(v);
    return nxAudioVoiceSetPlaybackOffset(&h->hardware, position) || failed();
}

int audio_output_buffer_play(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    unsigned int scale = v->format.tag == 0x69 ? 64 : 1;
    unsigned int position = v->buffer_cursor / scale * v->format.block_align;
    if (!h->valid && !make_voice(h)) return 0;
    if (nxAudioVoiceGetState(&h->hardware) == NX_PLAYING)
        return (nxAudioVoiceSetLooping(&h->hardware, v->looping) && audio_output_buffer_loop(v)) || failed();
    if (position >= v->buffer_size) { v->playing = 0; return 1; }
    if (!nxAudioBufferInitialize(&h->buffers[0], v->buffer, v->buffer_size) ||
        !nxAudioBufferSubmit(&h->hardware, &h->buffers[0])) return failed();
    h->buffer_attached = 1;
    if (!nxAudioVoiceSetLooping(&h->hardware, v->looping) || !audio_output_buffer_loop(v) ||
        !nxAudioVoiceSetPlaybackOffset(&h->hardware, position)) return failed();
    return v->paused || nxAudioVoiceStart(&h->hardware) || failed();
}
