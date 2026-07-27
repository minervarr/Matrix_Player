# TODO — Matrix Player Windows

## Audio Engine
- [ ] Wire `UsbAudioDriver::open(0x32BB, 0x0004)` into `PlayerWindow::onPlay()`
- [ ] Add USB device picker dropdown (enumerate libusb devices by VID/PID)
- [ ] Feed `Decoder` PCM output into `UsbAudioDriver::writeFloat32()`
- [ ] Sync seekbar position from actual USB audio playback position
- [ ] **DoP**: Port `DsdPackager.java` to C++, add `writeDop()` to `UsbAudioDriver`
      See: `libs/firstparty/audio_engine/src/main/java/com/nerio/audioengine/DsdPackager.java`

## Decoder
- [ ] Parse Vorbis comment tags from FLAC (title, artist, album, duration, track number)
      `dr_flac` provides `drflac_get_vorbis_comment_iterator` for this.
- [ ] Swap `dr_flac` for **FFmpeg** when adding more formats (DSF/DFF, WAV, MP3, OPUS)
      Keep `Decoder` interface unchanged — just swap the `Impl`.

## Library / Scanner
- [ ] Parse `durationMs`, `sampleRate`, `bitDepth` from FLAC header during scan
- [ ] **Parallel scan**: Use `std::thread` pool (one per `std::thread::hardware_concurrency()`)
      mirroring Android's `ExecutorService.newFixedThreadPool(availableProcessors())`
- [ ] Add SQLite scan cache: store `file_size + mtime` to skip unchanged files on rescan
      See: Android player's hybrid scan in `MainActivity.java` lines 860–989
- [ ] Support DSF / DFF file extensions once FFmpeg decoder is in

## Database
- [ ] Add `play_history` table (track_id, played_at timestamp)
- [ ] Add `track_stats` table (play_count, skip_count, listen_time_ms)
- [ ] Add `playback_state` table (last track, position, volume) for resume on launch
- [ ] Decide: stay with SQLite or evaluate DuckDB for analytics queries on large libraries

## Artwork
- [ ] Replace GDI+ JPEG decode with **libjpeg-turbo** (same as Android player's `NativeImageDecoder`)
      Significant speed difference on 4K displays with large cover files.
- [ ] Add dimension check when multiple candidate images exist — pick highest resolution
- [ ] Add LRU bitmap cache: thumbnail key + fullscreen key, separate bitmaps
      See: `ArtworkCache.java` (L1 LRU, L2 disk, L3 source resolution)
- [ ] Add 7-day disk cache for online artwork (Tidal/Qobuz CDN URLs, future)

## UI / UX
- [ ] **Smarter title/modifier handling** (user request 2026-07-09): the current
      `splitNameModifier()` only peels trailing `(...)`/`[...]` groups. Future ideas:
      classify modifier *kinds* (deluxe/reissue vs. feat. credits vs. "from the
      series ..." source tags vs. classical work/movement suffixes) and style or
      hide each kind differently; option to collapse modifiers entirely in dense
      views and reveal on hover. Classical music (very long structured names) is
      the hard case to design for.
- [ ] Apply dark theme with green accent (#00C853) matching Android player
      Use `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` for title bar
- [ ] Borderless window with custom chrome (drawn in WM_PAINT / WM_NCCALCSIZE)
- [ ] Album grid view (thumbnail + name) instead of plain listbox
- [ ] Track number sorting within albums
- [ ] `Keep screen on` in ArtWindow: `SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_CONTINUOUS)`
- [ ] Keyboard shortcuts: Space=play/pause, Left/Right=seek 10s, F=fullscreen art
- [ ] Restyle warning/alert UI toward a more alarming sci-fi/HUD treatment
      (neon-red, hazard framing) — currently a plain amber banner (see
      `CLR_WARNING`); user has a specific reference mood-board in mind.

## Architecture / Future
- [ ] EQ processor: port `eq_processor.h` (biquad SIMD) for Windows use
      Already in `libs/firstparty/audio_engine/src/main/cpp/eq_processor.h` — no JNI needed on Windows
- [ ] Audio server concept: background Windows service that owns the USB DAC,
      accepts PCM via named pipe so mpv and other apps can use the libusbK path
- [ ] Gapless playback: pre-decode next track while current plays (mirror `NativeGaplessDecoder`)
- [ ] DSD mode selector: Native / DoP / PCM fallback (mirrors `DsdMode.java`)
