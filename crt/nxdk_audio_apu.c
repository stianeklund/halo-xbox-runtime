/* Optional nxdk-audio transport. The driver owns the APU and AC97; the
 * software XAudio worker must not run at the same time. */
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>
#include <nxaudio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_internal.h"
#include "../compat/audio/backend.h"
#include "xdk_hrtf.h"
#include "xdk_i3dl2.h"

struct apu_voice {
    nxAudioVoice hardware;
    nxAudioBuffer buffers[AUDIO_PACKET_COUNT];
    void *copies[AUDIO_PACKET_COUNT];
    unsigned int capacity[AUDIO_PACKET_COUNT];
    int owners[2]; /* hardware SSL slot -> Halo packet slot */
    struct audio_voice *voice;
    /* A stream's other hardware voice. After an async flush it is the voice
     * still stopping, which keeps its copies until the driver reports idle. */
    struct apu_voice *spare;
    unsigned int frequency;
    int stream, valid, stopping, buffer_attached;
    /* A mono DS_CTRL3D voice on a hardware 3D voice: its HRTF entry
     * double-buffer toggle and the last pair and Doppler ratio applied. */
    int spatial, toggle, hrtf_valid;
    nxAudioHRTFParams hrtf;
    float doppler;
};

static CRITICAL_SECTION audio_lock;
/* Auto-reset: completions that land before the worker runs share one poll. */
static KEVENT audio_event;
static volatile int initialized;
/* Ready, created voices, destroyed voices, queued hardware buffers,
 * completed hardware buffers, worker polls, stop timeouts, driver errors. */
volatile unsigned int halo_audio_apu_stats[8];
/* The last driver error's caller and nxdk-audio result, for XBDM. */
volatile unsigned int halo_audio_apu_last_failure[2];

void standalone_audio_lock(void) { EnterCriticalSection(&audio_lock); }
void standalone_audio_unlock(void) { LeaveCriticalSection(&audio_lock); }

static __attribute__((noinline)) int failed(void)
{
    halo_audio_apu_stats[7]++;
    halo_audio_apu_last_failure[0] = (unsigned int)__builtin_return_address(0);
    halo_audio_apu_last_failure[1] = nxAudioGetLastError();
    return 0;
}

static void buffer_consumed(nxAudioVoice *hardware, void *context)
{
    (void)hardware;
    (void)context;
    /* No allocation, FPU work, metadata mutation, or Halo callbacks here. */
    KeSetEvent(&audio_event, IO_NO_INCREMENT, FALSE);
}

