#include "vita_sw_decoder.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>

#include "decoder_runtime.h"
#include "internal/elementary_audio.h"
#include "internal/software_video.h"

#ifndef VITA_SW_DECODER_HAS_H264_VITA
#define VITA_SW_DECODER_HAS_H264_VITA 0
#endif

#define VITA_SW_DECODER_AVIO_BUFFER_SIZE (64 * 1024)
#define VITA_SW_DECODER_CLOCK_WINDOW_US 32000ULL

typedef struct {
	VitaSwDecoderStreamHandle stream;
	AVIOContext *avio;
	AVFormatContext *format;
	volatile int *cancel;
} VitaSwDecoderInput;

struct VitaSwDecoderPlayer {
	VitaSwDecoderPlayerConfig config;
	VitaSwDecoderInput video_input;
	VitaSwDecoderInput audio_input;
	SoftwareVideoState video;
	ElementaryAudioState audio;
	volatile int local_cancel;
	volatile int *cancel;
	volatile int start_gate;
	int opened;
	int paused;
	uint64_t duration_ms;
	uint64_t video_bitrate_bps;
	uint64_t clock_us;
	uint64_t clock_wall_us;
	int clock_started;
	int has_audio;
};

int vita_sw_decoder_prepare_runtime(void) {
	return vita_sw_decoder_runtime_prepare();
}

const char *vita_sw_decoder_backend_name(void) {
	return "software";
}

static int stream_avio_read(void *opaque, uint8_t *buffer, int size) {
	VitaSwDecoderInput *input = (VitaSwDecoderInput *)opaque;
	if (!input || !input->stream.read || (input->cancel && *input->cancel))
		return AVERROR_EXIT;
	int ret = input->stream.read(input->stream.opaque, buffer, (size_t)size);
	if (ret == 0) return AVERROR_EOF;
	return ret < 0 ? AVERROR(EIO) : ret;
}

static int64_t stream_avio_seek(void *opaque, int64_t offset, int whence) {
	VitaSwDecoderInput *input = (VitaSwDecoderInput *)opaque;
	if (!input || (input->cancel && *input->cancel)) return AVERROR_EXIT;
	if (whence == AVSEEK_SIZE) return input->stream.size;
	if (!input->stream.seek) return AVERROR(ENOSYS);
	return input->stream.seek(input->stream.opaque, offset,
	                          whence & ~AVSEEK_FORCE);
}

static int interrupt_cb(void *opaque) {
	volatile int *cancel = (volatile int *)opaque;
	return cancel && *cancel;
}

static int playback_streams_ready(const AVFormatContext *format) {
	int have_video = 0;
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVStream *stream = format->streams[i];
		const AVCodecParameters *params = stream->codecpar;
		if (params->codec_type == AVMEDIA_TYPE_VIDEO &&
		    params->codec_id == AV_CODEC_ID_H264) {
			if (params->width <= 0 || params->height <= 0 ||
			    stream->time_base.num <= 0 || stream->time_base.den <= 0)
				return 0;
			have_video = 1;
		} else if (params->codec_type == AVMEDIA_TYPE_AUDIO &&
		           params->codec_id == AV_CODEC_ID_AAC &&
		           (params->sample_rate <= 0 ||
		            params->ch_layout.nb_channels <= 0)) {
			return 0;
		}
	}
	return have_video;
}

static int indexed_container(const AVFormatContext *format) {
	const char *name = format && format->iformat ? format->iformat->name : NULL;
	return name && (strstr(name, "mov,mp4") || strstr(name, "matroska") ||
	                strstr(name, "webm") || strstr(name, "avi"));
}

static void input_close(VitaSwDecoderInput *input) {
	if (!input) return;
	if (input->format) avformat_close_input(&input->format);
	if (input->avio) {
		av_freep(&input->avio->buffer);
		avio_context_free(&input->avio);
	}
	if (input->stream.close && input->stream.opaque)
		input->stream.close(input->stream.opaque);
	memset(input, 0, sizeof(*input));
}

