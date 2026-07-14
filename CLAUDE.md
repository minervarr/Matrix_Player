# Matrix Player Windows — CLAUDE.md

## What this project is

A native Windows C++ music player that drives a USB DAC **directly** via libusbK,
bypassing WASAPI and the Windows audio stack entirely. Audio goes from file → decoder
→ ring buffer → isochronous USB transfers to the DAC. No OS audio mixer involved.

This is the Windows sibling of an Android music player
(`C:\Users\incxiuefb\Documents\Files\clone\media_player`) which has a rich reference
implementation. When in doubt about a feature (artwork loading, gapless, scan strategy,
EQ), read the Android player's Java/C++ code — the architecture maps 1:1.

---

## Repository layout

```
windows_matrix_player/
  src/
    main.cpp            — WinMain, CoInit, launches PlayerWindow
    player_window.h/cpp — Main Win32 window: album list, track list, seekbar, art thumb
    art_window.h/cpp    — Separate fullscreen artwork window (dual-monitor friendly)
    library.h/cpp       — Folder scan, Track/Album structs, art resolution
    decoder.h/cpp       — dr_flac wrapper with async decode thread + seek
    db.h/cpp            — SQLite persistence (tracks, albums tables)
  libs/
    firstparty/
      audio_engine/     — git submodule → github.com/minervarr/audio_engine (our own code)
    thirdparty/
      dr_flac.h         — Single-header FLAC decoder (mackron/dr_libs)
      sqlite3.h / .c    — SQLite amalgamation 3.46.1 (vendored, not a submodule)
      soxr/             — git submodule → github.com/chirlu/soxr (resampler)
  build.bat             — One-shot build: loads MSVC vcvars64, runs cmake+ninja
  CMakeLists.txt        — Ninja + MSVC, C++17, links audio_engine_windows.lib
  TODO.md               — Prioritized feature backlog
  .gitignore            — Excludes build/, .db, .vs/
```

---

## Build

Requires: Visual Studio Build Tools (MSVC cl.exe), CMake, Ninja, libusbK driver
on the target USB DAC.

```bat
build.bat
```

Output: `build\matrix_player_windows.exe`

The build pulls in `libs/firstparty/audio_engine` via submodule. If submodule is empty:
```bat
git submodule update --init --recursive
```

The MSVC path in `build.bat` is:
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat`

---

## Audio engine (`libs/firstparty/audio_engine/`)

The submodule lives at `C:\Users\incxiuefb\Documents\Files\clone\audio_engine`.
It is a **C++ static library** (`audio_engine_windows.lib`) that drives USB Audio Class
devices (UAC1/UAC2) directly via libusbK, bypassing WASAPI.

### Key class: `UsbAudioDriver` (`libs/firstparty/audio_engine/src/main/cpp/usb_audio.h`)

```cpp
UsbAudioDriver driver;
driver.open(0x32BB, 0x0004);   // Hiby FC4: VID=0x32BB PID=0x0004
driver.parseDescriptors();
driver.configure(44100, 2, 16);
driver.start();
driver.writeFloat32(pcmData, numSamples);  // call from decode loop
driver.stop();
driver.close();
```

**Windows open signature differs from Android:**
- Android: `open(int fd)` — receives file descriptor from UsbManager
- Windows:  `open(uint16_t vid, uint16_t pid)` — opens by VID/PID via libusb

### Tested device
- Hiby FC4 — VID `0x32BB`, PID `0x0004`, UAC2, High-Speed USB
- MI_00 (interface 0) must have **libusbK** bound via Zadig
- Supports: 44.1k–768kHz PCM, 16/24/32-bit, DSD native (alt=4)

### Audio formats the engine supports
- PCM: `writeFloat32()`, `writeInt16()`, `writeInt24Packed()`, `writeInt32()`
- Native DSD: `configure(rate, 2, 32, preferDsd=true)` then `write()` raw bytes
- DoP: **TODO** — needs C++ port of `DsdPackager.java`
  (see `libs/firstparty/audio_engine/src/main/java/com/nerio/audioengine/DsdPackager.java`)

---

## Current skeleton state

### What works (skeleton)
- Win32 window with album list, track list, seekbar, time label, art thumbnail
- Folder picker → recursive FLAC scan → SQLite persistence
- Album art resolution: cover.jpg/folder.jpg priority → single image fallback
- GDI+ image loading scaled to fit (thumbnail 180×180, fullscreen on separate window)
- dr_flac async decode with seek support
- Fullscreen art window (Escape or double-click to close, multi-monitor aware)

### What is NOT yet wired (most important next steps)
1. **USB audio output** — `PlayerWindow::onPlay()` has a silent PCM sink.
   Wire `Decoder`'s `PcmCallback` into `UsbAudioDriver::writeFloat32()`.
   See `TODO(engine)` comment in `player_window.h`.

2. **FLAC metadata** — `library.cpp:quickParseFLAC()` only reads filename as title.
   Use `drflac_get_vorbis_comment_iterator` to parse title/artist/album/duration.

3. **Seekbar position** — currently reads trackbar position, not actual playback time.
   Wire from `UsbAudioDriver` once audio output is connected.

---

## Design decisions (don't change without reason)

| Decision | Choice | Why |
|---|---|---|
| GUI | Win32 raw | No framework overhead, user owns every pixel |
| FLAC decoder | dr_flac (single-header) | Zero deps, SIMD fast, swap for FFmpeg later |
| Other formats | TODO via FFmpeg | dr_flac first, FFmpeg when DSF/WAV/MP3 needed |
| DB | SQLite (embedded) | Same model as Android player, single file |
| USB driver | libusbK | Best isochronous support on Windows, FOSS (LGPL) |
| Audio stack | Bypassed entirely | No WASAPI — raw USB isochronous to DAC |
| Album art (fullscreen) | Separate window | Dual-monitor: art on one screen, controls on other |
| Submodule | audio_engine only | dr_flac + sqlite3 vendored (designed for it) |
| Build | CMake + Ninja + MSVC cl.exe | No .sln files, matches audio_engine build pattern |

---

## Reference: Android player

`C:\Users\incxiuefb\Documents\Files\clone\media_player` — study this for:
- Parallel scan strategy (`MainActivity.java` lines 860–989)
- Three-tier artwork cache (`ArtworkCache.java`)
- Gapless decode pipeline (`AudioEngine.java`, `NativeGaplessDecoder`)
- DSD mode handling (`DsdMode.java`, `DsdPackager.java`)
- EQ biquad implementation (`eq_processor.h`) — already in `libs/firstparty/audio_engine`, no port needed
- Play history / stats schema (`TrackDao.java`, `StatsDao.java`)

---

## Key TODOs (see TODO.md for full list)

1. Wire USB audio output into play button
2. Parse FLAC Vorbis comment tags for metadata
3. Port `DsdPackager.java` → C++ `writeDop()` in `UsbAudioDriver`
4. Swap GDI+ JPEG for libjpeg-turbo (same as Android's `NativeImageDecoder`)
5. Parallel folder scan (std::thread pool, one per CPU core)
6. Dark theme + green accent (#00C853)
