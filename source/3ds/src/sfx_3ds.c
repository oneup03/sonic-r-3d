/**
 * sfx_3ds.c — sound effects on ndsp.
 *
 * 3DS counterpart to sdl/src/sound/sfx_sdl.c and dc/src/sfx_dc.c: same slot
 * table and dispatch, PC WAVs from SOUND/SFX decoded into linear memory once,
 * played on ndsp channels 2..23 (0 and 1 belong to the music streamer).
 *
 * Looping slots (engine, water) pin a channel while they loop and have their
 * pitch/volume updated in place; one-shots take any idle channel, stealing
 * the oldest voice when all are busy.
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fileio.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "sonicr_paths.h"
#include "sound/replay_voice.h"
#include "adx_stream_3ds.h"

extern void *g_soundBuffers[64];
extern int   g_soundActive[64];
extern int   g_initFeatureB;
extern int   g_volumeBase;
extern int   g_demoMode;
extern void  SetAllSoundVolumes(void);

#define SFX_MAX_SLOTS   64
#define SFX_FIRST_CHAN  2
#define SFX_NUM_CHAN    22

typedef struct {
    int16_t *pcm;        /* linearAlloc'd, s16 (mono or interleaved stereo) */
    u32      frames;     /* frames per channel */
    int      channels;
    int      rate;
    int      durationMs;
    ndspWaveBuf wb;
} SfxClip;

static SfxClip s_clip[SFX_MAX_SLOTS];
static int  s_ready = 0;
static int  s_ndspOk = 0;

/* channel bookkeeping */
static int  s_chanSlot[SFX_NUM_CHAN];     /* slot playing on channel, -1 free */
static u32  s_chanSerial[SFX_NUM_CHAN];   /* for oldest-voice stealing */
static u32  s_serial = 0;
static int  s_slotChan[SFX_MAX_SLOTS];    /* channel a LOOPING slot is pinned to, -1 none */
static int  s_isLooping[SFX_MAX_SLOTS];
static int  s_loopVol[SFX_MAX_SLOTS];
static int  s_loopFreq[SFX_MAX_SLOTS];

static const struct { int slot; const char *filename; } s_sfxTable[] = {
    { 0x00, "PAUSE.WAV"    }, { 0x01, "CHOOSE.WAV"   }, { 0x02, "SELECT.WAV"   },
    { 0x03, "RUNLEFT.WAV"  }, { 0x04, "RUNRIGHT.WAV" }, { 0x05, "AMY.WAV"      },
    { 0x06, "JET.WAV"      }, { 0x07, "JUMP.WAV"     }, { 0x08, "SPIN.WAV"     },
    { 0x09, "SPINGO.WAV"   }, { 0x0A, "SPINREV.WAV"  }, { 0x0B, "TAILS.WAV"    },
    { 0x0D, "JUMP.WAV"     }, { 0x0E, "FIRE.WAV"     }, { 0x0F, "EXPLODE.WAV"  },
    { 0x10, "AMYSKID.WAV"  }, { 0x11, "AMYWATER.WAV" }, { 0x12, "WATERRUN.WAV" },
    { 0x13, "WATERRUN.WAV" }, { 0x14, "BUBBLE.WAV"   }, { 0x15, "SPLASH.WAV"   },
    { 0x16, "POP.WAV"      }, { 0x18, "HITCHAR.WAV"  }, { 0x1A, "BONUS.WAV"    },
    { 0x1B, "GETTOKEN.WAV" }, { 0x1C, "GETCHAOS.WAV" }, { 0x1D, "RING1.WAV"    },
    { 0x1E, "RING1.WAV"    }, { 0x1F, "WARP.WAV"     }, { 0x20, "SKID1.WAV"    },
    { 0x21, "DOOR.WAV"     }, { 0x22, "RECORD.WAV"   }, { 0x23, "GOTALL.WAV"   },
    { 0x24, "BONUS.WAV"    }, { 0x27, "TAG.WAV"      }, { 0x2D, "THUNDER.WAV"  },
    { 0x32, "SPRING.WAV"   }, { 0x33, "BUMPER1.WAV"  }, { 0x34, "BUMPER2.WAV"  },
    { 0x35, "READY.WAV"    }, { 0x36, "SET.WAV"      }, { 0x37, "GO.WAV"       },
    { -1,   NULL           }
};

