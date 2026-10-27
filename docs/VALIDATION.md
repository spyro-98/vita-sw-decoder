# vita-sw-decoder: build, validation and release

Current package version **2.0.0**, app integration **VitaMediaDeck 1.4.1**.
Runtime/API unchanged in this documentation refresh. Pin the resulting Git
revision in the embedding app; keep `VERSION`, CMake and ABI versions at 2.0.0.

## Native build

```sh
export VITASDK=/path/to/vitasdk
./tools/build-ffmpeg.sh
cmake -S . -B build/package \
  -DVITA_SW_DECODER_FFMPEG_ROOT="$PWD/build/deps/ffmpeg-vita-sw" \
  -DVITA_SW_DECODER_BUILD_EXAMPLE=ON
cmake --build build/package -j 4
cmake --install build/package --prefix "$PWD/build/stage"
./tools/release-audit.sh
```

The HW builder pins patched FFmpeg n6.0/wiliwili; the SW standalone builder pins
FFmpeg 8.1.2. VitaMediaDeck instead points both packages at its shared patched
n6.0 archives. A SW standalone release audit intentionally rejects h264_vita
symbols and expects the 8.1.2 source archive; the app's n6.0 integration uses
the app audit, not a claim that this distinct standalone audit passed.

## Host integration regressions

With sibling VitaMediaDeck and a configured FFmpeg 6 host build, run from the
app repository:

```sh
VMD_HOST_FFMPEG=/path/to/ffmpeg6 bash tests/decoder/run-host-tests.sh
VMD_HOST_FFMPEG=/path/to/ffmpeg6 bash tests/playback_seek/run-host-tests.sh
VMD_HOST_FFMPEG=/path/to/ffmpeg6 bash tests/player_feedback/run-host-tests.sh
```

These cover bounded copy/padding, queue notification/lifetime substitutes,
MP4/MKV duration, selected-track timing, subtitle seek scheduling and frozen-frame
restart ownership. Host HW tests substitute SceVideodec and cannot establish
real Vita decoder compatibility, AudioOut output, GXM behavior, FPS or power.

## 27 October integration result

VitaMediaDeck's standard 1.4.1 build at app commit `5419ffe` passed host lifecycle,
HLS/DASH, decoder-wrapper and seek/subtitle regressions, plus native Release/LTO
and import/license audits. The user then confirmed successful IPTV HW playback,
including sources described as 1080p. This is integrated-app HW feedback;
selected rendition, FPS, clocks and energy were not independently measured.
This is not new SW playback/performance evidence. The CPU backend's
implementation and standalone FFmpeg pin are unchanged.

See the [app report](https://github.com/spyro-98/VitaMediaDeck/blob/feature/iptv/docs/iptv-hw-startup-2026-10-27.md) for the exact tested VPK hash and evidence boundaries.

## Physical acceptance

Use legally available known files and report commit, FFmpeg prefix/version,
backend, source/transport, codec profile/resolution/fps, selected audio/subtitles,
actual clock readback and diagnostics. Test open/pause/resume, both seek
directions, near EOF, audio switch/rollback, cancellation, repeated shutdown,
no-audio media, malformed inputs and live reconnect. Check A/V landing, stalled
I/O, retained display surfaces and repeated session resource lifetime. Never
publish credentials, URL tokens or raw private-media dumps.

Existing app evidence is documented in [performance](PERFORMANCE.md); fresh
native builds/source inspection do not extend it to untested material.

## Source release

After intentional commits and a clean tree, `./tools/package-source.sh` packages
the current HEAD as a 2.0.0 source archive. Include the actual linked FFmpeg
corresponding source, license bundle, dependency lock and SHA-256 with binaries.
The packaging/audit commands do not create GitHub releases, tags or uploads.
