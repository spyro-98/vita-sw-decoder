#include "internal/elementary_audio.h"

#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include "module_log.h"

/* Audio output uses a dedicated raw Vita worker. The stack leaves headroom for
 * demux and hardware AAC calls without coupling the module to an app thread. */
#define ELEMENTARY_AUDIO_THREAD_PRIORITY 0x10000100
#define ELEMENTARY_AUDIO_THREAD_STACK    0x40000
/* Vita AudioOut grain used by the hardware AAC output loop. */
#define ELEMENTARY_AUDIO_GRAIN 1024
/* Short wait while paused: it is not pacing, just to avoid spinning the CPU
 * while the thread stays alive without producing output. */
#define ELEMENTARY_AUDIO_PAUSE_DELAY_US (10 * 1000)

static int first_audio_stream(const AVFormatContext *ctx) {
	for (unsigned int i = 0; ctx && i < ctx->nb_streams; i++) {
		if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			return (int)i;
	}
	return -1;
}

static int parse_adts_header(const uint8_t *data, size_t size,
	                         uint32_t *channels, uint32_t *sample_rate) {
	static const uint32_t rates[13] = {
		96000, 88200, 64000, 48000, 44100, 32000, 24000,
		22050, 16000, 12000, 11025, 8000, 7350
	};
	if (!data || size < 7 || data[0] != 0xff || (data[1] & 0xf6) != 0xf0)
		return -1;
	unsigned rate_index = (data[2] >> 2) & 0x0f;
	unsigned channel_config = ((unsigned)(data[2] & 1) << 2) |
	                          ((unsigned)data[3] >> 6);
	if (rate_index >= 13 || channel_config < 1 || channel_config > 2)
		return -1;
	if (channels) *channels = channel_config;
	if (sample_rate) *sample_rate = rates[rate_index];
	return 0;
}

/* Reads the next packet of the selected audio track, silently discarding
 * packets belonging to other tracks that may be present in the same
 * AVFormatContext. Returns
 * 1 if *packet was filled in, 0 at EOF, <0 on error (including the abort due
 * to cancel: the interrupt_callback is already attached to the
 * AVFormatContext by the caller, see elementary_audio.h). */
static int read_audio_packet(ElementaryAudioState *st, AVPacket *packet) {
	if (st->prefetched_packet) {
		av_packet_move_ref(packet, st->prefetched_packet);
		av_packet_free(&st->prefetched_packet);
		return 1;
	}
	int ret;
	while ((ret = av_read_frame(st->demux, packet)) >= 0) {
		if (packet->stream_index == st->stream_index) return 1;
		av_packet_unref(packet);
	}
	return ret == AVERROR_EOF ? 0 : ret;
}

