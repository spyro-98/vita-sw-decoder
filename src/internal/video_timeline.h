#ifndef VMD_VIDEO_TIMELINE_H
#define VMD_VIDEO_TIMELINE_H
#include <stdint.h>
/* Called after rescaling into microseconds. The legacy path passes origin=0. */
static inline uint64_t video_timeline_us(int64_t timestamp, int64_t origin) {
    if ((origin > 0 && timestamp < INT64_MIN + origin) ||
        (origin < 0 && timestamp > INT64_MAX + origin)) return 0;
    timestamp -= origin;
    return timestamp < 0 ? 0 : (uint64_t)timestamp;
}
#endif