static int input_open(VitaSwDecoderInput *input,
	                  const VitaSwDecoderStreamFactory *factory,
	                  volatile int *cancel) {
	if (!input || !factory || !factory->open) return AVERROR(EINVAL);
	memset(input, 0, sizeof(*input));
	input->cancel = cancel;
	int ret = factory->open(factory->opaque, &input->stream);
	if (ret < 0 || !input->stream.read || !input->stream.seek) {
		input_close(input);
		return ret < 0 ? ret : AVERROR(EINVAL);
	}
	unsigned char *buffer = av_malloc(VITA_SW_DECODER_AVIO_BUFFER_SIZE);
	if (!buffer) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio = avio_alloc_context(buffer, VITA_SW_DECODER_AVIO_BUFFER_SIZE, 0,
	                                 input, stream_avio_read, NULL,
	                                 stream_avio_seek);
	if (!input->avio) {
		av_free(buffer);
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->avio->seekable = AVIO_SEEKABLE_NORMAL;
	input->format = avformat_alloc_context();
	if (!input->format) {
		input_close(input);
		return AVERROR(ENOMEM);
	}
	input->format->pb = input->avio;
	input->format->flags |= AVFMT_FLAG_CUSTOM_IO;
	input->format->interrupt_callback.callback = interrupt_cb;
	input->format->interrupt_callback.opaque = (void *)cancel;
	input->format->probesize = 1024 * 1024;
	input->format->max_analyze_duration = 2 * AV_TIME_BASE;
	ret = avformat_open_input(&input->format, NULL, NULL, NULL);
	if (ret < 0) {
		input_close(input);
		return ret;
	}
	/* Fast indexed files normally expose everything during open. Run bounded
	 * discovery whenever the exact H.264/AAC parameters needed by playback are
	 * incomplete; container type alone is not evidence that they are ready. */
	ret = indexed_container(input->format) &&
	      playback_streams_ready(input->format)
	    ? 0 : avformat_find_stream_info(input->format, NULL);
	if (ret < 0) {
		input_close(input);
		return ret;
	}
	return 0;
}

static int find_stream(const AVFormatContext *format, enum AVMediaType type,
	                   enum AVCodecID codec, int ordinal) {
	if (ordinal < 0) return -1;
	for (unsigned int i = 0; format && i < format->nb_streams; i++) {
		const AVCodecParameters *params = format->streams[i]->codecpar;
		if (params->codec_type != type || params->codec_id != codec) continue;
		if (ordinal-- == 0) return (int)i;
	}
	return -1;
}

static uint64_t input_duration_ms(const AVFormatContext *format) {
	if (!format || format->duration <= 0) return 0;
	return (uint64_t)format->duration * 1000ULL / AV_TIME_BASE;
}

static void close_session(VitaSwDecoderPlayer *player) {
	if (!player) return;
	if (player->cancel) *player->cancel = 1;
	__sync_synchronize();
	if (player->video.started) vita_sw_software_video_join(&player->video);
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	input_close(&player->audio_input);
	input_close(&player->video_input);
	player->opened = 0;
	player->start_gate = 0;
}

static int seek_input_ms(VitaSwDecoderInput *input, int stream_index,
	                     uint64_t position_ms) {
	if (!input || !input->format || stream_index < 0) return AVERROR(EINVAL);
	AVStream *stream = input->format->streams[stream_index];
	int64_t target = av_rescale_q((int64_t)position_ms,
	                              (AVRational){ 1, 1000 }, stream->time_base);
	int ret = avformat_seek_file(input->format, stream_index, INT64_MIN,
	                             target, target, AVSEEK_FLAG_BACKWARD);
	if (ret < 0)
		ret = av_seek_frame(input->format, stream_index, target,
		                    AVSEEK_FLAG_BACKWARD);
	if (ret >= 0) avformat_flush(input->format);
	return ret;
}

static int restart_audio_track(VitaSwDecoderPlayer *player, int audio_track,
	                           uint64_t position_ms) {
	player->audio.stop = 1;
	__sync_synchronize();
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	input_close(&player->audio_input);
	memset(&player->audio, 0, sizeof(player->audio));

	int ret = input_open(&player->audio_input, &player->config.stream,
	                     player->cancel);
	if (ret < 0) return ret;
	int audio_index = find_stream(player->audio_input.format,
	                              AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC,
	                              audio_track);
	if (audio_index < 0) {
		input_close(&player->audio_input);
		return AVERROR_DECODER_NOT_FOUND;
	}
	if (position_ms > 0) {
		ret = seek_input_ms(&player->audio_input, audio_index, position_ms);
		if (ret < 0) {
			input_close(&player->audio_input);
			return ret;
		}
	}
	ret = vita_sw_elementary_audio_start(&player->audio,
	                                     player->audio_input.format, audio_index,
	                                     position_ms, player->cancel,
	                                     &player->start_gate);
	if (ret < 0) {
		input_close(&player->audio_input);
		return ret;
	}
	vita_sw_elementary_audio_set_volume(&player->audio,
	                                    player->config.volume_percent);
	player->audio.paused = player->paused;
	player->has_audio = 1;
	return 0;
}

static int restart_session_in_place(VitaSwDecoderPlayer *player,
	                                uint64_t position_ms) {
	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0);
	int audio_index = player->has_audio
	                ? find_stream(player->audio_input.format,
	                              AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC,
	                              player->config.audio_track) : -1;
	if (video_index < 0 || (player->has_audio && audio_index < 0))
		return AVERROR_DECODER_NOT_FOUND;
	player->start_gate = 0;
	player->video.stop = 1;
	player->audio.stop = 1;
	__sync_synchronize();
	if (player->video.started) vita_sw_software_video_join(&player->video);
	if (player->audio.started || player->audio.decoder_ready || player->audio.port_open)
		vita_sw_elementary_audio_join(&player->audio);
	int ret = seek_input_ms(&player->video_input, video_index, position_ms);
	if (ret >= 0 && player->has_audio)
		ret = seek_input_ms(&player->audio_input, audio_index, position_ms);
	if (ret < 0) return ret;
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));
	player->clock_us = position_ms * 1000ULL;
	player->clock_wall_us = sceKernelGetProcessTimeWide();
	player->clock_started = 0;
	ret = vita_sw_software_video_start(&player->video, player->video_input.format,
	                           video_index, player->config.expected_width,
	                           player->config.expected_height,
	                           player->config.expected_fps, position_ms,
	                           player->cancel, &player->start_gate);
	if (ret < 0) return ret;
	if (player->has_audio) {
		ret = vita_sw_elementary_audio_start(&player->audio,
		                            player->audio_input.format, audio_index,
		                            position_ms, player->cancel,
		                            &player->start_gate);
		if (ret < 0) return ret;
		vita_sw_elementary_audio_set_volume(&player->audio,
		                                    player->config.volume_percent);
		player->audio.paused = player->paused;
	}
	uint64_t deadline = sceKernelGetProcessTimeWide() + 2000000ULL;
	while (!*player->cancel && !player->video.had_error && !player->video.eof &&
	       sceKernelGetProcessTimeWide() < deadline) {
		int ready = 0;
		vita_sw_software_video_buffer_status(&player->video, &ready, NULL, NULL);
		if (ready >= 2) break;
		sceKernelDelayThread(1000);
	}
	if (*player->cancel) return AVERROR_EXIT;
	__sync_synchronize();
	player->start_gate = 1;
	return 0;
}