static int vita_sw_elementary_audio_thread(SceSize args, void *argp) {
	(void)args;
	ElementaryAudioState *st = *(ElementaryAudioState **)argp;

	AVPacket *packet = av_packet_alloc();
	if (!packet) {
		st->had_error = 1;
		return 0;
	}

	const AVRational stream_tb = st->demux->streams[st->stream_index]->time_base;
	const AVRational us_tb = { 1, 1000000 };
	int decode_attempts = 0;
	int last_applied_volume = -1;
	while (st->start_gate && !*st->start_gate && !*st->cancel)
		sceKernelDelayThread(1000);

	while (!*st->cancel) {
		int requested_volume = st->volume_percent;
		if (requested_volume != last_applied_volume && st->port >= 0) {
			last_applied_volume = requested_volume;
			int hardware_percent = requested_volume > 100 ? 100 : requested_volume;
			int hardware_volume = hardware_percent * SCE_AUDIO_VOLUME_0DB / 100;
			int volume[2] = { hardware_volume, hardware_volume };
			sceAudioOutSetVolume(st->port,
			                      SCE_AUDIO_VOLUME_FLAG_L_CH |
			                          SCE_AUDIO_VOLUME_FLAG_R_CH,
			                      volume);
		}
		if (st->paused) {
			/* No sceAudioOutOutput and no advancing of played_pts_us while
			 * paused (SS5 point 5): the thread stays alive and rechecks the
			 * state after a short interval. */
			sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
			continue;
		}

		int ret = read_audio_packet(st, packet);
		if (ret == 0) {
			st->eof = 1;
			break;
		}
		if (ret < 0) {
			/* If cancel is already raised, the error is almost certainly the
			 * interrupt_callback abort: it is not a decode failure, so we
			 * exit cleanly without had_error. */
			if (!*st->cancel) {
				log_printf("elementary_audio: av_read_frame -> %d", ret);
				st->had_error = 1;
			}
			break;
		}
		if (st->initial_position_ms > 0) {
			int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
			int64_t pts_ms = pts == AV_NOPTS_VALUE ? -1
			               : av_rescale_q(pts, stream_tb, (AVRational){ 1, 1000 });
			if (pts_ms < 0 || (uint64_t)pts_ms < st->initial_position_ms) {
				av_packet_unref(packet);
				continue;
			}
			log_printf("elementary_audio: resume AU=%lld ms requested=%llu ms",
			           (long long)pts_ms,
			           (unsigned long long)st->initial_position_ms);
			st->initial_position_ms = 0;
		}

		VitaAacDecodedAudio decoded;
		memset(&decoded, 0, sizeof(decoded));
		int decode_ret = vita_sw_aac_decoder_decode(&st->decoder, packet->data,
		                                         (size_t)packet->size, &decoded);
		if (decode_ret < 0) {
			log_printf("elementary_audio: vita_sw_aac_decoder_decode -> %d (attempt #%d)",
			          decode_ret, decode_attempts);
			av_packet_unref(packet);
			/* Error not on the first packet: fatal, SS5 point 7. On the first
			 * packet it is tolerated (hardware decoder warm-up) and we carry
			 * on. */
			if (decode_attempts > 0) {
				st->had_error = 1;
				break;
			}
			decode_attempts++;
			continue;
		}
		decode_attempts++;

		/* The pts must be read before av_packet_unref(): unref clears it. */
		int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
		av_packet_unref(packet);

		if (decode_ret == 0 || !decoded.pcm || decoded.frames == 0) {
			/* No samples produced for this packet: it is not an error
			 * (symmetry with the video decoder, SS5 point 2), we keep
			 * feeding packets. */
			continue;
		}
		/* A pause that lands after decode must not discard this access unit.
		 * The per-grain loop below holds the decoded PCM until resume. */
		if (*st->cancel) continue;

		/* The AudioOut port consumes exactly the grain chosen at open (1024
		 * samples/channel). SBR can make one AAC access unit produce 2048
		 * samples, which must be delivered as two consecutive grains; passing
		 * the base pointer only once would discard half the PCM while moving the
		 * clock by the full packet. AAC-LC/SBR are expected to be exact
		 * multiples here; fail closed on any other shape instead of corrupting
		 * sync silently. */
		if (decoded.frames % ELEMENTARY_AUDIO_GRAIN != 0) {
			log_printf("elementary_audio: PCM not aligned to output grain (%u frames)",
			          decoded.frames);
			st->had_error = 1;
			break;
		}
		/* Same 100..200% software boost as the direct player. This PCM buffer
		 * belongs to our decoder and isn't reused until the next decode, which
		 * happens only after all blocking AudioOut grains below have returned. */
		if (requested_volume > 100) {
			int16_t *pcm = (int16_t *)(uintptr_t)decoded.pcm;
			uint32_t samples = decoded.frames * decoded.channels;
			for (uint32_t i = 0; i < samples; i++) {
				int32_t value = (int32_t)pcm[i] * requested_volume / 100;
				if (value > INT16_MAX) value = INT16_MAX;
				else if (value < INT16_MIN) value = INT16_MIN;
				pcm[i] = (int16_t)value;
			}
		}
		int64_t pts_us = pts != AV_NOPTS_VALUE
		               ? av_rescale_q(pts, stream_tb, us_tb) : AV_NOPTS_VALUE;
		for (uint32_t offset = 0; offset < decoded.frames;
		     offset += ELEMENTARY_AUDIO_GRAIN) {
			while (st->paused && !*st->cancel)
				sceKernelDelayThread(ELEMENTARY_AUDIO_PAUSE_DELAY_US);
			if (*st->cancel) break;
			const int16_t *grain = decoded.pcm + offset * st->decoder.channels;
			int output_ret = sceAudioOutOutput(st->port, grain);
			if (output_ret < 0) {
				log_printf("elementary_audio: sceAudioOutOutput -> 0x%08X",
				          (unsigned)output_ret);
				st->had_error = 1;
				break;
			}

			/* Publish only samples that AudioOut has accepted. */
			if (pts_us >= 0) {
				uint64_t end_us = (uint64_t)pts_us +
				    (uint64_t)(offset + ELEMENTARY_AUDIO_GRAIN) * 1000000ULL /
				    st->sample_rate;
				st->played_until_ms = end_us / 1000ULL > UINT32_MAX
				                         ? UINT32_MAX : (uint32_t)(end_us / 1000ULL);
			}
		}
		if (st->had_error || *st->cancel) break;
	}

	av_packet_free(&packet);
	return 0;
}

