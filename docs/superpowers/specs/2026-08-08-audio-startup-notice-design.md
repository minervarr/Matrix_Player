# Startup audio backend default + unified error notice

Date: 2026-08-08

## Problem

On a fresh install (no `audio_backend` setting saved), the app defaults to
`AudioBackend::Usb` and probes for the USB DAC at startup, before the window
is even shown. When no DAC is present (or libusbK/Zadig isn't set up), this
throws a native `MessageBoxA` (Windows) — a modal dialog with OS chrome that
clashes with the app's own dark/serif visual language, and is the first thing
a new user sees, reading as "the app is already broken."

Two more sites duplicate this: `player_view.cc:5417` (configure failed) and
`:6080` (device fault mid-playback) both set the existing non-modal
`audioNotice_` strip (Canvas-drawn, documented in `UI_DESIGN_SYSTEM.md` §8.7)
**and** fire the same native `MessageBoxA` right after — redundant, and the
same visual clash.

A fourth site, `applyAudioSettingsPanel()`, does the opposite: when a user
explicitly picks USB in Audio Settings and the DAC doesn't open, nothing is
shown at all — silent failure.

## Design

### 1. Backend default (`player_view.cc:426-439`)

When no `audio_backend` setting is saved:
- Windows → `AudioBackend::Wasapi`, with `wasapiMode_` defaulting to
  `Exclusive` (currently defaults to `Shared`; a saved `"shared"` still wins).
- Linux → `AudioBackend::Alsa` when `MATRIX_HAVE_ALSA` is compiled in,
  otherwise `AudioBackend::Usb` (no other safe fallback exists).

An explicitly saved value (`"usb"`, `"wasapi"`, `"alsa"`, `"jack"`) always
overrides the default, same as today.

Effect: the USB-open block (`player_view.cc:471-499`) only ever runs when
`audioBackend_ == Usb`, which after this change can only happen because the
user previously chose USB on purpose. The libusbK/Zadig check is now tied to
an explicit opt-in into direct USB output, never to a fresh install.

### 2. One notice mechanism, never the native dialog

- Remove `host_->showErrorMessage(...)` from all three audio call sites
  (`:496` startup USB-not-found, `:5417` configure failed, `:6080` device
  fault). All three keep only the existing `audioNotice_` strip.
- The startup USB-not-found message shortens to one line (no embedded
  Zadig/libusbK step list — that content belongs in the app's manual/docs,
  not in a runtime string): `"USB DAC not found (VID=XXXX PID=XXXX) — check
  Audio Settings."`
- `applyAudioSettingsPanel()` gains the same short `audioNotice_` message when
  the explicitly-chosen USB device fails to open (today: silent).
- `Host::showErrorMessage` stops being used for anything audio-related. Its
  one remaining call site is the Vulkan init failure (`:222`), a genuinely
  different case — it fires before any Canvas surface exists, so the strip
  isn't drawable yet.

### Out of scope

- No new UI widget or overlay panel — this reuses the existing `audioNotice_`
  strip exactly as documented in `UI_DESIGN_SYSTEM.md` §8.7.
- No change to `Host::showErrorMessage`'s implementation itself (still a
  native `MessageBoxA` on Windows, stderr-only on Linux) — it's simply no
  longer called from audio code paths.
- No documentation of the Zadig/libusbK setup steps is written as part of
  this change (the user will handle the manual separately).

## Testing

Manual only, per project convention (`CLAUDE.md`: "validate audio/DSP changes
by building and listening; validate GUI changes by building and running
`matrix_player`"):
1. Fresh DB (no `audio_backend` setting), no DAC plugged in → app opens
   straight to WASAPI Exclusive, no dialog, no strip.
2. `audio_backend = "usb"` saved, no DAC plugged in → app opens, short strip
   appears, no native dialog.
3. Audio Settings → pick USB with no DAC plugged in → Apply → short strip
   appears, no native dialog, panel still closes.
4. Trigger a configure failure and a mid-playback device fault (e.g. pull the
   DAC while playing) → strip shows the driver's own error text, no native
   dialog in either case.