static DWORD WINAPI apu_worker(void *context)
{
    /* Completions wake the worker. The timeout only backs up stop recovery:
     * GetStatus and DirectSoundDoWork poll for themselves. */
    LARGE_INTEGER timeout = {.QuadPart = -10 * 10000LL};
    (void)context;
    while (!initialized) Sleep(1);
    for (;;) {
        KeWaitForSingleObject(&audio_event, Executive, KernelMode, FALSE, &timeout);
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
    KeInitializeEvent(&audio_event, SynchronizationEvent, FALSE);
    if (!nxAudioInit(&parameters)) goto failed_lock;
    thread = CreateThread(NULL, 0x10000, apu_worker, NULL, 0, NULL);
    if (!thread) { nxAudioShutdown(); goto failed_lock; }
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
    CloseHandle(thread);
    halo_audio_apu_stats[0] = 1;
    initialized = 1;
    DbgPrint("[audio] nxdk-audio APU backend ready (experimental stereo)\n");
    return 1;
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

/* The frequency the game set, scaled by a 3D voice's Doppler ratio, within
 * DirectSound's pitch range (DSBFREQUENCY_MIN..MAX). */
static int set_pitch(struct apu_voice *h)
{
    float rate = (float)h->frequency * (h->spatial ? h->voice->doppler : 1.0f);
    rate = rate < 188.0f ? 188.0f : rate > 191999.0f ? 191999.0f : rate;
    h->doppler = h->voice->doppler;
    return nxAudioVoiceSetPitch(&h->hardware, rate / (float)h->voice->format.rate);
}

/* FUN_00207cc8: fistp(x - 0.5) under round-to-nearest, plus one if negative. */
static int xdk_ftol(float x)
{
    int i;
    x -= 0.5f;
    __asm__("fistpl %0" : "=m"(i) : "t"(x) : "st");
    return i < 0 ? i + 1 : i;
}

/* CSensaura3d_GetFullHRTFFilterPair (cachebeta 0x20a2f5) quantizes the
 * pseudo-angles to the filter grid. The rear fold applies only to surround
 * output, so it is omitted. Left of centre swaps the ears and negates ITD. */
static void hrtf_pair(float azimuth, float elevation, nxAudioHRTFParams *p)
{
    float magnitude = azimuth < 0.0f ? -azimuth : azimuth;
    int el = xdk_ftol(elevation < 0.0f ? elevation - 3.0f : elevation + 3.0f) / 6 * 6, az = 0, i;
    const unsigned char *left, *right;
    if (el < -90 || el > 90) el = 0;
    if (el != 90 && el != -90) {
        if (el > 60 || el < -60) az = xdk_ftol(magnitude + 6.0f) / 12 * 12;
        else if (el > 30 || el < -30) az = xdk_ftol(magnitude + 3.0f) / 6 * 6;
        else az = xdk_ftol(magnitude + 1.5f) / 3 * 3;
        if (az < 0 || az > 180) az = 0;
    }
    left = xdk_hrtf_filters[xdk_hrtf_index[az / 3][(el + 90) / 6]];
    right = left + 32;
    p->itd = left[31];
    if (azimuth < 0.0f) {
        right = left;
        left = right + 32;
        p->itd = (int16_t)-p->itd;
    }
    for (i = 0; i < 31; i++) {
        p->hrir_left[i] = (int8_t)left[i];
        p->hrir_right[i] = (int8_t)right[i];
    }
}

/* Retail writes each new pair to the voice's other HRTF entry (2v or
 * 2v + 1), then retargets the voice, so the VP never reads an entry
 * mid-update. An unchanged pair is not rewritten. */
static int set_hrtf(struct apu_voice *h)
{
    nxAudioHRTFParams p;
    unsigned int entry;
    KIRQL irql;
    hrtf_pair(h->voice->azimuth, h->voice->elevation, &p);
    if (h->hrtf_valid && !memcmp(&p, &h->hrtf, sizeof(p))) return 1;
    entry = h->hardware.voice_index * 2u + (unsigned int)h->toggle;
    irql = KeRaiseIrqlToDpcLevel();
    nxAudioHRTFSetParameters((uint8_t)entry, &p);
    KfLowerIrql(irql);
    if (!nxAudioVoiceSetHRTFTarget(&h->hardware, (uint16_t)entry)) return failed();
    h->toggle ^= 1;
    h->hrtf = p;
    h->hrtf_valid = 1;
    return 1;
}

static uint32_t attenuation(int level)
{
    uint32_t units = level < 0 ? ((uint32_t)-level << 6) / 100 : 0;
    return units > 0xfff ? 0xfff : units;
}

/* Retail's register image (FUN_00208000, CMcpxVoiceClient_SetFilter):
 * slots 0-4 carry the mB levels for bins 6-10, slots 5-7 are muted, and
 * the I3DL2 filter mode takes TAR_FCB. nxdk-audio's shadows stay current
 * for its own later writes. */
static int set_spatial(struct apu_voice *h)
{
    nxAudioVoice *hw = &h->hardware;
    const int *level = h->voice->level;
    uint32_t a[8], vola, volb, volc, misc;
    unsigned int i;
    KIRQL irql;
    for (i = 0; i < 8; i++) a[i] = i < 5 ? attenuation(level[i]) : 0xfff;
    vola = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME0, a[0]) | APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME1, a[1]) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME6_B3_0, a[6]) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLA_VOLUME7_B3_0, a[7]);
    volb = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME2, a[2]) | APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME3, a[3]) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME6_B7_4, a[6] >> 4) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLB_VOLUME7_B7_4, a[7] >> 4);
    volc = APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME4, a[4]) | APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME5, a[5]) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME6_B11_8, a[6] >> 8) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_TAR_VOLC_VOLUME7_B11_8, a[7] >> 8);
    misc = (hw->cfg_misc & ~(uint32_t)NV_PAVS_VOICE_CFG_MISC_FMODE) |
           APU_MAKE_VALUE(NV_PAVS_VOICE_CFG_MISC_FMODE, NX_FILTER_MODE_3D_I3DL2_REVERB);
    if (vola == hw->tar_vola && volb == hw->tar_volb && volc == hw->tar_volc && misc == hw->cfg_misc &&
        h->voice->filter == hw->tar_fcb)
        return 1;
    hw->tar_vola = vola;
    hw->tar_volb = volb;
    hw->tar_volc = volc;
    hw->cfg_misc = misc;
    hw->tar_fcb = h->voice->filter;
    irql = KeRaiseIrqlToDpcLevel();
    apu_wait_for_pio(9);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_CURRENT_VOICE, hw->voice_index);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 1);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLA, vola);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLB, volb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_VOLC, volc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_CFG_MISC, misc);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCA, hw->tar_fca);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_VOICE_TAR_FCB, hw->tar_fcb);
    apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_VOICE_LOCK, 0);
    KfLowerIrql(irql);
    return 1;
}

