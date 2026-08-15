# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in `app_shell`.

## What this is

**The application shell.** `vk_canvas` makes *drawing* portable — a Renderer, a
Canvas, MSDF/raster text, and three real backends. This library makes the
*application* portable: the window, the message pump, the input, where the files
live, how big the text is, and which way round the screen is.

Together they are the answer to one question: **can a new app be written once
and run on Windows, Linux and Android?** vk_canvas alone gets you a picture.
This gets you a program.

An app that builds on it writes:

```cpp
class MyApp : public AppView {
public:
    bool create() {
        host_ = make_host();               // or an injected one
        return host_ && host_->init(this);
    }
    void run() {                           // the loop is the APP's, see below
        while (running_) {
            host_->pump(/*haveWork=*/dirty_);
            if (host_->quitRequested()) break;
            if (dirty_) { draw(); dirty_ = false; }
        }
    }
    void onHostResized() override { dirty_ = true; }
    void shutdown()      override { running_ = false; }
    std::unique_ptr<Host> host_;
    bool running_ = true, dirty_ = true;
};

int app_shell_main() { MyApp app; return app.create() ? (app.run(), 0) : 1; }
```

…and gets `main()`, `WinMain()`, the Wayland and Win32 event loops, the crash
handler, the log file, and the Android `Host` for free. Android needs one extra
file — an `android_main()` that constructs the app and an `AndroidHost` — and
that file is six lines long.

**`create()` and `run()` are the APP's, not this library's.** app_shell owns the
platform BOOTSTRAP and hands control over; it does not own the frame loop,
because what counts as "work to do" is the app's question — an editor idles at
zero frames, a game never idles. `docs/app_shell.tex` is the full reference
manual; read it before extending the seam.

Extracted from Matrix Player, which is still its first and, for now, only
consumer. **API-shaping decisions should favour the NEXT consumer, not that
one** — that is the whole reason this is a separate library rather than a folder
in `gui/`.

It lives IN that repository as a plain directory, not as a submodule. The split
that matters is the one in the source (nothing here may know what a track is),
not one in version control: this is first-party, it changes in step with the app
that consumes it, and a fourth clone step would buy nothing. A second app either
copies the folder or points its own `add_subdirectory()` at this one.

---

## Layout

```
app_shell/
  app_view.hh        — what a Host calls. The app implements it.
  host.hh            — what the app calls. A platform implements it.
  app_main.hh        — app_shell_main(), plus the three names an app supplies
  app_paths.hh/.cc   — exeDir() / stateDir(), and the rule about the split
  ui_metrics.hh/.cc  — one scale factor, five type roles, space()/stroke()
  ui_orientation.*   — Horizontal or Vertical, derived from the window's shape
  layout_rect.hh     — a portable rectangle (replaces Win32's RECT)
  color.hh           — a portable colour (replaces COLORREF)
  os/
    win32_host.cc          — real Win32 window + message pump + minidump
    wayland_host.cc        — real Wayland, via vk_canvas's WaylandDisplay/Window
    android_host.hh/.cc    — real ALooper/ANativeWindow host
    app_paths_android.*    — Android's answer to exeDir()/stateDir()
    launch_intent.*        — read a string extra off the launch intent
    safe_area.*            — the display CUTOUT (never a system bar)
    storage_permission.*   — all-files access, asked at most once
  tests/             — plain assert(), Debug-only, no framework
  cmake/AppShellMinTextSize.cmake — the build-time text-size floor generator
```

`app_shell` (portable) links nothing. `app_shell_win32` / `app_shell_wayland` /
`app_shell_android` are one host each, and a consumer links exactly one.

---

## The seam, and the three things that were wrong with it

`Host` and `AppView` are two halves of one interface. The rule is simply which
way the call goes: an app calls `Host`, a platform calls `AppView`.

This was lifted out of an application, and the lifting was mostly about
**removing that application's vocabulary from the platform half**. Three things
had to go, and the shapes they left behind are load-bearing:

1. **`Host::init()` took the concrete app class**, and each backend then called
   its methods by name. Measured across the three, they called nineteen methods
   any app would have and two only a music player could. Hence `AppView`, with
   almost everything defaulted to empty: an app that takes no keyboard should
   not have to write an empty override to say so.