static int open_session(VitaSwDecoderPlayer *player, uint64_t start_position_ms) {
	player->cancel = player->config.cancel_flag
	               ? player->config.cancel_flag : &player->local_cancel;
	*player->cancel = 0;
	player->start_gate = 0;
	player->clock_us = 0;
	player->clock_wall_us = 0;
	player->clock_started = 0;
	player->has_audio = 0;
	player->video_bitrate_bps = 0;
	memset(&player->video, 0, sizeof(player->video));
	memset(&player->audio, 0, sizeof(player->audio));

	int ret = input_open(&player->video_input, &player->config.stream,
	                     player->cancel);
	if (ret < 0) goto fail;
	int video_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 0);
	int audio_index = find_stream(player->video_input.format,
	                              AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC,
	                              player->config.audio_track);
	if (video_index < 0) {
		ret = AVERROR_DECODER_NOT_FOUND;
		goto fail;
	}
	AVCodecParameters *video_parameters =
	    player->video_input.format->streams[video_index]->codecpar;
	if (video_parameters->bit_rate > 0)
		player->video_bitrate_bps = (uint64_t)video_parameters->bit_rate;
	else if (player->video_input.format->bit_rate > 0)
		player->video_bitrate_bps = (uint64_t)player->video_input.format->bit_rate;
	if (audio_index >= 0) {
		ret = input_open(&player->audio_input, &player->config.stream,
		                 player->cancel);
		if (ret < 0) goto fail;
		audio_index = find_stream(player->audio_input.format,
		                          AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC,
		                          player->config.audio_track);
		if (audio_index < 0) { ret = AVERROR_DECODER_NOT_FOUND; goto fail; }
		player->has_audio = 1;
	}
	player->duration_ms = input_duration_ms(player->video_input.format);
	if (!player->duration_ms && player->has_audio)
		player->duration_ms = input_duration_ms(player->audio_input.format);
	if (player->duration_ms && start_position_ms >= player->duration_ms)
		start_position_ms = 0;

	if (start_position_ms > 0) {
		ret = seek_input_ms(&player->video_input, video_index, start_position_ms);
		if (ret >= 0 && player->has_audio)
			ret = seek_input_ms(&player->audio_input, audio_index, start_position_ms);
		if (ret < 0) {
			/* A stale history point must never turn startup into a linear decode
			 * across a long file. Fall back to a checked beginning seek and report
			 * position zero to both pipelines. */
			seek_input_ms(&player->video_input, video_index, 0);
			if (player->has_audio)
				seek_input_ms(&player->audio_input, audio_index, 0);
			start_position_ms = 0;
		}
	}

	ret = vita_sw_software_video_start(&player->video, player->video_input.format,
	                           video_index, player->config.expected_width,
	                           player->config.expected_height,
	                           player->config.expected_fps, start_position_ms,
	                           player->cancel, &player->start_gate);
	if (ret < 0) goto fail;
	if (player->has_audio) {
		ret = vita_sw_elementary_audio_start(&player->audio,
		                            player->audio_input.format, audio_index,
		                            start_position_ms, player->cancel,
		                            &player->start_gate);
		if (ret < 0) goto fail;
		vita_sw_elementary_audio_set_volume(&player->audio,
		                                    player->config.volume_percent);
		player->audio.paused = player->paused;
	} else player->clock_us = start_position_ms * 1000ULL;
	/* Video is allowed to decode while audio waits on the start gate. A small
	 * presentation cushion prevents the audio clock from outrunning the first
	 * remote frames without adding a fixed startup delay. */
	int target_ready = player->video.source_fps >= 50 ? 8 : 4;
	uint64_t deadline = sceKernelGetProcessTimeWide() + 5000000ULL;
	while (!*player->cancel && !player->video.had_error && !player->video.eof &&
	       sceKernelGetProcessTimeWide() < deadline) {
		int ready = 0;
		vita_sw_software_video_buffer_status(&player->video, &ready, NULL, NULL);
		if (ready >= target_ready) break;
		sceKernelDelayThread(1000);
	}
	if (*player->cancel) { ret = AVERROR_EXIT; goto fail; }
	__sync_synchronize();
	player->start_gate = 1;
	player->opened = 1;
	return 0;

