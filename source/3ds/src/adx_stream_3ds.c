/**
 * adx_stream_3ds.c — CRI ADX music streamed to ndsp channel 0.
 *
 * The shared decoder (sdl/src/sound/adx.c) turns the file into interleaved
 * s16 PCM; a decoder thread keeps three wave buffers ahead of the DSP. The
 * thread is woken by the ndsp frame callback and polls anyway every 20 ms so
 * a missed signal can never starve the channel.
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adx_stream_3ds.h"
#include "sound/adx.h"

#define MUSIC_CHANNEL   0
#define NUM_BUFS        3
#define BUF_FRAMES      8192          /* stereo frames per buffer, ~186 ms at 44.1 kHz */
#define BUF_BYTES       (BUF_FRAMES * 2 * 2)

enum { ST_NULL = 0, ST_READY, ST_PLAYING, ST_PAUSED, ST_FINISHED };

static int         s_inited = 0;
static AdxDecoder  s_dec;
static int         s_decOpen = 0;
static int         s_loop = 0;
static volatile int s_state = ST_NULL;
static int         s_vol = 255;
static ndspWaveBuf s_wb[NUM_BUFS];
static u8         *s_pcm[NUM_BUFS];
static LightLock   s_lock;
static LightEvent  s_event;
static Thread      s_thread = NULL;
static volatile int s_quit = 0;
static int         s_eof = 0;   /* decoder hit the end and we are not looping */

static void ndsp_cb(void *arg)
{
    (void)arg;
    LightEvent_Signal(&s_event);
}

static void apply_volume(void)
{
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = (float)s_vol / 255.0f;
    ndspChnSetMix(MUSIC_CHANNEL, mix);
}

/* Fill one wave buffer. Returns 1 if queued, 0 at end of stream. Called with
 * the lock held. */
static int refill(int i)
{
    size_t got = Adx_Read(&s_dec, s_pcm[i], BUF_BYTES);
    if (got == 0) {
        if (!s_loop) {
            return 0;
        }
        Adx_Rewind(&s_dec);
        got = Adx_Read(&s_dec, s_pcm[i], BUF_BYTES);
        if (got == 0) {
            return 0;
        }
    }
    int bytesPerFrame = 2 * s_dec.channels;
    s_wb[i].data_vaddr = s_pcm[i];
    s_wb[i].nsamples   = (u32)(got / bytesPerFrame);
    s_wb[i].looping    = false;
    s_wb[i].status     = NDSP_WBUF_FREE;
    DSP_FlushDataCache(s_pcm[i], got);
    ndspChnWaveBufAdd(MUSIC_CHANNEL, &s_wb[i]);
    return 1;
}

static void decoder_thread(void *arg)
{
    (void)arg;
    while (!s_quit) {
        LightEvent_WaitTimeout(&s_event, 20 * 1000000LL);
        LightLock_Lock(&s_lock);
        if (s_state == ST_PLAYING && s_decOpen) {
            for (int i = 0; i < NUM_BUFS; i++) {
                if (s_wb[i].status == NDSP_WBUF_DONE || s_wb[i].status == NDSP_WBUF_FREE) {
                    if (s_eof) {
                        continue;
                    }
                    if (!refill(i)) {
                        s_eof = 1;
                    }
                }
            }
            if (s_eof) {
                int busy = 0;
                for (int i = 0; i < NUM_BUFS; i++) {
                    if (s_wb[i].status == NDSP_WBUF_QUEUED || s_wb[i].status == NDSP_WBUF_PLAYING) {
                        busy = 1;
                    }
                }
                if (!busy) {
                    s_state = ST_FINISHED;
                }
            }
        }
        LightLock_Unlock(&s_lock);
    }
}

int adx_stream_init(void)
{
    if (s_inited) {
        return 1;
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        s_pcm[i] = (u8 *)linearAlloc(BUF_BYTES);
        if (s_pcm[i] == NULL) {
            fprintf(stderr, "adx: linearAlloc failed\n");
            return 0;
        }
        memset(&s_wb[i], 0, sizeof(s_wb[i]));
    }
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_event, RESET_ONESHOT);
    ndspSetCallback(ndsp_cb, NULL);

    /* Decoder core: 2 on a New 3DS (needs Luma's extended exheader, which it
     * grants 3dsx apps), else the system core after handing it 30% of our
     * time, else wherever the kernel puts it. */
    s_quit = 0;
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    if (isNew) {
        s_thread = threadCreate(decoder_thread, NULL, 32 * 1024, prio - 1, 2, false);
    }
    if (s_thread == NULL) {
        APT_SetAppCpuTimeLimit(30);
        s_thread = threadCreate(decoder_thread, NULL, 32 * 1024, prio - 1, 1, false);
    }
    if (s_thread == NULL) {
        s_thread = threadCreate(decoder_thread, NULL, 32 * 1024, prio - 1, -2, false);
    }
    if (s_thread == NULL) {
        fprintf(stderr, "adx: threadCreate failed; music refilled from the frame loop\n");
    }
    s_state = ST_NULL;
    s_inited = 1;
    return 1;
}

