# Android native port

> **SUPERSEDED IN PART, 2026-08-14.** This document's central design decision —
> that Android needs its own UI class (`AndroidPlayerView`) and that
> `AndroidHost` must NOT implement `Host` — was reversed. `AndroidPlayerView`
> is deleted; the phone runs the desktop `PlayerWindow` through a third real
> `Host`. See `2026-08-14-android-runs-the-real-app.md` for what was actually
> measured and why the premise did not survive it.
>
> The document is kept as written, not edited to match. Everything else in it
> still holds and is still the reference: the zero-Java rule, the JNI pattern,
> the AAudio/decoder decisions, `app_paths` for Android, the safe-area and
> immersive handling, and the build-system findings. A spec with its history
> rewritten stops being able to explain how the project got here.

## Context

Matrix Player is Windows + Linux today, both real GUI builds from one source
tree (`core/` portable logic, `gui/` per-OS `Host` + drawing, the two
first-party engines `framework/audio_engine` and `framework/vk_canvas`
consumed as submodules). The user wants a third equal peer: a native Android
build of the same player, offline-only (no streaming, no network), not
intended for the Play Store (personal sideload install), and — the hard
requirement — **zero Java written by this project**. Every existing OS-forced
JNI touch point must go through the pattern already established in this repo
(a JNI call from C++ into a system-provided Java object; never a `.java` file
of our own).

Two things make this tractable rather than a from-scratch mobile app:

1. **`framework/audio_engine` already has a working Android backend.**
   Phase 2 (per its own `CLAUDE.md`) shipped `AAudioSink`/`AAudioSource`
   (`backends/aaudio/`), `MediaCodecDecoder` (`backends/mediacodec/`), native
   FLAC/MP3 decode, DSD/DoP, all `ae::Engine` (`core/`) + C++ backends — no
   `.java` inside the engine itself.
2. **`framework/vk_canvas` already has a working Android backend.**
   `platform/android/` is a real `NativeActivity` app (`main.cc`, `app.cc`,
   `android_platform.cc`, Gradle wrapper) that renders through `vk_canvas_core`
   the same way the Windows/Linux backends do.

Neither of those is directly reusable as-is (see "What is NOT reused" below),
but both mean the hard 90% — bit-perfect USB/AAudio output, native decode,
Vulkan rendering on Android — is proven working code, not something this port
invents.

### Reference project: `C:\Users\nava\Documents\Programs\media_player`

An older, separate Android music player by the same author, reviewed directly
(not from memory) as part of this design. It is **entirely Java**
(`MainActivity` + `Fragment`s, native Android `MediaPlayer`, `MediaStore`
scanning, XML/`ConstraintLayout` UI) — architecturally the opposite of this
port. It is not ported from; it was inspected for exactly one thing: how it
got broad filesystem access on modern Android (see "Storage" below). Its
release-type classification logic is superseded by `core/variants.cpp`, which
already does the same job with tests behind it.

## Non-goals

- Play Store distribution, in-app purchases, or any of the reference app's
  streaming integrations (`kawusapi`/Qobuz/Tidal) — offline library playback
  only, matching the rest of this project's `MATRIX_ISO_TEST`-style bit-perfect
  focus.
- Feature-dropping relative to desktop. Every engine capability already wired
  into `gui/` (bit-perfect USB, Reference EQ, gapless, DSD/DoP, listening
  analytics, driver AutoEQ profiles, playlists) is in scope for Android too —
  nothing here is a "mobile-lite" cut-down.
- Building or verifying the code. No Android SDK/NDK/Gradle toolchain is
  installed in this environment (confirmed by the user). Implementation work
  proceeds as complete, best-effort C++ following this repo's existing
  conventions exactly (mirroring the Windows/Linux backend files line-for-line
  in structure), but is **unbuilt and unverified** until a session with the
  toolchain available compiles it. See "Build tooling gap" below.

## What is NOT reused as-is