fail:
	close_session(player);
	return ret;
}

VitaSwDecoderPlayer *vita_sw_decoder_create(void) {
	return calloc(1, sizeof(VitaSwDecoderPlayer));
}

int vita_sw_decoder_open(VitaSwDecoderPlayer *player,
	                    const VitaSwDecoderPlayerConfig *config) {
	if (!player || !config || !config->stream.open) return AVERROR(EINVAL);
	close_session(player);
	player->config = *config;
	if (player->config.audio_track < 0) player->config.audio_track = 0;
	if (player->config.volume_percent < 0) player->config.volume_percent = 0;
	if (player->config.volume_percent > 300) player->config.volume_percent = 300;
	player->paused = 0;
	return open_session(player, config->start_position_ms);
}

void vita_sw_decoder_close(VitaSwDecoderPlayer *player) {
	close_session(player);
}

void vita_sw_decoder_destroy(VitaSwDecoderPlayer *player) {
	if (!player) return;
	close_session(player);
	free(player);
}

void vita_sw_decoder_set_paused(VitaSwDecoderPlayer *player, int paused) {
	if (!player) return;
	player->paused = paused != 0;
	player->audio.paused = player->paused;
	player->clock_wall_us = sceKernelGetProcessTimeWide();
}

void vita_sw_decoder_set_volume(VitaSwDecoderPlayer *player, int percent) {
	if (!player) return;
	if (percent < 0) percent = 0;
	if (percent > 300) percent = 300;
	player->config.volume_percent = percent;
	vita_sw_elementary_audio_set_volume(&player->audio, percent);
}

void vita_sw_decoder_request_stop(VitaSwDecoderPlayer *player) {
	if (player && player->cancel) *player->cancel = 1;
}

int vita_sw_decoder_seek(VitaSwDecoderPlayer *player, uint64_t position_ms) {
	if (!player || !player->opened) return AVERROR(EINVAL);
	if (player->duration_ms && position_ms > player->duration_ms)
		position_ms = player->duration_ms;
	int paused = player->paused;
	int ret = restart_session_in_place(player, position_ms);
	if (ret >= 0) return 0;
	close_session(player);
	player->paused = paused;
	return open_session(player, position_ms);
}