/* The 3DS speakers make the PC mix read harsh next to the music, and ndsp
 * sums channels without headroom: scale every effect down a little. */
#define SFX_MASTER_GAIN 0.7f

static float ds_volume_to_linear(int dsVolume)
{
    if (dsVolume <= g_volumeBase) return 0.0f;
    if (dsVolume >= 0)            return SFX_MASTER_GAIN;
    return SFX_MASTER_GAIN * (float)(dsVolume - g_volumeBase) / (float)(-g_volumeBase);
}

static char *read_file(const char *path, long *out_size)
{
    FILE *fp = fOpen(path, "rb");
    if (!fp) return NULL;
    fSeek(fp, 0, SEEK_END);
    long sz = fTell(fp);
    fSeek(fp, 0, SEEK_SET);
    if (sz <= 44) { fClose(fp); return NULL; }
    char *buf = (char *)malloc(sz);
    if (!buf) { fClose(fp); return NULL; }
    if (fRead(buf, 1, sz, fp) != (size_t)sz) { free(buf); fClose(fp); return NULL; }
    fClose(fp);
    *out_size = sz;
    return buf;
}

static int find_wav_chunks(const char *buf, long size, int *bits, long *dataOff,
                           long *dataLen, int *fmt, int *channels, int *rate)
{
    if (size < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) return 0;
    long off = 12;
    int haveFmt = 0, haveData = 0;
    while (off + 8 <= size) {
        const char *id = buf + off;
        uint32_t csz;
        memcpy(&csz, buf + off + 4, 4);
        long body = off + 8;
        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= size) {
            uint16_t b, tag, ch; uint32_t r;
            memcpy(&tag, buf + body + 0, 2);
            memcpy(&ch,  buf + body + 2, 2);
            memcpy(&r,   buf + body + 4, 4);
            memcpy(&b,   buf + body + 14, 2);
            *bits = b; *fmt = tag; *channels = ch; *rate = (int)r;
            haveFmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            *dataOff = body;
            *dataLen = (long)csz;
            if (*dataOff + *dataLen > size) *dataLen = size - *dataOff;
            haveData = 1;
        }
        if (haveFmt && haveData) return 1;
        off = body + ((csz + 1) & ~1u);
    }
    return haveFmt && haveData;
}

static void free_slot(int slot)
{
    SfxClip *c = &s_clip[slot];
    if (s_slotChan[slot] >= 0) {
        int ch = s_slotChan[slot];
        ndspChnWaveBufClear(SFX_FIRST_CHAN + ch);
        s_chanSlot[ch] = -1;
        s_slotChan[slot] = -1;
    }
    for (int ch = 0; ch < SFX_NUM_CHAN; ch++) {
        if (s_chanSlot[ch] == slot) {
            ndspChnWaveBufClear(SFX_FIRST_CHAN + ch);
            s_chanSlot[ch] = -1;
        }
    }
    if (c->pcm) {
        linearFree(c->pcm);
    }
    memset(c, 0, sizeof(*c));
    s_isLooping[slot] = 0;
    s_loopVol[slot] = -1;
    s_loopFreq[slot] = -1;
    g_soundBuffers[slot] = NULL;
    g_soundActive[slot] = 0;
}

