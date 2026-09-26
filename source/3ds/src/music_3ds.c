/**
 * music_3ds.c — music control for the 3DS build, over the ADX streamer in
 * adx_stream_3ds.c. Files live at {DATA_DIR}/MUSIC/trackN.adx, N = the original
 * CD track number (2..21). Logic mirrors dc/src/music_dc.c.
 */

#include <stdint.h>
#include <stdio.h>

#include "adx_stream_3ds.h"
#include "sonicr_paths.h"

extern int g_mciDeviceId;
extern int g_musicEnabled;
extern void SFX_DuckStop(void);

static int s_currentTrack = 0;
static int s_currentTrackLoops = 0;
static int s_currentTrackIsFanfare = 0;
static int s_paused = 0;

#define MUSIC_VOL_NORMAL 255
#define MUSIC_VOL_DUCKED (MUSIC_VOL_NORMAL / 2)

static int s_musicDucked = 0;
static int s_musicLevel = 8;

static void Music_ApplyVolume(void)
{
    int base = s_musicDucked ? MUSIC_VOL_DUCKED : MUSIC_VOL_NORMAL;
    adx_stream_volume(base * s_musicLevel / 8);
}

void Music_SetVolume(int level)
{
    if (level < 0) level = 0;
    if (level > 8) level = 8;
    s_musicLevel = level;
    Music_ApplyVolume();
}

void Music_SetDucked(int ducked)
{
    s_musicDucked = ducked ? 1 : 0;
    Music_ApplyVolume();
}

/* Tracks 2, 3, 4, 0x13, 0x14, 0x15 are fanfares: they play once. */
static int track_loops(int track)
{
    return !(track == 2 || track == 3 || track == 4 ||
             track == 0x13 || track == 0x14 || track == 0x15);
}

static void play_track(int track)
{
    if (track < 2 || track > 21) {
        return;
    }
    if (track == s_currentTrack) {
        if (adx_stream_is_playing() || s_currentTrackIsFanfare) {
            return;
        }
    }
    if (s_currentTrack != 2 && s_currentTrack != 0 && !s_currentTrackLoops && adx_stream_is_playing()) {
        if (!track_loops(track)) {
            return;
        }
    }
    if (s_currentTrack != 0) {
        adx_stream_stop();
        adx_stream_destroy();
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/MUSIC/track%d.adx", DATA_DIR, track);

    int loops = track_loops(track);
    if (adx_stream_create(path, loops) != 0) {
        s_currentTrack = 0;
        return;
    }
    Music_ApplyVolume();
    adx_stream_play();

    s_currentTrack = track;
    s_currentTrackLoops = loops;
    s_currentTrackIsFanfare = !loops;
    s_paused = 0;
}

static void stop_music(void)
{
    if (s_currentTrack == 0) {
        return;
    }
    adx_stream_stop();
    adx_stream_destroy();
    s_currentTrack = 0;
    s_currentTrackLoops = 0;
    s_paused = 0;
    s_currentTrackIsFanfare = 0;
}

int GetLogicalCDTrack(void)
{
    if (s_currentTrack == 0) {
        return 0;
    }
    if (adx_stream_is_playing()) {
        return s_currentTrack;
    }
    return s_currentTrack + 1;
}

int OpenCDDevice(void)
{
    g_mciDeviceId = 1;
    return 1;
}

void CloseCDDevice(void)
{
    stop_music();
    adx_stream_shutdown();
}

void PauseCD(void)
{
    if (s_currentTrack == 0) {
        return;
    }
    if (s_currentTrackIsFanfare && !adx_stream_is_playing()) {
        return;
    }
    adx_stream_pause();
    s_paused = 1;
}

void ResumeCD(void)
{
    if (s_paused) {
        adx_stream_play();
        s_paused = 0;
    }
}

void StopCD(void)
{
    stop_music();
    SFX_DuckStop();
}

void UpdateCDPlayback(int track)
{
    if (g_musicEnabled == 0) {
        return;
    }
    play_track(track);
}
