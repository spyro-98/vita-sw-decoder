#ifndef VITA_SW_DECODER_ELEMENTARY_AUDIO_H
#define VITA_SW_DECODER_ELEMENTARY_AUDIO_H

#include <stdint.h>

#include <psp2/kernel/threadmgr.h>

#include <libavformat/avformat.h>

#include "internal/vita_aac_decoder.h"

/* Reads AAC packets from an AVFormatContext owned by the caller, decodes them
 * with sceAudiodec and sends PCM through blocking sceAudioOutOutput. The
 * submitted PTS is the player audio master clock. */

typedef struct {
	AVFormatContext *demux;   /* owned by the caller, NOT closed here */
	int stream_index;
	VitaAacDecoder decoder;
	volatile int *cancel;      /* shared with the caller, not owned */
	volatile int *start_gate;  /* decoder is ready, output starts only on GO */
	volatile int had_error;
	volatile int eof;
	/* ARM32 can publish a 32-bit millisecond clock atomically; the previous
	 * volatile uint64_t could tear between the audio and render threads. */
	volatile uint32_t played_until_ms;
	uint32_t sample_rate;
	AVPacket *prefetched_packet; /* first probed TS AU, consumed by the worker */
	int input_is_adts;
	volatile int paused;       /* the caller raises/lowers it, the thread honors it */
	volatile int volume_percent; /* shared 0..300 perceived volume */
	uint64_t initial_position_ms; /* progressive restart: discard earlier AUs */
	int port;
	SceUID thid;
	int decoder_ready; /* guard for vita_sw_aac_decoder_term(), see elementary_audio.c */
	int port_open;      /* guard for sceAudioOutReleasePort() */
	int started;        /* guard for sceKernelWaitThreadEnd()/DeleteThread() */
} ElementaryAudioState;

/* Finds the first audio track in `demux` (if stream_index < 0) or uses the
 * one given, initializes the AAC decoder with the codecpar parameters and
 * starts the read/decode/output thread. Returns 0 on success, <0 on error (no
 * resource stays allocated on failure). */
int vita_sw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate);

/* Waits for the thread to end (because of cancel/eof/had_error), closes the
 * audio port and the decoder. Safe even if vita_sw_elementary_audio_start() was never
 * called or failed (zeroed state). */
void vita_sw_elementary_audio_join(ElementaryAudioState *st);

/* Last pts (microseconds) actually sent to sceAudioOutOutput. Used by the
 * video thread as the synchronization clock. */
uint64_t vita_sw_elementary_audio_clock_us(const ElementaryAudioState *st);

void vita_sw_elementary_audio_set_volume(ElementaryAudioState *st, int percent);

#endif /* VITA_SW_DECODER_ELEMENTARY_AUDIO_H */
