#ifndef VITA_SW_DECODER_H
#define VITA_SW_DECODER_H

#include <stddef.h>
#include <stdint.h>

/* A factory must create a new independent cursor every time open() is called.
 * The player opens two cursors for a muxed file so audio and video can demux
 * concurrently without sharing seek state. */
typedef struct VitaSwDecoderStreamHandle {
	void *opaque;
	int (*read)(void *opaque, void *buffer, size_t size);
	int64_t (*seek)(void *opaque, int64_t offset, int whence);
	void (*close)(void *opaque);
	int64_t size;
} VitaSwDecoderStreamHandle;

typedef struct VitaSwDecoderStreamFactory {
	void *opaque;
	int (*open)(void *opaque, VitaSwDecoderStreamHandle *out);
} VitaSwDecoderStreamFactory;

typedef struct VitaSwDecoderPlayer VitaSwDecoderPlayer;

typedef struct VitaSwDecoderPlayerConfig {
	VitaSwDecoderStreamFactory stream;
	uint32_t expected_width;
	uint32_t expected_height;
	int expected_fps;
	uint64_t start_position_ms;
	int volume_percent;
	/* Optional cooperative cancellation flag used during remote opens and
	 * decode. The caller must keep it alive until close returns. */
	volatile int *cancel_flag;
} VitaSwDecoderPlayerConfig;

typedef struct VitaSwDecoderPlayerStatus {
	int opened;
	int paused;
	int eof;
	int error;
	int hardware_accelerated;
	int direct_rendering;
	int ready_frames;
	int frame_capacity;
	int fps;
	uint32_t width;
	uint32_t height;
	uint64_t position_ms;
	uint64_t duration_ms;
	unsigned int frames_decoded;
	unsigned int frames_shown;
	unsigned int frames_dropped;
} VitaSwDecoderPlayerStatus;

/* Convenience factory for a normal Vita path. The path must remain valid
 * until the player is closed. */
void vita_sw_decoder_file_stream_factory(const char *path,
	                             VitaSwDecoderStreamFactory *factory);

/* Loads the packaged decoder compatibility runtime once for the process.
 * Normal player open calls prepare it automatically; embedding applications
 * may call this before creating a separate SceAvPlayer instance. */
int vita_sw_decoder_prepare_runtime(void);
const char *vita_sw_decoder_backend_name(void);

VitaSwDecoderPlayer *vita_sw_decoder_create(void);
int vita_sw_decoder_open(VitaSwDecoderPlayer *player,
	                    const VitaSwDecoderPlayerConfig *config);
void vita_sw_decoder_close(VitaSwDecoderPlayer *player);
void vita_sw_decoder_destroy(VitaSwDecoderPlayer *player);

void vita_sw_decoder_set_paused(VitaSwDecoderPlayer *player, int paused);
void vita_sw_decoder_set_volume(VitaSwDecoderPlayer *player, int percent);
void vita_sw_decoder_request_stop(VitaSwDecoderPlayer *player);

/* Reopens both independent cursors at the requested media timestamp. */
int vita_sw_decoder_seek(VitaSwDecoderPlayer *player, uint64_t position_ms);

/* Call present() inside a vita2d drawing scene, then render_complete() after
 * vita2d_wait_rendering_done(). */
int vita_sw_decoder_present(VitaSwDecoderPlayer *player, int fill_screen);
int vita_sw_decoder_present_rect(VitaSwDecoderPlayer *player,
	                            float x, float y, float width, float height,
	                            int fill_rect);
void vita_sw_decoder_render_complete(VitaSwDecoderPlayer *player);
void vita_sw_decoder_get_status(VitaSwDecoderPlayer *player,
	                           VitaSwDecoderPlayerStatus *status);

#endif