- **`framework/audio_engine/platform/android/`** — its Gradle/CMake project
  builds one big `SHARED` library (`matrix_audio`) that bundles `ae::Engine`
  *and* the C ABI wrapper (`api/src/audio_engine.cpp`) together, because it
  exists for a Java host app to call over JNI (the old reference project's
  model). This port's own native code is C++, so there is no language boundary
  between "app" and "engine" to cross — it links `ae_core`/`ae_usb`/etc.
  directly, the same way `gui/CMakeLists.txt` does on desktop, and never
  touches `api/`. This existing project is left untouched; it remains valid
  for anyone who *does* want a Java-consumable AAR.
- **`framework/vk_canvas/platform/android/`'s `app.cc`/`main.cc`** — these are
  vk_canvas's own test-scene demo, not app code. This port writes its own
  `android_host.cc` (implementing the same `Host` interface `windows_host.cc`
  and `linux_host.cc` implement) and its own touch UI, but reuses
  `vk_canvas_core` (via `add_subdirectory`, same as the demo does) and several
  of the demo's platform helper files that exist but aren't yet wired into
  its own `app.cc` (`input_handler.cc`, `orientation.cc`, `fullscreen.hh`,
  `jni_util.hh`) — see "Rendering" below.
- **`gui/src/player_view.cc`** — built for mouse+keyboard, a fixed sidebar,
  and desktop-style settings-panel overlays. A real touch UI (bottom
  navigation, gesture-driven, phone-sized layout) needs its own drawing/
  hit-testing code. It draws through the same `Canvas`/`widgets::` primitives
  and reuses `core/` unchanged; only the layout code is new. `theme.hh`,
  `ui_metrics.hh`, and `ui_icons.hh` (portable, no `Host` dependency already)
  are reused verbatim from `gui/src/`.
- **The reference `media_player`'s storage trick** — `requestLegacyExternalStorage`
  pinned to `targetSdk 28`, plus hand-parsing a SAF tree URI back into a raw
  path string. Works only because scoped storage isn't enforced below
  `targetSdk 29`; freezing the whole app's target API for the life of the
  project to keep one permission trick is not worth it here. See "Storage".

## Repo layout addition

```
android/
  app/                        — Gradle module (self-contained, mirrors the
                                 structure of framework/vk_canvas/platform/android/
                                 and framework/audio_engine/platform/android/):
                                 build.gradle, AndroidManifest.xml, gradlew,
                                 assets/ (fonts/shaders/eq_profiles.json — read-only,
                                 packaged into the APK, mirrors app_paths.hh's
                                 exe-relative half)
  src/
    android_host.cc/hh        — Host implementation (NativeActivity event loop,
                                 wires FrameInput, calls into vk_canvas_core)
    android_player_view.cc/hh — the new touch UI: layout, drawing, hit-testing.
                                 Reuses core/ and gui/src/theme.hh + ui_metrics.hh
                                 + ui_icons.hh unchanged.
    safe_area.hh/cc           — NEW: DisplayCutout JNI query (see "Notch" below).
                                 Lives next to fullscreen.hh's existing
                                 query_nav_bar_height(), same file family.
    storage_permission.hh/cc  — NEW: SAF folder-picker intent + MANAGE_EXTERNAL_STORAGE
                                 request flow (JNI, see "Storage" below).
    android_folder_watch.cpp  — core/src/os/ addition IF linux_folder_watch.cpp's
                                 inotify calls turn out not to just work unmodified
                                 on a real granted path (needs on-device verification
                                 once the toolchain exists — see "Build tooling gap").
    app_paths_android.cc      — Android's app_paths.hh backend: AAssetManager for
                                 read-only assets, Context-provided internal storage
                                 path for matrix_player.db / log / atlas cache.
  CMakeLists.txt               — reached via app/build.gradle's externalNativeBuild;
                                 add_subdirectory(core), add_subdirectory(framework/
                                 audio_engine), add_subdirectory(framework/vk_canvas/core);
                                 links ae_core/ae_usb/ae_aaudio/ae_mediacodec/ae_flac/
                                 ae_mp3/ae_dsd + vk_canvas_core directly (no C ABI).
```

## Storage & permissions