int vita_sw_decoder_select_audio_track(VitaSwDecoderPlayer *player,
	                                   int audio_track,
	                                   uint64_t position_ms) {
	if (!player || !player->opened || audio_track < 0) return AVERROR(EINVAL);
	if (audio_track == player->config.audio_track) return 0;
	if (player->duration_ms && position_ms > player->duration_ms)
		position_ms = player->duration_ms;
	int previous_track = player->config.audio_track;
	player->config.audio_track = audio_track;
	int ret = restart_audio_track(player, audio_track, position_ms);
	if (ret >= 0) return 0;
	player->config.audio_track = previous_track;
	restart_audio_track(player, previous_track, position_ms);
	return ret;
}

static uint64_t presentation_clock(VitaSwDecoderPlayer *player) {
	uint64_t now = sceKernelGetProcessTimeWide();
	if (!player->has_audio) {
		if (!player->clock_started) {
			player->clock_started = 1;
			player->clock_wall_us = now;
			return player->clock_us;
		}
		uint64_t delta = now - player->clock_wall_us;
		player->clock_wall_us = now;
		if (!player->paused) player->clock_us += delta;
		return player->clock_us;
	}
	uint64_t raw = vita_sw_elementary_audio_clock_us(&player->audio);
	if (!raw) return 0;
	if (!player->clock_started) {
		player->clock_started = 1;
		player->clock_us = raw;
		player->clock_wall_us = now;
		return raw;
	}
	uint64_t delta = now - player->clock_wall_us;
	player->clock_wall_us = now;
	if (!player->paused) player->clock_us += delta;
	uint64_t minimum = raw > VITA_SW_DECODER_CLOCK_WINDOW_US
	                 ? raw - VITA_SW_DECODER_CLOCK_WINDOW_US : 0;
	uint64_t maximum = raw + VITA_SW_DECODER_CLOCK_WINDOW_US;
	if (player->clock_us < minimum) player->clock_us = minimum;
	if (player->clock_us > maximum) player->clock_us = maximum;
	return player->clock_us;
}

int vita_sw_decoder_present(VitaSwDecoderPlayer *player, int fill_screen) {
	if (!player || !player->opened) return 0;
	uint64_t clock = presentation_clock(player);
	uint64_t raw = player->has_audio
	             ? vita_sw_elementary_audio_clock_us(&player->audio) : clock;
	return vita_sw_software_video_present(&player->video, clock, raw,
	                              fill_screen, !player->paused);
}

int vita_sw_decoder_present_rect(VitaSwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect) {
	if (!player || !player->opened) return 0;
	uint64_t clock = presentation_clock(player);
	uint64_t raw = player->has_audio
	             ? vita_sw_elementary_audio_clock_us(&player->audio) : clock;
	return vita_sw_software_video_present_rect(&player->video,
	                                   clock, raw,
	                                   x, y, width, height, fill_rect,
	                                   !player->paused);
}

void vita_sw_decoder_render_complete(VitaSwDecoderPlayer *player) {
	if (player && player->opened) vita_sw_software_video_render_complete(&player->video);
}

void vita_sw_decoder_get_status(VitaSwDecoderPlayer *player,
	                           VitaSwDecoderPlayerStatus *status) {
	if (!status) return;
	memset(status, 0, sizeof(*status));
	if (!player) return;
	status->opened = player->opened;
	status->paused = player->paused;
	status->eof = player->video.eof &&
	              (!player->has_audio || player->audio.eof);
	status->error = player->video.had_error ||
	                (player->has_audio && player->audio.had_error);
	status->width = player->video.content_width;
	status->height = player->video.content_height;
	status->video_bitrate_bps = player->video_bitrate_bps;
	status->duration_ms = player->duration_ms;
	status->position_ms = (player->has_audio
	    ? vita_sw_elementary_audio_clock_us(&player->audio)
	    : player->clock_us) / 1000ULL;
	SoftwareVideoDebugSnapshot debug;
	vita_sw_software_video_debug_snapshot(&player->video, &debug);
	status->hardware_accelerated = debug.hardware_accelerated;
	status->direct_rendering = debug.direct_rendering;
	status->ready_frames = debug.ready_frames;
	status->frame_capacity = debug.queue_capacity;
	status->fps = debug.source_fps;
	status->frames_decoded = debug.frames_decoded;
	status->frames_shown = debug.frames_shown;
	status->frames_dropped = debug.frames_dropped;
}
