# vita-sw-decoder: runtime contract

Reviewed against the current 2.0.0 source for VitaMediaDeck 1.4.1, 23 October 2026.
This package owns demux, H.264/AAC decoding, timing and surfaces. The app owns UI,
providers, clocks, subtitle packets, preview jobs and fallback policy.

## Factory and cancellation

Zero/designated-initialize every 2.x factory, handle and config. Rebuild all
consumers after the 1.x to 2.x layout change. Each factory open returns an
independent cursor. Video/audio cursors are opened serially; the contract does
not promise thread-safe concurrent factory calls.

A seekable input supplies read, seek, exact size and close. Optional
`open_with_cancel` handles operation cancellation; handle `abort` is non-owning
and must return promptly. The decoder publishes active I/O while invoking it.
Ordinary restart grants up to 30 ms cooperative grace; explicit stop interrupts
immediately. An aborted cursor is reopened before reuse. The decoder does not
clear a caller's cancellation flag; cancellation suppresses expensive recovery.

## Presentation and restart

Use the ordering in [the example](../examples/local_file.c): start frame, present,
end drawing, `vita2d_wait_rendering_done()`, `vita_sw_decoder_render_complete()`,
then swap. Surface release must follow the GXM fence. Before background restart
follow the app's owner-side fence and `vita_sw_decoder_prepare_background_restart()`.
No worker may free a surface still referenced by a submitted draw.

Finite-file startup/resume/seek use the selected H.264 stream as byte-index
anchor, preserve reference packets during preroll and validate video/audio
landing within 250 ms before releasing the shared gate. A seek at end is
clamped inside the stream tail. Non-cancellation failures may require checked
fresh-session recovery. Audio replacement rolls back to the previous track
when possible; honor the documented positive rollback result.

Duration first uses container timing, then finite playable audio/video stream
timing. Attachment/cover/subtitle timing must not inflate the media duration.
This enables seek/HUD duration on compatible MP4s with missing container duration.
Non-seekable streams do not gain random access from that calculation.

Eight-grain PCM queue, dedicated 1024-frame AudioOut feeder and the accepted
audio sample clock remain separate from video work. Clean audio EOF transfers
clock ownership without dropping the remaining video tail.

## Live API

`vita_sw_decoder_open_live()` is opt-in, not a replacement for file open. Two
forward-only MPEG-TS cursors must start at the same retained segment; starvation
blocks cooperatively, rather than reporting EOF. App transport bounds memory and
waiting. Video and audio share the demux PTS origin. Compatible H.264/AAC is
limited to 1280x720; seek and track changes return ENOSYS without closing play.

V1 options remain supported with reserved words zero. V2 reuses the same 16-byte
layout for explicit decoded AAC rate/channels on app-remuxed streams. The app
owns HLS/DASH, playlist duration, reconnect, discontinuity and live pause policy.

## Boundaries

Subtitle selection/rearming after seek, camera QR, UI themes, status bar and
mini-player navigation are VitaMediaDeck responsibilities. Their 1.4.1 changes
do not change this package's ABI. A file named MP4/MKV does not make an unsupported
codec, encrypted stream or non-seekable transport playable. See the main
[README](../README.md) for API examples and [performance](PERFORMANCE.md) for limits.
