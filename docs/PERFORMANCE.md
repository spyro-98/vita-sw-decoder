# vita-sw-decoder: performance and evidence

Source baseline is ABI 2.0.0; app integration 1.4.1. No new runtime algorithm,
codec, clock default or battery measurement is introduced by this doc refresh.

## Implemented in the decoder baseline

- Kernel event notification for frame/surface availability, with bounded waits
  and stop handling; readiness is rechecked around notification.
- Copy only needed YUV rows and deterministic padding, rather than clear every
  complete surface on every frame. Preserve crop/pitch and GXM plane layout.
- MP4 duration fallback from finite A/V stream timing and indexed seek/preroll.
- Diagnostics separate decode, copy, queue/surface wait, startup and seek work;
  dropped counters include preroll and are not alone a playback loss metric.

## Software path and options

| Option | Default | Effect / boundary |
| --- | --- | --- |
| VITA_SW_DECODER_CPU_THREADS | 3 | 1..3 workers for seekable H.264; forward-only input forces one |
| VITA_SW_DECODER_PLANAR_UPLOAD | OFF | Optional copied YUV420P3 GXM upload; requires comparative console tests |

Normal output is copied NV12/P2 sampled as YVU420P2. Planar output changes plane
layout/upload, not the H.264 codec. CPU zero-copy remains disabled: FFmpeg frame
workers may retain reference buffers beyond a safe display slot lifetime.
Existing late-frame/non-reference catch-up policies are not a claim of identical
quality at all loads. Do not infer power savings from worker count or fewer copies.

## Physical results already recorded

VitaMediaDeck PlayerFix reached about 23.9 fps on tested 960x544/24 material with
P2/P3. A P3 1280x720/30 sample reached 11.4 fps with about 2.6 seconds lag; the user
also observed SW 720p30 stutter at CPU 333 MHz. Two SW starts/seeks timed out
under that load and Auto recovered hardware. Increasing only the timeout would
not make sustained playback real-time. P3 throughput, quality and energy
superiority are unproven. These are old test sessions, not new 1.4.1 measurements.

The app's standard clock request remains 444/222/222/111 MHz when app control is
selected. API readback must be measured; requested clocks are not evidence of
actual clocks. PSVshell mode leaves control external. A physical comparison
requires the same file, intervals, backend, clocks, temperature, audio/subtitles
and UI load, plus throughput, dropped pictures, lag, memory and battery data.

The detailed evidence is in VitaMediaDeck's
[PlayerFix report](https://github.com/spyro-98/VitaMediaDeck/blob/feature/iptv/docs/console-validation-playerfix-2026-10-23.md).
The paper/algorithm research is historical candidate work, not implemented speed
or energy gains. See [validation](VALIDATION.md) for reproducible checks.
