# Android runs the real app: a third `Host`, not a third UI

**Date:** 2026-08-14
**Supersedes (in part):** `2026-08-08-android-native-port-design.md`
**Status:** implemented and **running on hardware** (moto g06, Android 15) —
see "Hardware bring-up" below for the six defects that only a real device
could have shown, five of which were older than this work

## Context

The 2026-08-08 port shipped `AndroidPlayerView`: a flat, touch-scrollable
track list with its own layout and its own drawing, justified by the rule that
opened this project's Android section —

> `AndroidPlayerView` is NOT a port of `PlayerWindow`, and `AndroidHost` is NOT
> an implementation of `Host`. […] a touch, phone-sized UI is genuinely
> different from a sidebar-and-grid desktop one.

Two days of work followed that rule faithfully: `ui_orientation.cc` and
`rail_layout.cc` crossed because they were pure, then `bar_a.cc` crossed behind
a plain-data `BarAModel`, verified by 40 byte-identical `matrix_ui_capture`
screenshots. It worked, and the phone still did not look like the app — it
showed the desktop's navigation rail floating over a list of file names. Bar B,
the grid, the album view, the search and the panels were all still to come, at
roughly one extraction each.

The user's response to seeing it was the whole reason this document exists:
*"no es nada igual a lo que existe en el escritorio; haz que sea lo mismísimo."*

## What the exploration found

The rule rested on a premise nobody had checked. Checking it took three greps:

1. **`gui/src/player_view.cc` includes no OS header.** Not one. Its complete
   platform coupling is `Host` (a pure virtual interface), `AudioOutput`
   (another one), and `art_view.hh`.
2. **`PlayerWindow::run()` is a loop over `host_->pump()`** and nothing else.
3. **Android already linked `matrix_core`** — `Db`, sqlite3, `scanLibrary`,
   `facets`, `variants` — and had its own `app_paths_android.cc`. All present,
   all unused.
4. `tools/ui_capture` already contained a `HeadlessHost : public Host` whose
   every method is an honest no-op, which is a working existence proof that the
   seam does not require a window, a monitor, or a cursor.

`PlayerWindow` was therefore **already portable in fact**. The 4000 lines of
drawing were never the obstacle; the missing 350-line `Host` was.

The second half of the rule — *"`Host::init()` is hard-typed to the concrete
`PlayerWindow`, with no seam another owner could implement"* — was true and
turned out to be irrelevant: once Android runs `PlayerWindow` itself, there is
no other owner that wants to implement it.

## Decision

Write `AndroidHost : public Host`. Delete `AndroidPlayerView`. The phone runs
`gui/src/player_view.cc`.

This is cheaper than the extraction path by roughly an order of magnitude AND
strictly better on the goal, because "identical" stops being a target to
converge on and becomes a property of compiling one file for two platforms.

**It does not make the extraction work pointless, and does not cancel it.**
`bar_a.cc` stays split. The extraction's real customer was never Android — it
is the framework, and a second application (`streamer`) that wants to draw this
app's furniture without inheriting its state. That work continues, on its own
schedule, as framework work rather than as port work.

## What had to be built

### 1. Surface loss (the only structural change)

Desktop windows are born once and die once, so `create()` builds the `Renderer`
once and `shutdown()` tears it down. Android destroys the surface **every time
the listener leaves the app** and creates a new one on return.

`PlayerWindow::onSurfaceLost()` / `onSurfaceRecreated()` are the GPU-only
halves of `shutdown()` and `create()`. Neither desktop host calls either — they
are dead code on Windows and Linux by construction, not by accident.

The invariant they enforce: **CPU-side state survives, GPU-side state does
not.** The loaded faces and the `RasterFont`'s placed cells persist; the
Renderer, the swapchain, every art texture and the glyph atlas image do not.
This exact distinction had already caused a latent bug two days earlier —
`AndroidHost::initFonts()` returned early on a single `fontsReady_` flag, which
would have left the second window with no atlas and made every string on screen
vanish after the first backgrounding, with no error anywhere.

Three details are load-bearing and were found by writing it, not by reasoning:

