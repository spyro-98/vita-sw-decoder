#ifndef VITA_SW_DECODER_LIVE_H
#define VITA_SW_DECODER_LIVE_H
#include "vita_sw_decoder.h"

/* Additive, opt-in ABI. Existing config/stream/status layouts and open()
 * behavior are unchanged. V1 accepts forward-only multiplexed MPEG-TS,
 * H.264 with mono/stereo AAC. Transport owns buffering and reconnection.
 * Every cursor must start at the SAME retained segment/timeline.
 * read blocks cooperatively until bytes, final EOF or an error; temporary
 * starvation must not return zero. abort must wake a blocked read promptly.
 * Discontinuity/eviction requires closing BOTH pipelines and reopening.
 * Seeking and audio track changes return ENOSYS without mutating playback. */
typedef struct VitaSwDecoderLiveOptions {
    uint32_t size;
    uint32_t version; /* 1: original contract; 2: explicit decoded AAC shape */
    union {
        uint32_t reserved[2]; /* V1 must be zero; layout remains 16 bytes. */
        struct { uint32_t sample_rate, channels; } audio; /* V2 only */
    };
} VitaSwDecoderLiveOptions;

int vita_sw_decoder_open_live(VitaSwDecoderPlayer *player,
    const VitaSwDecoderPlayerConfig *config,
    const VitaSwDecoderLiveOptions *options);
#endif