void adx_stream_shutdown(void)
{
    if (!s_inited) {
        return;
    }
    adx_stream_stop();
    adx_stream_destroy();
    s_quit = 1;
    LightEvent_Signal(&s_event);
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
    ndspSetCallback(NULL, NULL);
    for (int i = 0; i < NUM_BUFS; i++) {
        if (s_pcm[i]) {
            linearFree(s_pcm[i]);
            s_pcm[i] = NULL;
        }
    }
    s_inited = 0;
}

void adx_stream_destroy(void)
{
    if (!s_inited) {
        return;
    }
    LightLock_Lock(&s_lock);
    ndspChnWaveBufClear(MUSIC_CHANNEL);
    if (s_decOpen) {
        Adx_Close(&s_dec);
        s_decOpen = 0;
    }
    s_state = ST_NULL;
    LightLock_Unlock(&s_lock);
}

int adx_stream_create(const char *path, int loop)
{
    if (!s_inited || path == NULL) {
        return -1;
    }
    adx_stream_destroy();
    LightLock_Lock(&s_lock);
    if (Adx_Open(&s_dec, path) != 0) {
        LightLock_Unlock(&s_lock);
        fprintf(stderr, "adx: open %s failed\n", path);
        return -1;
    }
    s_decOpen = 1;
    s_loop = loop;
    s_eof = 0;
    for (int i = 0; i < NUM_BUFS; i++) {
        memset(&s_wb[i], 0, sizeof(s_wb[i]));
    }
    ndspChnReset(MUSIC_CHANNEL);
    ndspChnSetInterp(MUSIC_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(MUSIC_CHANNEL, (float)s_dec.sampleRate);
    ndspChnSetFormat(MUSIC_CHANNEL, s_dec.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    apply_volume();
    s_state = ST_READY;
    LightLock_Unlock(&s_lock);
    fprintf(stderr, "adx: %s (%d Hz, %d ch, loop %d)\n", path, s_dec.sampleRate, s_dec.channels, loop);
    return 0;
}

void adx_stream_play(void)
{
    if (!s_inited || !s_decOpen) {
        return;
    }
    LightLock_Lock(&s_lock);
    if (s_state == ST_PAUSED) {
        ndspChnSetPaused(MUSIC_CHANNEL, false);
        s_state = ST_PLAYING;
    }
    else if (s_state == ST_READY) {
        s_state = ST_PLAYING;
        /* Prime the queue right away so the first frames don't wait 20 ms. */
        for (int i = 0; i < NUM_BUFS; i++) {
            if (!refill(i)) {
                s_eof = 1;
                break;
            }
        }
    }
    LightLock_Unlock(&s_lock);
    LightEvent_Signal(&s_event);
}

void adx_stream_pause(void)
{
    if (!s_inited) {
        return;
    }
    LightLock_Lock(&s_lock);
    if (s_state == ST_PLAYING) {
        ndspChnSetPaused(MUSIC_CHANNEL, true);
        s_state = ST_PAUSED;
    }
    LightLock_Unlock(&s_lock);
}

void adx_stream_stop(void)
{
    if (!s_inited) {
        return;
    }
    LightLock_Lock(&s_lock);
    ndspChnWaveBufClear(MUSIC_CHANNEL);
    ndspChnSetPaused(MUSIC_CHANNEL, false);
    if (s_decOpen) {
        Adx_Rewind(&s_dec);
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        s_wb[i].status = NDSP_WBUF_FREE;
    }
    s_eof = 0;
    if (s_state != ST_NULL) {
        s_state = ST_READY;
    }
    LightLock_Unlock(&s_lock);
}

void adx_stream_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    s_vol = vol;
    if (s_inited) {
        apply_volume();
    }
}

int adx_stream_is_playing(void)
{
    return s_state == ST_PLAYING || s_state == ST_PAUSED;
}