- The art textures' **cache keys** (`trackPanelArtTexAlbum_`,
  `transportArtTexPath_`) must be cleared alongside the handles. A handle reset
  to invalid while its key still names the old album leaves each loader
  convinced it is already showing the right thing, so the art never returns.
- `glyphBaker_` owns Vulkan pipelines; GPU baking is dropped (`useGpuBake(false)`)
  rather than re-armed, since it is opt-in and measured slower anyway.
- `run()` must not draw when `renderer_` is null, and must let `pump()` block
  while it is — otherwise a backgrounded app spins on a dirty flag nobody can
  service.

### 2. `Host::dataReader()`

Fonts are files beside the executable on desktop and entries inside the APK on
Android. One new method on `Host` removes the difference from `create()`:
`exeDir()` returns `""` on Android, so `exeDir() + "fonts/…"` **is** the asset
name AAssetManager wants. Same string, two readers.

Deliberately separate from `assetReader()` (rooted at `<exe>/assets/` for
shaders), and deliberately not used for the music library — album art and audio
files are ordinary absolute paths on every platform.

### 3. `ArtWindow` declines

`player_view.hh` holds `ArtWindow artWin_` **by value**, so `art_view.hh` has to
compile under the NDK, and it includes Win32 or Wayland headers. A phone has
neither a second monitor nor a second top-level window, so the Android branch is
a class with the same surface that returns `false` from `create()`.

It **declines** rather than being `#ifdef`'d out because `ensureArtWindow()`
already treats a refused `create()` as permanent (`artWinFailed_`). That keeps
all nine `artWin_` call sites in `player_view.cc` free of any knowledge that
Android exists — which is the property this whole design is protecting.

### 4. Touch, translated honestly

24 px slop. Under it the gesture is a tap and the press is delivered **at
release**, so a finger that slides off cancels — the way a button behaves
everywhere. Past it the gesture becomes wheel deltas for good, and the press
never happens: a drag must scroll and must not press what was under it when it
started. The wheel is fed the finger's own pixel displacement, so content
tracks the finger rather than stepping.

Hover follows the finger. That is the only hover a touch screen has, and it is
not a lie: the desktop's hover means "the pointer is here", and here it is.

### 5. `AAudioOutput`

Modelled on `AlsaOutput`. Two differences, both AAudio's doing rather than
simplifications: there is **no device list** (the system owns the route and
moves it when headphones are plugged in — a list here would be one the app
cannot honour), and the stream is **always 16-bit**. So `strictBitperfect` on a
deeper source is refused outright, and `deviceMaxBits` is stated as 16 — which
is what makes the signal-chain readout report "truncated to 16-bit device"
instead of claiming bit-perfect on a phone.

## Verification

Desktop, after every step:

- 8 assert tests + `dsp_null_test` (`=== PASS (20498565 checks) ===`).
- **The 20 `matrix_ui_capture` states in 1920×1080 and 1080×1920, byte-identical
  to the pre-change baseline.** This is the primary check: nothing in this work
  should move a desktop pixel, so any difference is a bug and not an
  improvement. Run four times across the change; identical each time.

Android:

- `BUILD SUCCESSFUL`; both APKs (~103 MB).
- `unzip -l …apk | grep -c "assets/fonts/"` → **69**; shaders → **14**.
- `nm -DC …/libmatrix_player_android.so | grep -c 'PlayerWindow::'` → **143**,
  including `create`, `run`, `drawFrame`, `onSurfaceLost`, `onSurfaceRecreated`.
  This is what distinguishes "the app crossed" from "it compiled".

## Hardware bring-up (same day, moto g06 / Android 15, 720x1640)

The app runs: the album grid with real artwork, the album view, both bars, the
New Computer Modern face with its CJK fallback, playback through AAudio, and
both orientations. **Leaving the app and returning keeps the process and
redraws everything** — the path §1 exists for, and the one that had the least
evidence behind it.

Six defects surfaced, and the shape of the list is the finding: **only one was
introduced by this work.** The rest had been in the tree for months and could
not be seen on a desktop.

