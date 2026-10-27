# Change record

## 2026-10-27 — Integrated IPTV HW acceptance and scope

- Record user-confirmed IPTV HW playback in VitaMediaDeck 1.4.1 after app commit
  `5419ffe`, including sources described as 1080p without asserting the selected
  rendition's measured resolution or FPS.
- Distinguish the app HLS timestamp fix and app-built FFmpeg hardware lifecycle
  patch from the unchanged standalone package code, ABI and dependency pins.
- Update app clock policy documentation: configurable HW333/SW444 defaults and
  priority for external PSVshell control; neither decoder sets system clocks.
- Keep the successful HW retest separate from SW throughput claims and retain
  the existing software performance limits.

## 2026-10-23 — VitaMediaDeck 1.4.1 documentation alignment

- Document current runtime/ownership/cancellation, MP4 timing and live API limits.
- Explain default/experimental performance choices and actual clock evidence.
- Separate app FFmpeg n6.0 integration from SW standalone 8.1.2 packaging.
- Add build/validation/console/source-release guides and update contribution,
  security and provenance guidance. Runtime source, API/ABI 2.0.0 and dependency
  pins are unchanged by this refresh.

## Existing 2.0.0 implementation baseline

2.x factory cancellation/abort ABI; independent seekable cursors; H.264/AAC
track metadata; indexed seek/resume and audio replacement/rollback; bounded PCM
queue; optional forward-only MPEG-TS live entry point; direct/copy surface
lifetime fixes, event waits, padding/copy efficiency and MP4 duration fallback.
These were already present before the documentation-only integration commit.