int vita_sw_elementary_audio_start(ElementaryAudioState *st, AVFormatContext *demux,
                           int stream_index, uint64_t initial_position_ms,
                           volatile int *cancel,
                           volatile int *start_gate) {
	if (!st || !demux || !cancel) return -1;
	memset(st, 0, sizeof(*st));
	st->port = -1;
	st->thid = -1;
	st->demux = demux;
	st->cancel = cancel;
	st->start_gate = start_gate;
	st->initial_position_ms = initial_position_ms;
	st->volume_percent = 100;

	if (stream_index < 0) stream_index = first_audio_stream(demux);
	if (stream_index < 0 || (unsigned int)stream_index >= demux->nb_streams ||
	    demux->streams[stream_index]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
		log_printf("elementary_audio: no valid audio track (stream_index=%d)",
		          stream_index);
		return -1;
	}
	st->stream_index = stream_index;

	const AVCodecParameters *params = demux->streams[stream_index]->codecpar;
	uint32_t channels = params->ch_layout.nb_channels > 0
	                   ? (uint32_t)params->ch_layout.nb_channels : 0;
	uint32_t sample_rate = params->sample_rate > 0 ? (uint32_t)params->sample_rate : 0;
	/* MPEG-TS carries AAC as ADTS. FFmpeg can identify the stream while still
	 * leaving codecpar channels/rate empty after a short probe.
	 * Inspect one AU directly, retain it for the worker, and configure
	 * sceAudiodec for ADTS instead of rejecting an otherwise valid live. */
	if (params->codec_id == AV_CODEC_ID_AAC &&
	    (!channels || !sample_rate || params->extradata_size <= 0)) {
		AVPacket *probe = av_packet_alloc();
		int probe_ret = probe ? read_audio_packet(st, probe) : AVERROR(ENOMEM);
		if (probe_ret > 0) {
			uint32_t adts_channels = 0, adts_rate = 0;
			if (parse_adts_header(probe->data, (size_t)probe->size,
			                      &adts_channels, &adts_rate) == 0) {
				st->input_is_adts = 1;
				if (!channels) channels = adts_channels;
				if (!sample_rate) sample_rate = adts_rate;
				log_printf("elementary_audio: ADTS detected ch=%u rate=%u packet=%d",
				          adts_channels, adts_rate, probe->size);
			}
			st->prefetched_packet = probe;
		} else {
			av_packet_free(&probe);
			if ((!channels || !sample_rate) && probe_ret < 0)
				log_printf("elementary_audio: probe ADTS -> %d", probe_ret);
		}
	}
	if (!channels || !sample_rate) {
		log_printf("elementary_audio: missing codec parameters (ch=%u rate=%u)",
		          channels, sample_rate);
		av_packet_free(&st->prefetched_packet);
		return -1;
	}
	st->sample_rate = sample_rate;

	/* SBR state is selected inside vita_sw_aac_decoder_init_ex. A future decoder
	 * revision can parse the complete AudioSpecificConfig when required. */
	int ret = vita_sw_aac_decoder_init_ex(&st->decoder, channels, sample_rate,
	                                  st->input_is_adts);
	if (ret < 0) {
		log_printf("elementary_audio: vita_sw_aac_decoder_init -> 0x%08X", (unsigned)ret);
		av_packet_free(&st->prefetched_packet);
		return ret;
	}
	st->decoder_ready = 1;

	int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, ELEMENTARY_AUDIO_GRAIN,
	                               (int)sample_rate,
	                               channels == 1 ? SCE_AUDIO_OUT_MODE_MONO
	                                             : SCE_AUDIO_OUT_MODE_STEREO);
	log_printf("elementary_audio: sceAudioOutOpenPort -> 0x%08X (ch=%u rate=%u)",
	          (unsigned)port, channels, sample_rate);
	if (port < 0) {
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		return port;
	}
	st->port = port;
	st->port_open = 1;
	int volume[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
	sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
	                     volume);

	st->thid = sceKernelCreateThread("VitaSwDecoderPlayerAudio", vita_sw_elementary_audio_thread,
	                                 ELEMENTARY_AUDIO_THREAD_PRIORITY,
	                                 ELEMENTARY_AUDIO_THREAD_STACK, 0, 0, NULL);
	if (st->thid < 0) {
		ret = st->thid;
		sceAudioOutReleasePort(st->port);
		st->port = -1;
		st->port_open = 0;
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		return ret;
	}

	void *self = st;
	ret = sceKernelStartThread(st->thid, sizeof(self), &self);
	if (ret < 0) {
		log_printf("elementary_audio: sceKernelStartThread -> 0x%08X", (unsigned)ret);
		sceKernelDeleteThread(st->thid);
		st->thid = -1;
		sceAudioOutReleasePort(st->port);
		st->port = -1;
		st->port_open = 0;
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
		av_packet_free(&st->prefetched_packet);
		return ret;
	}

	st->started = 1;
	return 0;
}

