/**
 * adx_stream_3ds.h — streamed ADX music on ndsp (one stream at a time).
 * Same shape as the DC's wav_* API in dc/src/sndwav.h.
 */
#ifndef ADX_STREAM_3DS_H
#define ADX_STREAM_3DS_H

int  adx_stream_init(void);        /* after ndspInit; 1 if the stream is usable */
void adx_stream_shutdown(void);
int  adx_stream_create(const char *path, int loop);   /* 0 ok, -1 fail */
void adx_stream_destroy(void);
void adx_stream_play(void);
void adx_stream_pause(void);
void adx_stream_stop(void);
void adx_stream_volume(int vol);   /* 0..255 */
int  adx_stream_is_playing(void);

#endif
