#ifndef VITA_DECODER_INTERNAL_INPUT_TIMING_H
#define VITA_DECODER_INTERNAL_INPUT_TIMING_H
#include <stdint.h>
#include <limits.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>

/* Indexed MOV/MP4 exposes track durations at read_header time. The aggregate
 * format duration is normally populated later by find_stream_info, which our
 * fast path deliberately avoids. Recover the same A/V timeline from metadata;
 * never infer a duration from bitrate or scan all packets during startup. */
static uint64_t decoder_input_duration_ms(const AVFormatContext *format) {
    if (!format) return 0;
    const AVRational ms = {1, 1000};
    if (format->duration > 0)
        return (uint64_t)av_rescale_q_rnd(format->duration, AV_TIME_BASE_Q, ms, AV_ROUND_DOWN);
    int64_t origin = format->start_time;
    if (origin == AV_NOPTS_VALUE) {
        for (unsigned i = 0; i < format->nb_streams; i++) {
            const AVStream *s = format->streams[i];
            if ((s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                 s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                 s->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) ||
                (s->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
                s->start_time == AV_NOPTS_VALUE || s->time_base.num <= 0 || s->time_base.den <= 0)
                continue;
            int64_t start = av_rescale_q(s->start_time, s->time_base, AV_TIME_BASE_Q);
            if (origin == AV_NOPTS_VALUE || start < origin) origin = start;
        }
    }
    if (origin == AV_NOPTS_VALUE) origin = 0;
    int64_t latest = 0;
    for (unsigned i = 0; i < format->nb_streams; i++) {
        const AVStream *s = format->streams[i];
        if ((s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
             s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) ||
            (s->disposition & AV_DISPOSITION_ATTACHED_PIC) || s->duration <= 0 ||
            s->time_base.num <= 0 || s->time_base.den <= 0) continue;
        int64_t duration = av_rescale_q(s->duration, s->time_base, AV_TIME_BASE_Q);
        int64_t start = s->start_time != AV_NOPTS_VALUE
                      ? av_rescale_q(s->start_time, s->time_base, AV_TIME_BASE_Q) : origin;
        if (duration <= 0 || start > INT64_MAX - duration) continue;
        int64_t end = start + duration;
        if (end <= origin || (origin < 0 && end > INT64_MAX + origin)) continue;
        int64_t span = end - origin;
        if (span > latest) latest = span;
    }
    return latest > 0 ? (uint64_t)av_rescale_q_rnd(latest, AV_TIME_BASE_Q, ms, AV_ROUND_DOWN) : 0;
}
#endif