static void load_wav_into_slot(int slot, const char *filename)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return;
    free_slot(slot);

    char path[512];
    snprintf(path, sizeof(path), DATA_DIR "/SOUND/SFX/%s", filename);
    long size = 0;
    char *buf = read_file(path, &size);
    if (!buf) {
        fprintf(stderr, "SFX: open %s failed for slot 0x%02X\n", path, slot);
        return;
    }
    int bits = 16, fmt = 1, channels = 1, rate = 22050;
    long dataOff = 0, dataLen = 0;
    if (!find_wav_chunks(buf, size, &bits, &dataOff, &dataLen, &fmt, &channels, &rate) ||
        fmt != 1 || (bits != 8 && bits != 16) || channels < 1 || channels > 2 || dataLen <= 0) {
        fprintf(stderr, "SFX: %s: unsupported WAV (fmt %d bits %d ch %d)\n", path, fmt, bits, channels);
        free(buf);
        return;
    }
    u32 frames = (u32)(dataLen / (channels * (bits / 8)));
    SfxClip *c = &s_clip[slot];
    c->pcm = (int16_t *)linearAlloc(frames * channels * sizeof(int16_t));
    if (!c->pcm) {
        fprintf(stderr, "SFX: linearAlloc failed for slot 0x%02X\n", slot);
        free(buf);
        return;
    }
    if (bits == 16) {
        memcpy(c->pcm, buf + dataOff, frames * channels * 2);
    } else {
        const unsigned char *src = (const unsigned char *)buf + dataOff;
        for (u32 i = 0; i < frames * channels; i++) {
            c->pcm[i] = (int16_t)(((int)src[i] - 128) << 8);
        }
    }
    free(buf);
    DSP_FlushDataCache(c->pcm, frames * channels * sizeof(int16_t));
    c->frames = frames;
    c->channels = channels;
    c->rate = rate;
    c->durationMs = (rate > 0) ? (int)(((uint64_t)frames * 1000) / rate) : 0;
    g_soundBuffers[slot] = (void *)(intptr_t)1;
    g_soundActive[slot] = 1;
}

void InitDirectSound(void)
{
    if (s_ready) {
        return;
    }
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_slotChan[i] = -1;
        s_isLooping[i] = 0;
        s_loopVol[i] = -1;
        s_loopFreq[i] = -1;
    }
    for (int ch = 0; ch < SFX_NUM_CHAN; ch++) {
        s_chanSlot[ch] = -1;
        s_chanSerial[ch] = 0;
    }

    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        fprintf(stderr, "InitDirectSound: ndspInit failed (%08lX) — is sdmc:/3ds/dspfirm.cdc present? Running silent.\n", (unsigned long)rc);
        s_ndspOk = 0;
        return;
    }
    s_ndspOk = 1;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    adx_stream_init();

    for (int i = 0; s_sfxTable[i].slot >= 0; i++) {
        load_wav_into_slot(s_sfxTable[i].slot, s_sfxTable[i].filename);
    }

    int loaded = 0;
    for (int i = 0; i < SFX_MAX_SLOTS; i++) if (s_clip[i].pcm) loaded++;
    fprintf(stderr, "InitDirectSound: ndsp ok, %d sfx loaded, linear free %u KB\n", loaded, (unsigned)(linearSpaceFree() / 1024));

    s_ready = 1;
    g_lpDirectSound = (void *)(intptr_t)1;
    g_initFeatureB = 1;
    SetAllSoundVolumes();
}

void CloseDirectSound(void)
{
    if (!s_ndspOk) {
        return;
    }
    s_ready = 0;
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        free_slot(i);
    }
    g_lpDirectSound = NULL;
    ndspExit();
    s_ndspOk = 0;
}

/* --- channel helpers ----------------------------------------------------- */

static void chan_set_volume(int ch, int vol255)
{
    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = mix[1] = (float)vol255 / 255.0f;
    ndspChnSetMix(SFX_FIRST_CHAN + ch, mix);
}