2. **Cross-thread events and timer ids were ENUMS DECLARED IN `host.hh`** —
   `AppEvent{TrackChange, ScanDone, …}`, `TimerId{SeekUpdate}` — and every
   backend switched on them. That is one app's words in the one file that must
   know nothing about it, and it meant the SAME dispatch was written out four
   times over (three hosts plus a headless capture tool). Both are plain `int`
   now, carried and never read: `postAppEvent(id, p1, p2)` comes back out of
   `AppView::onAppEvent(id, p1, p2)` unchanged.

3. **`snapToEdge()` took the app's hotkey id**, so the Win32 host held a table
   mapping one player's Alt+F/J/C/U/G/H to window geometry. It takes a
   `SnapEdge` now — an edge is what a window system deals in — and hotkeys are
   *registered* by the app (`registerHotkey(id, keyCode)`) instead of being
   hardcoded per backend.

The same principle covers `Host::launchArgument()`: Android is launched with an
intent extra, and the host used to read a key named `"scan_root"` and hand it to
the app's own `commitAddFolder()`. Now `AndroidHost` is TOLD which key to read
(a constructor argument) and states the string; deciding what a launch argument
MEANS happens in `AppView::onHostReady()`, which is the app's.

**The test for anything new here: could a drawing program use it?** If the
answer needs the word "track", "album" or "playlist", it belongs in the app.

### Two rules learned by crashing on a phone

1. **Nothing decided before `pump()` may be trusted after it.** `pump()` is
   where a platform delivers "your surface is gone". A `bool canDraw` computed
   before the pump and used after it segfaults on the first launch that hides
   the system bars, because hiding them recreates the window.
2. **`safeInsets()` is the CUTOUT and never a system bar.** A bar is software
   and gets hidden; insetting for one that is not on screen leaves a permanent
   empty strip. A cutout is glass.

### What genuinely differs between platforms, stated rather than faked

- `snapToEdge` and `adaptToCurrentMonitor` are real no-ops on Wayland: a client
  cannot position itself or ask which monitor it is on. They are no-ops on
  Android for the more obvious reason.
- `registerHotkey` is SYSTEM-WIDE on Windows (`RegisterHotKey`) and
  focused-window-only on Wayland. A documented narrowing, not a silent drop.
- `onSurfaceLost()` / `onSurfaceRecreated()` are Android-only by construction —
  dead code on both desktops, and the split they enforce (CPU state survives,
  GPU state does not) is invisible until the second visit to the app.

---

## `ui_metrics` and the header it includes

`ui_metrics.hh` includes `ui_min_text_size.gen.h`, generated at build time from
**the consuming app's own fonts**: the emitted floor is the worst case across
them, so it is a per-application number and not a library constant.

The recipe is `cmake/AppShellMinTextSize.cmake`; the consumer calls
`app_shell_generate_min_text_size(<its faces>)` and then
`add_dependencies(app_shell generate_ui_min_text_size)`. It cannot run under a
cross-compiler, so an Android build copies the header out of a desktop build
tree — see any consumer's `android/CMakeLists.txt`.

`ui_metrics` takes the window's **short side**, not its height. Identical for
any window wider than tall, but a 1080x1920 monitor would otherwise scale 1.78x
because the screen is TALL, not big.

---

## The three names an app supplies

Cache variables, set before `add_subdirectory()`, all with working defaults so
this library configures and builds on its own:

| | |
|---|---|
| `APP_SHELL_APP_NAME` | the log file's base name, and the Android logcat tag |
| `APP_SHELL_WIN_CLASS` | the Win32 window class (only has to be unique per process) |
| `APP_SHELL_STATE_HOME` | directory under `$HOME` for what the app WRITES; empty means beside the executable |

Android also needs `APP_SHELL_NATIVE_APP_GLUE_DIR` — native_app_glue ships
inside the NDK, so only the consumer knows where it is.

`app_paths`'s split is the rule worth keeping: READ-ONLY shipped data is
exe-relative and stays that way; everything WRITTEN goes through `stateDir()`.
They are the same directory by default, so a build tree stays one self-contained
folder you can move anywhere; naming `APP_SHELL_STATE_HOME` splits them, which
is what a system package needs since `/opt` is root-owned.

---

## Tests

Debug-only, plain `assert()`, no framework — the convention this whole family of
repositories uses. They **compile** the sources rather than linking `app_shell`,
so a test can never quietly start depending on Vulkan.

```bash
./build/<tree>/app_shell_build/ui_metrics_test
./build/<tree>/app_shell_build/ui_orientation_test
```

Keep them pure.

---

## Committing

Use `git_wrapper` at the CONSUMING repository's root, never plain `git commit`/
`git push`. There is no separate repository here to push.