Chosen: **SAF folder picker once (`ACTION_OPEN_DOCUMENT_TREE`), for user
consent and discoverability of what's being granted) + `MANAGE_EXTERNAL_STORAGE`**
("All files access", a special permission the user grants once in system
Settings — available to sideloaded apps without Play Store review) instead of
the reference app's `targetSdk`-pinning trick. Consequence: `core/library.cpp`'s
`std::filesystem` walk and `core/src/decoder.cpp`'s POSIX file opens work on
real paths, completely unmodified — no `content://` URI resolution anywhere in
this port, no per-file JNI round-trip. `FolderWatcher`'s Linux backend
(`inotify`) very likely works unmodified too, since Android runs a real Linux
kernel and a granted path is a granted path — flagged in the layout above as
needing on-device confirmation rather than assumed, since it can't be tested
without the toolchain.

`storage_permission.hh/cc` is the one new file: launches the
`Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION` intent via JNI (same
call shape as `fullscreen.hh`'s existing JNI helpers — `env_for()` +
`check_exc()` from `jni_util.hh`), and separately the `ACTION_OPEN_DOCUMENT_TREE`
picker for the consent step. Neither requires a `.java` file — both are Intents
built and fired from C++ against the `NativeActivity`'s own `Activity` object.

## Audio engine integration

`android/CMakeLists.txt` treats `framework/audio_engine` exactly like
`gui/CMakeLists.txt` treats it on desktop: `add_subdirectory`, link the C++
targets directly. This requires one small, principled addition to
`framework/audio_engine/CMakeLists.txt` (its own `CLAUDE.md` says read it
before touching it — done): an `if(ANDROID)` block defining `ae_aaudio`
(`backends/aaudio/aaudio_sink.cpp` + `aaudio_source.cpp`) and `ae_mediacodec`
(`backends/mediacodec/mediacodec_decoder.cpp`) as `STATIC` targets, mirroring
the existing `ae_alsa`/`ae_jack` conditional-target pattern exactly (same
style, same place in the file). This does not touch or replace
`platform/android/`'s existing self-contained Gradle project — that stays as
an independent, valid path for a Java-consuming app; this port simply doesn't
use it.

> **What actually happened (2026-08-13).** That block was designed here and
> then **never written**. `android/CMakeLists.txt` linked `ae_aaudio` and
> `ae_mediacodec` from the start and nothing defined either —
> `git log -S ae_aaudio` over audio_engine's `CMakeLists.txt` returns no commit
> at all. Nobody noticed because nobody built this target: the first attempt to
> do so failed three times before even reaching it (missing SDK platform,
> `ui_min_text_size.gen.h` looked for only under `build/windows/`, then this).
>
> It exists now, with one deliberate narrowing from the design above:
> **`ae_aaudio` is the SINK ONLY, and `ae_mediacodec` is not built.**
> `aaudio_source.cpp` is capture, which this slice has no use for. Dropping
> MediaCodec is the more interesting call: this engine decodes FLAC and MP3
> with its own vendored libFLAC/libmpg123 on every platform ON PURPOSE, because
> some phones ship no FLAC decoder — so `AMediaCodec` would be a second,
> device-dependent decode path for formats already covered, and would make
> "bit-perfect everywhere" a claim that varies by handset. Adding either back
> is one line.

## Rendering integration

`android/CMakeLists.txt` also does `add_subdirectory(framework/vk_canvas/core)`
and links `vk_canvas_core` directly, matching
`framework/vk_canvas/platform/android/CMakeLists.txt`'s own pattern for its
demo. `android_host.cc` is this port's `NativeActivity` entry point (glue
event loop, `FrameInput` wiring, `Canvas`/`Renderer` setup) — structurally the
Android sibling of `gui/src/os/windows_host.cc` / `linux_host.cc`, implementing
the same `Host` interface (`gui/src/host.hh`) so `android_player_view.cc` can
be written against the same seam the desktop UI already proves out.

It wires in four helper files from `vk_canvas/platform/android/` that exist
today but the demo itself doesn't yet call (`app.cc`'s own comment: "WIP
Android glue, not yet wired"):

- `input_handler.cc` — touch input → `InputSink`/`FrameInput`.
- `orientation.cc` — physical-orientation sensor (device held sideways while
  the window itself stays portrait-locked).
- `fullscreen.hh` — immersive mode + `query_nav_bar_height()`.
- `jni_util.hh` — the `env_for()`/`check_exc()` substrate everything above
  (and the new `safe_area.hh`) is built on.