static int chan_alloc(void)
{
    int best = -1;
    u32 bestSerial = 0xFFFFFFFFu;
    for (int ch = 0; ch < SFX_NUM_CHAN; ch++) {
        int slot = s_chanSlot[ch];
        if (slot < 0) {
            return ch;
        }
        if (s_isLooping[slot] && s_slotChan[slot] == ch) {
            continue;   /* never steal a loop */
        }
        if (!ndspChnIsPlaying(SFX_FIRST_CHAN + ch)) {
            return ch;
        }
        if (s_chanSerial[ch] < bestSerial) {
            bestSerial = s_chanSerial[ch];
            best = ch;
        }
    }
    return best;
}

static void chan_start(int ch, int slot, int vol, int freq, int loop)
{
    SfxClip *c = &s_clip[slot];
    int id = SFX_FIRST_CHAN + ch;
    ndspChnWaveBufClear(id);
    ndspChnReset(id);
    ndspChnSetInterp(id, NDSP_INTERP_LINEAR);
    ndspChnSetRate(id, (float)(freq > 0 ? freq : c->rate));
    ndspChnSetFormat(id, c->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    chan_set_volume(ch, vol);
    memset(&c->wb, 0, sizeof(c->wb));
    c->wb.data_vaddr = c->pcm;
    c->wb.nsamples = c->frames;
    c->wb.looping = loop ? true : false;
    ndspChnWaveBufAdd(id, &c->wb);
    s_chanSlot[ch] = slot;
    s_chanSerial[ch] = ++s_serial;
}

#define SFX_TRIM_UNITY 256
static const short s_sfxSlotTrim[SFX_MAX_SLOTS] = {
    [0x0B] = 154,   /* TAILS.WAV rotor loop, 60% — see sfx_dc.c */
    [0x10] = 128,   /* AMYSKID.WAV — skids sit right on top of the music */
    [0x20] = 128,   /* SKID1.WAV */
    [0x03] = 180,   /* RUNLEFT.WAV / RUNRIGHT.WAV footsteps */
    [0x04] = 180,
};

static float sfx_slot_trim(int slot)
{
    int t = s_sfxSlotTrim[slot];
    return (t <= 0) ? 1.0f : (float)t / (float)SFX_TRIM_UNITY;
}

/* --- public API ----------------------------------------------------------- */

void SFX_Tick(void)
{
}

void SFX_Play(int slot, int loop, int freq)
{
    if (!s_ready)                          return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return;
    if (s_clip[slot].pcm == NULL)          return;

    int vol = (int)(ds_volume_to_linear(g_masterVolume) * 255.0f);
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;

    if (loop) {
        if (!s_isLooping[slot]) {
            int ch = s_slotChan[slot];
            if (ch < 0) {
                ch = chan_alloc();
                if (ch < 0) return;
            }
            chan_start(ch, slot, vol, freq, 1);
            s_slotChan[slot] = ch;
            s_isLooping[slot] = 1;
            s_loopVol[slot] = vol;
            s_loopFreq[slot] = freq;
            return;
        }
        /* Live pitch update only; volume is owned by SFX_SetVolume. */
        if (freq && freq != s_loopFreq[slot] && s_slotChan[slot] >= 0) {
            ndspChnSetRate(SFX_FIRST_CHAN + s_slotChan[slot], (float)freq);
            s_loopFreq[slot] = freq;
        }
        return;
    }

    int playVol = IS_REPLAY_VOICE_SLOT(slot) ? 255 : (int)((float)vol * sfx_slot_trim(slot));
    if (s_isLooping[slot] && s_slotChan[slot] >= 0) {
        ndspChnWaveBufClear(SFX_FIRST_CHAN + s_slotChan[slot]);
        s_chanSlot[s_slotChan[slot]] = -1;
        s_slotChan[slot] = -1;
    }
    s_isLooping[slot] = 0;
    s_loopVol[slot] = -1;
    s_loopFreq[slot] = -1;
    int f = freq;
    if (slot == 0xD) f = 22050 + 5512;
    int ch = chan_alloc();
    if (ch < 0) return;
    chan_start(ch, slot, playVol, f, 0);
}

void SFX_Stop(int slot)
{
    if (!s_ready) return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return;
    for (int ch = 0; ch < SFX_NUM_CHAN; ch++) {
        if (s_chanSlot[ch] == slot) {
            ndspChnWaveBufClear(SFX_FIRST_CHAN + ch);
            s_chanSlot[ch] = -1;
        }
    }
    s_slotChan[slot] = -1;
    s_isLooping[slot] = 0;
    s_loopVol[slot] = -1;
    s_loopFreq[slot] = -1;
}

void SFX_StopAll(void)
{
    if (!s_ready) return;
    for (int ch = 0; ch < SFX_NUM_CHAN; ch++) {
        ndspChnWaveBufClear(SFX_FIRST_CHAN + ch);
        s_chanSlot[ch] = -1;
    }
    for (int i = 0; i < SFX_MAX_SLOTS; i++) {
        s_slotChan[i] = -1;
        s_isLooping[i] = 0;
        s_loopVol[i] = -1;
        s_loopFreq[i] = -1;
    }
}

void SFX_SetVolume(int slot, int dsVolume)
{
    if (!s_ready) return;
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return;
    if (s_clip[slot].pcm == NULL) return;
    int vol = (int)(ds_volume_to_linear(dsVolume) * 255.0f * sfx_slot_trim(slot));
    if (vol < 0) vol = 0;
    if (vol > 255) vol = 255;
    if (s_isLooping[slot] && s_slotChan[slot] >= 0 && s_loopVol[slot] != vol) {
        chan_set_volume(s_slotChan[slot], vol);
        s_loopVol[slot] = vol;
    }
}

void SFX_SetPan(int slot, int dsPan)   { (void)slot; (void)dsPan; }
void SFX_SetPosition(int slot, int p)  { (void)slot; (void)p; }

static int PlaySoundSimple(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return 0;
    if (g_soundActive[slot] == 0) return 0;
    if (g_optSfxVolume == 0) return 1;
    SFX_Play(slot, 0, 0);
    return 1;
}

static int PlaySoundWithParams(int slot, int distance, int freqParam)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return 0;
    if (g_soundActive[slot] == 0) return 0;
    if (g_optSfxVolume == 0) return 1;

    if (distance != 0x100) {
        int d = distance;
        if (d < 0x40) d = 0xFF;
        else d = 0xFF - d;
        int volDiff = g_masterVolume - g_volumeBase;
        if (volDiff < 0) volDiff = -volDiff;
        int divisor = (g_demoMode == 2) ? 0x12c : 0xff;
        int attenVol = (d * volDiff) / divisor + g_volumeBase;
        SFX_SetVolume(slot, attenVol);
    }
    if (freqParam != 0) {
        freqParam = (freqParam * 99900) / 255 + 100;
    }
    SFX_Play(slot, 1, freqParam);
    return 1;
}

static int g_ringAlternate;

void PlaySoundEffect(int soundCmd, int distance, int freqParam)
{
    int slot = soundCmd & 0xFFFF;
    if (soundCmd & 0xFFFF0000) {
        PlaySoundWithParams(slot, distance, freqParam);
    } else {
        PlaySoundSimple(slot);
    }
    if (g_demoMode == DEMO_REPLAY) return;
    if ((soundCmd & 0xFFFF) == 0x1D) {
        slot = 0x1D + g_ringAlternate;
        g_ringAlternate = (g_ringAlternate + 1) & 1;
    }
}

void LoadSoundEffect(const char *filename, int slot)
{
    if (!s_ndspOk) return;
    load_wav_into_slot(slot, filename);
}

int SFX_ClipDurationMs(int slot)
{
    if (slot < 0 || slot >= SFX_MAX_SLOTS) return 0;
    return s_clip[slot].durationMs;
}
