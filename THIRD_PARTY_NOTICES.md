# vita-sw-decoder third-party notices

The module is distributed under GPL-3.0-only; see `LICENSE`.

It is designed for the pinned FFmpeg software H.264 build. The selected FFmpeg
configuration is LGPL-2.1-or-later. Embedding applications must provide the
corresponding source, license texts, and VitaSDK port notices when distributing
binaries.
`tools/build-ffmpeg.sh` preserves the verified upstream source tarball beside
the generated archive so a binary release can include corresponding source.

- https://github.com/FFmpeg/FFmpeg
- https://vitasdk.org/

## VitaMediaDeck 1.4.1 integration

The app links both decoder packages against the same patched FFmpeg n6.0
archives. The SW standalone builder's FFmpeg 8.1.2 source/hash is a separate
configuration. Distribute notices and corresponding source matching the actual
linked archives; do not label the app binary FFmpeg 8.1.2 merely because the SW
standalone dependency lock lists it. The documentation refresh changes no
license, dependency pin or redistribution obligation. See
[build and validation](docs/VALIDATION.md).