static int set_output(struct apu_voice *h)
{
    if (!h->spatial) return set_gain(h);
    return set_hrtf(h) && set_spatial(h) && (h->doppler == h->voice->doppler || set_pitch(h));
}

static int make_voice(struct apu_voice *h)
{
    const struct audio_format *f = &h->voice->format;
    nxAudioFormat format = {0};
    format.sample_rate = f->rate;
    format.channels = (uint8_t)f->channels;
    format.bytes_per_sample = (uint8_t)(f->bits / 8);
    format.codec = f->tag == 0x69 ? NX_AUDIO_CODEC_ADPCM : NX_AUDIO_CODEC_PCM;
    /* Retail plays a mono DS_CTRL3D voice on one of the 64 3D voices. When
     * those run out, the 2D voice takes the adapter's stereo approximation. */
    h->spatial = h->voice->is_3d && f->channels == 1;
    format.type = h->spatial ? (h->stream ? NX_VOICE_TYPE_3D_STREAM : NX_VOICE_TYPE_3D_STATIC)
                             : (h->stream ? NX_VOICE_TYPE_2D_STREAM : NX_VOICE_TYPE_2D_STATIC);
    if (!nxAudioVoiceCreate(&h->hardware, &format)) {
        if (!h->spatial) return failed();
        h->spatial = 0;
        format.type = h->stream ? NX_VOICE_TYPE_2D_STREAM : NX_VOICE_TYPE_2D_STATIC;
        if (!nxAudioVoiceCreate(&h->hardware, &format)) return failed();
    }
    h->valid = 1;
    h->stopping = h->buffer_attached = 0;
    h->owners[0] = h->owners[1] = -1;
    h->toggle = h->hrtf_valid = 0;
    halo_audio_apu_stats[1]++;
    nxAudioBufferSetCallback(&h->hardware, buffer_consumed, h);
    if (!set_pitch(h) || !set_output(h)) {
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

/* The driver marks STOPPED only after its DPC unlinks the idle voice.
 * Keep cache/staging ownership until that handshake finishes. */
static int wait_stopped(struct apu_voice *h)
{
    unsigned int attempts;
    for (attempts = 0; attempts < 150; attempts++) {
        if (nxAudioVoiceGetState(&h->hardware) == NX_STOPPED) return 1;
        Sleep(1);
    }
    halo_audio_apu_stats[6]++;
    DbgPrint("[audio] APU stop timed out; retaining DMA buffers\n");
    return 0;
}

static int stop_voice(struct apu_voice *h)
{
    if (!h->valid) return 1;
    h->stopping = 1;
    if (!nxAudioVoiceStop(&h->hardware)) return failed();
    return wait_stopped(h);
}

/* Destroy a voice that an async flush left stopping, once it is idle. */
static int reap_spare(struct apu_voice *h, int wait)
{
    struct apu_voice *s = h->spare;
    if (!s || !s->valid) return 1;
    if (wait ? !wait_stopped(s) : nxAudioVoiceGetState(&s->hardware) != NX_STOPPED) return 0;
    nxAudioVoiceDestroy(&s->hardware);
    halo_audio_apu_stats[2]++;
    s->valid = 0;
    return 1;
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
    struct apu_voice *h = v->backend;
    if (!stop_voice(h) || !reap_spare(h, 1)) return 0;
    finish_flush(v);
    return 1;
}

/* Retail DirectSound cuts a flushed stream and starts its next packet on
 * another hardware voice in the same frame. Waiting here for the stop
 * handshake left an audible gap between consecutive weapon shots. Hand the
 * stream to its spare voice instead, and let the poll reap the old one. */
int audio_output_flush_async(struct audio_voice *v)
{
    struct apu_voice *h = v->backend, *s;
    /* Halo flushes a channel twice per new sound. The second finds no
     * voice, and must not wait for the first to retire. */
    if (h->stream && !h->valid) {
        finish_flush(v);
        return 1;
    }
    if (!h->stream || h->stopping || !reap_spare(h, 0))
        return audio_output_flush(v);
    if (!h->spare) {
        if (!(s = calloc(1, sizeof(*s)))) return audio_output_flush(v);
        s->voice = v;
        s->stream = 1;
        h->spare = s;
        s->spare = h;
    }
    s = h->spare;
    h->stopping = 1;
    if (!nxAudioVoiceStop(&h->hardware)) return failed();
    if (nxAudioVoiceGetState(&h->hardware) == NX_STOPPED) {
        finish_flush(v);
        return 1;
    }
    h->owners[0] = h->owners[1] = -1;
    s->frequency = h->frequency;
    v->backend = s;
    audio_voice_flush(v);
    return 1;
}

int audio_output_ready(const struct audio_voice *v)
{
    const struct apu_voice *h = v->backend;
    return audio_voice_ready(v) && (!h->valid || !h->stopping);
}

void audio_output_destroy(struct audio_voice *v)
{
    struct apu_voice *h = v->backend, *voices[2];
    unsigned int i, j;
    if (!h) return;
    /* Release flushed synchronously, so the spare voice has stopped too. */
    voices[0] = h;
    voices[1] = h->spare;
    for (j = 0; j < 2; j++) {
        if (!(h = voices[j])) continue;
        if (h->valid) {
            nxAudioVoiceDestroy(&h->hardware);
            halo_audio_apu_stats[2]++;
        }
        for (i = 0; i < AUDIO_PACKET_COUNT; i++)
            if (h->copies[i]) MmFreeContiguousMemory(h->copies[i]);
        free(h);
    }
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

static int physically_contiguous(const unsigned char *data, unsigned int size)
{
    ULONG_PTR start = MmGetPhysicalAddress((PVOID)data);
    unsigned int offset = PAGE_SIZE - ((ULONG_PTR)data & (PAGE_SIZE - 1));
    for (; offset < size; offset += PAGE_SIZE)
        if (MmGetPhysicalAddress((PVOID)(data + offset)) != start + offset) return 0;
    return 1;
}

int audio_output_submit(struct audio_voice *v, const struct audio_packet *p)
{
    struct apu_voice *h = v->backend;
    unsigned int slot = v->write_packet;
    const void *data = p->data;
    void *copy;
    if (h->stopping && h->valid) return 0;
    if (!audio_voice_ready(v) || !audio_data_valid(&v->format, p->data, p->size)) return 0;
    if (!h->valid && !make_voice(h)) return 0;
    /* SSL ADPCM blocks cannot straddle discontiguous physical pages. Halo's
     * sound cache is one physical allocation, so its packets play in place,
     * as retail's do; anything else gets a contiguous copy per packet slot,
     * reused after its game callback. Copying every packet left too little
     * contiguous memory once the effects image took its scratch. */
    if (!physically_contiguous(p->data, p->size)) {
        if (h->capacity[slot] < p->size) {
            copy = MmAllocateContiguousMemory(p->size);
            if (!copy) return failed();
            if (h->copies[slot]) MmFreeContiguousMemory(h->copies[slot]);
            h->copies[slot] = copy;
            h->capacity[slot] = p->size;
        }
        memcpy(h->copies[slot], p->data, p->size);
        data = h->copies[slot];
    }
    if (!nxAudioBufferInitialize(&h->buffers[slot], data, p->size)) return failed();
    if (!audio_voice_submit(v, p)) return 0;
    pump(h);
    return 1;
}

void audio_output_poll(struct audio_voice *v)
{
    struct apu_voice *h = v->backend;
    if (!h) return;
    reap_spare(h, 0);
    if (!h->valid) return;
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
    return !h->valid || set_output(h) || failed();
}

/* nxdk-audio starts, like retail DirectSound, with submix headroom 1 on
 * bins 0-30; Halo's SetMixBinHeadroom(0x7fffffff, 0) clears it. Each step
 * halves a bin's input in the VP. Not voice-indexed, but keep the PIO
 * sequence whole relative to the completion DPC's own writes. */
int audio_output_mixbin_headroom(unsigned int mix_bins, unsigned int headroom)
{
    unsigned int bin;
    KIRQL irql;
    if (!initialized) return 0;
    irql = KeRaiseIrqlToDpcLevel();
    for (bin = 0; bin < 32; bin++) {
        if (!(mix_bins & (1u << bin))) continue;
        apu_wait_for_pio(1);
        apu_write_reg(APU_VP_OFFSET + NV1BA0_PIO_SET_SUBMIX_HEADROOM + bin * 4,
                      headroom & NV1BA0_PIO_SET_SUBMIX_HEADROOM_AMOUNT);
    }
    KfLowerIrql(irql);
    return 1;
}

/* DownloadEffectsImage (cachebeta 0x203a21, FUN_00207be4/FUN_00207963). The
 * image's first 0x800 bytes are retail's GP loader, which the patched
 * nxdk-audio GP program replaces. It keeps the loader's scratch layout: the
 * command block at 0x800, effect code from 0x818, then the effect state,
 * then the effect table. Effect scratch starts at 0xc000. */
#define EFFECTS_SCRATCH 0xc000u
#define EFFECTS_STATE_X 0x80u
#define EFFECTS_MAX 16u
#define GP_XMEM (APU_MMIO_BASE + APU_GP_OFFSET)
#define GP_YMEM (APU_MMIO_BASE + APU_GP_OFFSET + 0x6000u)

/* DSEFFECTMAP and DSEFFECTIMAGEDESC, as retail relocates them: code and
 * scratch as host pointers into scratch memory, state and Y memory as GP
 * MMIO addresses. */
struct effect_map {
    void *code;
    uint32_t code_size;
    void *state;
    uint32_t state_size;
    void *ymem;
    uint32_t ymem_size;
    void *scratch;
    uint32_t scratch_size;
};
static struct {
    uint32_t count, scratch_size;
    struct effect_map map[EFFECTS_MAX];
} effects;
static unsigned char *effects_scratch;
/* The I3DL2 reverb's state in scratch (a byte offset) and in GP X memory;
 * zero without one. state_address is the scratch offset loaded to X:$80. */
static uint32_t reverb_state, reverb_x, state_address;

/* The XDK's image cipher (0x20de7e, 0x20dfc2, 0x20e097): a 64-bit LFSR
 * keystream, a fixed key, and an 8-byte IV per block. */
struct lfsr {
    uint32_t lo, hi;
};

static void lfsr_step(struct lfsr *s)
{
    uint32_t lo = s->lo;
    s->lo = lo >> 1 | s->hi << 31;
    s->hi = (((((((lo << 2) ^ lo) << 1) ^ lo) << 28) ^ s->hi) & 0x80000000u) | s->hi >> 1;
}

static struct lfsr lfsr_key(void)
{
    struct lfsr s = {0x49de12ba, 0x7fa49bca};
    unsigned int i;
    for (i = 0; i < 81; i++) lfsr_step(&s);
    return s;
}

static void unscramble(const unsigned char iv[8], unsigned char *data, uint32_t size)
{
    struct lfsr s = lfsr_key();
    unsigned char key[8];
    uint32_t i;
    memcpy(key, &s.lo, 4);
    memcpy(key + 4, &s.hi, 4);
    lfsr_step(&s);
    for (i = 0; i < size; i++) {
        lfsr_step(&s);
        data[i] = (unsigned char)(key[i & 7] ^ s.lo ^ (data[i] - iv[i & 7] * key[i & 7]));
    }
}

/* The GP clears the flags once it has loaded what a command asked for. It
 * reads the block at the start of every 32-sample frame. */
static int gp_idle(nxAudioGPCommand *command)
{
    unsigned int attempts;
    for (attempts = 0; attempts < 100; attempts++) {
        if (!command->flags) return 1;
        Sleep(1);
    }
    DbgPrint("[audio] GP command timed out\n");
    return 0;
}

int audio_output_effects_image(const void *image, unsigned int size, const unsigned int *locations,
                               void **description)
{
    const unsigned char *in = image;
    unsigned char *scratch, *keys, key[8];
    nxAudioGPCommand *command;
    struct lfsr s;
    uint32_t header[6], table, count, total, raw[8], i;
    if (!initialized || size < NX_AUDIO_GP_SCRATCH_CODE + 8 || size > EFFECTS_SCRATCH) return 0;
    memcpy(header, in + NX_AUDIO_GP_SCRATCH_COMMAND, sizeof(header));
    /* Retail's relocations assume the state follows the code, from X:$80. */
    if (header[0] || header[1] >= size || header[3] >= size ||
        header[2] != NX_AUDIO_GP_SCRATCH_CODE + header[1] * 4)
        return 0;
    table = header[2] + header[3] * 4;
    if (table + 8 > size) return 0;
    memcpy(&count, in + table, 4);
    memcpy(&total, in + table + 4, 4);
    if (!count || count > EFFECTS_MAX || table + 8 + count * 40 > size || total > 0x200000) return 0;
    for (i = 0; i < count; i++) {
        memcpy(raw, in + table + 8 + i * 32, sizeof(raw));
        if (raw[0] < NX_AUDIO_GP_SCRATCH_CODE || raw[0] > header[2] || raw[1] > header[2] - raw[0] ||
            raw[2] & 3 || raw[2] < header[2] || raw[2] > table || raw[3] > table - raw[2] ||
            raw[6] > EFFECTS_SCRATCH + total || raw[7] > EFFECTS_SCRATCH + total - raw[6])
            return 0;
    }

    if (!(scratch = nxAudioGPScratch(EFFECTS_SCRATCH + total))) return failed();
    command = (nxAudioGPCommand *)(scratch + NX_AUDIO_GP_SCRATCH_COMMAND);
    if (!gp_idle(command)) return failed();
    memcpy(scratch + NX_AUDIO_GP_SCRATCH_CODE, in + NX_AUDIO_GP_SCRATCH_CODE, size - NX_AUDIO_GP_SCRATCH_CODE);
    /* The per-effect IVs are themselves scrambled, with the key as the IV. */
    keys = scratch + table + 8 + count * 32;
    s = lfsr_key();
    memcpy(key, &s.lo, 4);
    memcpy(key + 4, &s.hi, 4);
    unscramble(key, keys, count * 8);
    effects.count = count;
    effects.scratch_size = total;
    for (i = 0; i < count; i++) {
        struct effect_map *m = &effects.map[i];
        memcpy(raw, in + table + 8 + i * 32, sizeof(raw));
        unscramble(keys + i * 8, scratch + raw[0], raw[1]);
        m->code = scratch + raw[0];
        m->code_size = raw[1];
        m->state = (void *)(GP_XMEM + (EFFECTS_STATE_X + (raw[2] - header[2]) / 4) * 4);
        m->state_size = raw[3];
        m->ymem = (void *)(GP_YMEM + raw[4]);
        m->ymem_size = raw[5];
        m->scratch = scratch + raw[6];
        m->scratch_size = raw[7];
    }
    effects_scratch = scratch;
    state_address = header[2];
    reverb_state = reverb_x = 0;
    if (locations && locations[0] < count) {
        memcpy(raw, in + table + 8 + locations[0] * 32, sizeof(raw));
        reverb_state = raw[2];
        reverb_x = EFFECTS_STATE_X + (raw[2] - header[2]) / 4;
    }
    if (description) *description = &effects;

    command->state_offset = header[0];
    command->code_words = header[1];
    command->state_address = header[2];
    command->state_words = header[3];
    command->reserved = header[5];
    __asm__ volatile("" ::: "memory");
    command->flags = NX_AUDIO_GP_LOAD_CODE | NX_AUDIO_GP_LOAD_STATE;
    if (!gp_idle(command)) return failed();
    DbgPrint("[audio] GP loaded %u effects, %u bytes of scratch\n", count, total);
    return 1;
}

/* CMcpxAPU_Commit3dSettings (0x206a5e) with a new I3DL2 listener: read the
 * reverb's live parameters, rebuild them, flag the change (bit 2 of the word
 * at 0x10) and reload words 0x10-0x21f from scratch. The scratch copy's words
 * 0x14-0x117 still hold the image's values, which the reload restores. */
int audio_output_reverb(const int listener[12])
{
    unsigned char state[0x220];
    nxAudioGPCommand *command;
    uint32_t i, word;
    if (!initialized || !reverb_state) return 1;
    command = (nxAudioGPCommand *)(effects_scratch + NX_AUDIO_GP_SCRATCH_COMMAND);
    if (!gp_idle(command)) return failed();
    for (i = 0; i < 0x118 / 4; i++) {
        word = apu_read_reg(APU_GP_OFFSET + (reverb_x + i) * 4);
        memcpy(state + i * 4, &word, 4);
    }
    xdk_i3dl2_reverb_parameters((const int32_t *)listener, state);
    memcpy(&word, state + 0x10, 4);
    word |= 4;
    memcpy(effects_scratch + reverb_state + 0x10, &word, 4);
    memcpy(effects_scratch + reverb_state + 0x118, state + 0x118, 0x108);
    command->state_offset = (reverb_state + 0x10 - state_address) / 4;
    command->state_address = reverb_state + 0x10;
    command->state_words = (0x220 - 0x10) / 4;
    __asm__ volatile("" ::: "memory");
    command->flags = NX_AUDIO_GP_LOAD_STATE;
    return 1;
}

int audio_output_frequency(struct audio_voice *v, unsigned int frequency)
{
    struct apu_voice *h = v->backend;
    h->frequency = frequency;
    return !h->valid || set_pitch(h) || failed();
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