void vita_sw_elementary_audio_join(ElementaryAudioState *st) {
	if (!st) return;
	/* Each step is gated on its own state flag, not on the numeric value of
	 * the corresponding field: if vita_sw_elementary_audio_start() was never called
	 * (or failed) on a state zeroed by the caller, port/thid set to 0 would
	 * be indistinguishable from valid handles. */
	if (st->started && st->thid >= 0) sceKernelWaitThreadEnd(st->thid, NULL, NULL);
	if (st->port_open) {
		sceAudioOutReleasePort(st->port);
		st->port_open = 0;
	}
	if (st->decoder_ready) {
		/* vita_sw_aac_decoder_term() is not safe on a VitaAacDecoder that was
		 * never initialized: teardown_partial() reads es_memblock/pcm_memblock
		 * from it as already written by vita_sw_aac_decoder_init(), which on a
		 * zeroed struct would be 0 (an apparently valid memblock) instead of
		 * the expected -1 sentinel. decoder_ready is the only correct guard
		 * here. */
		vita_sw_aac_decoder_term(&st->decoder);
		st->decoder_ready = 0;
	}
	if (st->started && st->thid >= 0) sceKernelDeleteThread(st->thid);
	av_packet_free(&st->prefetched_packet);
	st->thid = -1;
	st->port = -1;
	st->started = 0;
}

uint64_t vita_sw_elementary_audio_clock_us(const ElementaryAudioState *st) {
	if (!st) return 0;
	return (uint64_t)st->played_until_ms * 1000ULL;
}

void vita_sw_elementary_audio_set_volume(ElementaryAudioState *st, int percent) {
	if (!st) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	st->volume_percent = percent;
}