### Notch / display-cutout precision

Answered directly during design: **yes, fully precise, and it does not need a
`.java` file — but it does need one new JNI call**, because
`android.view.DisplayCutout` has no NDK/C equivalent at all (unlike, say,
sensor data, which the NDK exposes natively). The call chain, guarded exactly
like every other call in `jni_util.hh` (`check_exc` after each step, so a
missing method — pre-API-28 devices — degrades to "no cutout" rather than
crashing):

```
activity.getWindow().getDecorView().getRootWindowInsets().getDisplayCutout()
    .getSafeInsetTop() / Left() / Right() / Bottom()   // exact pixels
```

`safe_area.hh/cc` implements this, same file family and JNI conventions as
`fullscreen.hh`. Deliberately NOT using `android_app::contentRect` (the
glue-provided rect, no JNI needed) as the primary source: `contentRect`
reflects general system-bar insets but stops being trustworthy for the cutout
specifically once the window goes edge-to-edge/immersive — which is exactly
the rendering mode this app wants for a full-screen Vulkan canvas. Re-query on
window resize / content-rect-changed events, the same way `fullscreen.hh`'s
immersive flags are re-applied on `APP_CMD_GAINED_FOCUS`.

## UI

`android_player_view.cc/hh` is new, touch-first: bottom navigation instead of
a sidebar, gesture-driven panel transitions instead of desktop overlay clicks,
layout computed from `vk_canvas/core/layout.hh`'s resolution-robust helpers
(`UiScale`, `dockTop/Bottom/Left/Right`) the same way the demos already do.
Draws through the same `Canvas`/`widgets::` primitives `player_view.cc` uses on
desktop. `core/` (library scan, DB, decoder, variants, EQ manager/profiles,
stats) is consumed unchanged — no new files there beyond the possible
`android_folder_watch.cpp` noted above.

## `app_paths` for Android

Desktop's split — read-only assets exe-relative, writable state via
`stateDir()` — maps to: read-only assets (`fonts/`, `assets/shaders/`,
`eq_profiles.json`) packaged into the APK and read through vk_canvas's
existing `AssetReader` seam (already Android-implemented, APK-asset-backed);
writable state (`matrix_player.db`, the log, the atlas cache) goes to the
path the Android `Context` provides for the app's own internal storage —
resolved once via a JNI call at startup (`getFilesDir()`), not hardcoded,
following the same "single source of truth" rule `app_paths.hh` already
documents for the other two platforms.

## Build tooling — resolved

Originally this design assumed no Android toolchain was available and that
implementation would be written unbuilt/unverified. That gap is now closed:
the SDK/NDK/JDK were installed and verified mid-design (custom root
`C:\Users\nava\Android_SDK_etc`, matching every version this design already
targeted for consistency with `framework/audio_engine`'s and
`framework/vk_canvas`'s own Android projects):

```
JDK 17.0.20 (Temurin)
build-tools;35.0.0
platforms;android-35
ndk;29.0.14206865
cmake;3.22.1
platform-tools 37.0.1 (adb confirmed responding)
```

`JAVA_HOME`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME` and `Path`
(`cmdline-tools\latest\bin`, `platform-tools`, JDK `bin`) are all set at User
scope and confirmed live in a fresh shell. The Vulkan SDK (`slangc`, needed
for `vk_canvas`'s shader compile step) is already on `PATH` from prior desktop
work.

Implementation now proceeds as buildable code, verified by actually invoking
Gradle/CMake as each piece lands — not written speculatively and left for a
future session to compile. No device/emulator is confirmed available yet, so
on-device-only checks (the `inotify`-on-Android question, runtime JNI
behavior, the notch/cutout query on a real display) still land as flagged
follow-ups until a device or emulator is confirmed.

## Testing

No device, no emulator, no toolchain — nothing here can run
`dsp_null_test`-style verification in this session. The audio/DSP paths
themselves are untouched (same `ae_core`/backends code as desktop, already
covered by `dsp_null_test`); what's new and unverified is entirely the
Android-side glue (JNI calls, CMake wiring, storage permissions, the touch
UI). The follow-up checklist (see above) is the concrete list of what to
smoke-test once a build is possible.