1. **SIGSEGV before the first frame** (mine). Android delivers `APP_CMD_RESUME`
   while `AndroidHost::init()` is still pumping for its first window, and
   `onResume()` asked `PlayerWindow` about its music roots — but `create()`
   opens the database *after* `init()` returns. Null `sqlite3*`. The guard
   existed (`appReady_`) and was only being applied to the Renderer; it now
   lives inside the function that has the requirement, not in its callers.
2. **Immersive mode was a silent no-op.** `fullscreen.hh` used
   `setSystemUiVisibility`, deprecated in API 30 and **ignored from Android 15**
   for apps targeting 35+. It ran, raised nothing, and did nothing. Fixed in the
   vk_canvas submodule with `WindowInsetsController`, legacy kept as fallback.
3. **Hiding the bars created the cutout problem.** While the status bar is
   shown, Android keeps the window clear of the camera hole *because the bar
   covers it*. Hide it and the window goes edge to edge — the full 720x1640
   panel — and bar A lands under a 70 px notch. Hence `Host::safeInsets()`,
   which reports the cutout and deliberately **not** the navigation bar: a bar
   is software and gets hidden, a cutout is glass and can only be avoided.
4. **`preTransform = caps.currentTransform`** (renderer.cc). That is a
   *promise* that the app has already rotated its own rendering, not a request
   for rotation. It never was true. On a desktop `currentTransform` is always
   IDENTITY so the line could not fail; on the phone it becomes `ROTATE_90` and
   the whole UI came out upside down and squashed. Now asks for IDENTITY when
   advertised and lets the compositor rotate.
5. **A stale `canDraw` across `pump()`** (mine, introduced hours earlier).
   `run()` decided "there is a renderer" *before* `host_->pump()` and used the
   answer *after* it — and pump() is exactly where `APP_CMD_TERM_WINDOW`
   destroys the Renderer. Surfaced only because fixing (2) made the window get
   recreated during startup. **Rule: nothing decided before pump() may be
   trusted after it.**
6. **The Audio Settings radio showed a backend that was not playing.** It
   re-derived its selection from the saved `audio_backend` string; on a fresh
   install nothing is saved, no case matched, and the loop fell through to
   index 0 — always USB Direct. Meanwhile `create()` defaults to AAudio on
   Android, ALSA on Linux, WASAPI on Windows. **A first-run Linux build has the
   same lie**; the phone only made it audible, because there the speakers
   contradict the panel. Now read straight off `audioBackend_`.

Two more of the same family, found by reading rather than by crashing: the
audio-notice strip was laid out as a tall narrow column in the horizontal
orientation while its drawing code assumed a wide short one (giant warning
triangle, text pushed off the strip); and **no build on this machine had an MP3
decoder at all**, because `python3 initialize_files.py` had never been run and
its two `message(WARNING)` lines scrolled past in the configure output.

The lesson worth carrying: a desktop-only project accumulates code whose
failure modes are unreachable on a desktop. Five of these six were latent, and
none of them needed the port to be wrong to exist.

## Known gaps, stated rather than hidden

- **No keyboard.** `onCharPortable()` is never fed, so the guided-search box can
  be seen and not typed into. The IME is separate work.
- **It looks identical and will touch worse.** The rail's letters are sized for a
  mouse. A touch-ergonomics pass is design work, not seam work.
- **libjpeg-turbo is not built for the NDK** (its CMake refuses
  `add_subdirectory`), so JPEG art decodes through `stb_image`. Its known cost is
  aspect distortion on non-square decode boxes; grid art is square.
- **Windows has been edited blind.** The surface seam, `dataReader()` and the
  `AudioBackend` enum all touch it; it needs `scripts\windows\build.ps1 -Debug`
  from MSYS2 UCRT64.

## What this unlocks, and what it does not

It unlocks the framework step, and it is a precondition for it rather than a
detour: `host.hh` can now be split into a portable core (window, pump, input,
timer, cross-thread event, assets, screen size) and a Matrix-specific remainder,
with **three** real implementations to check the cut against instead of two.

It does not authorise doing that yet. `docs/android-platform-reuse.md` asks that
nothing be consolidated into the framework until one of the two apps has been
seen running on a device, and cites `platform/android/input_handler.hh` — pushed
up validated by the compiler alone, and used by nobody since. That warning
applies to this work exactly as written.
