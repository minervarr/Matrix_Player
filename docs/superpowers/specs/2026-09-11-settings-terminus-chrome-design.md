# Settings-panel chrome in Terminus — design

**Goal:** Draw Settings' fixed UI vocabulary — panel headers, button labels,
section captions, toggle words, short status lines — in Terminus (the bitmap
face already used for bar A's Settings and Find cells), in ALL CAPS, so the
Settings screens read as a terminal readout and reinforce "this is Matrix
Player." Folder paths, device names, EQ profile names, and any other runtime
string keep the existing serif face, because Terminus is a finite baked glyph
set and can never represent arbitrary Unicode.

**Why now:** Terminus already exists in this codebase (`gui/src/terminus_glyph.hh/.cc`,
`tools/terminus/bake_glyphs.py`) as a two-glyph proof (`S`, `F`) drawn as
run-length rectangles at an integer scale. This spec extends it from "one
centred glyph per cell" to "a run of glyphs" and applies it to the five
Settings panels.

## The one rule that decides what converts

Terminus cannot represent everything, and hand-classifying every string in
Settings as "chrome" or "data" would be tedious and would rot as copy changes.
Instead there is **one mechanical rule**, extending the convention
`drawTerminusGlyph` already uses (`bool`, false on a miss):

> `drawTerminusText()` tries to draw the WHOLE string in Terminus. If any
> character in it has no baked glyph, the call draws nothing and returns
> `false`; the caller then draws that exact string, unchanged, through the
> existing serif path.

Consequences of this rule, all deliberate:

- A folder path or device name containing anything outside the baked set —
  which for non-Latin scripts is everything — automatically stays serif. No
  call site needs to know it is "data."
- A short imperative label ("Remove Selected", "Done", "Apply") is plain
  ASCII and converts.
- A longer explanatory sentence that happens to use an em dash or curly
  quote (this codebase's prose style leans on em dashes constantly) also
  falls back automatically, which is the right outcome: a paragraph of
  explanation reads worse in a blocky terminal face than a short label does.
  This is not a length rule — it falls out of the ASCII-only rule for free.
- Baking the FULL printable ASCII range (0x20 space through 0x7E `~`) up
  front, rather than one letter at a time as `S`/`F` were, means future copy
  changes in Settings never need a re-bake. Lowercase letters map to their
  capital's glyph at lookup time — callers keep writing normal mixed-case
  string literals; `drawTerminusText` uppercases internally. Space advances
  the pen without drawing (needs the bake script's "an all-zero glyph is an
  error" check relaxed for exactly this one character).

## Scope: which draw functions convert

Two shared widgets carry most of Settings' chrome:

- `panels::drawHeader` — the page title.
- `panels::drawButton` — Done / Cancel / Remove / Assign / Select / Close /
  Apply / Forget / etc.

Both are also reused by scenes built in the prior pass (`EqSwitcher`,
`drawSignalChain`) that were **not** part of this request. Each gets a new
`bool terminusChrome = false` parameter so those two scenes are untouched by
default; only the Settings call sites pass `true`.

**In scope** (every direct `canvas.text`/`canvas.textStyled` call inside these
functions is converted, via the fallback rule above):

| Function | Panel | Representative chrome strings found |
|---|---|---|
| `drawManageFolders` | Manage Folders | `Music Folders`, `No music folders added yet.`, `Remove Selected`, `Done` |
| `drawAudioSettings` | Audio Output Settings | `Audio Output Settings`, `Output backend:`, `USB DAC:`, `Device:`, `Mode:`, `Starting port:`, `Apply` |
| `drawBluetoothCodecSection` | (called from Audio Settings) | `Bluetooth codec`, `No Bluetooth headphones connected.`, `Now: `, `Pair as companion device`, `Forget` |
| `drawEqSettings` | EQ / AutoEQ Profiles | `EQ / AutoEQ Profiles`, `Select`, `Remove`, `Assign to Device`, `Clear` |
| `drawFolderPicker` | Select Music Folder | `Select Music Folder`, `No subfolders here.`, `Cancel`, `Select This Folder` |
| `drawInterfaceSettings` | Interface | `Interface`, `Scrolling`, `Close` |

This list is representative, not exhaustive — the implementation plan
enumerates every call site by file:line. Several longer explanatory
paragraphs already found in these functions (e.g. "SBC is a lossy encode —
this route can never be bit-perfect.", "Bitperfect mode active — EQ applies
once Reference EQ mode is enabled.") contain em dashes and will fall back to
serif automatically under the rule above; no special-casing needed.

**Explicitly out of scope, and why:**

- Anything drawn by `widgets::drawScrollList` (a `vk_canvas` submodule
  widget — off-limits per the no-submodule-changes constraint, and it is
  exactly where the real data rows live: folder paths, USB/ALSA device
  names, EQ profile names).
- `EqSwitcher` and `drawSignalChain` — not Settings, not asked for. The
  opt-in flag on the two shared widgets is what keeps them untouched.
- Anything already excluded by the ASCII rule (prose paragraphs with an em
  dash, a device string with an accented character, etc.) — this is a
  consequence of the rule, not a separate list to maintain.

## Technical design

**`gui/src/terminus_glyph.hh` gains two functions**, mirroring the existing
`Canvas::text`/`Canvas::textWidth` call shape so conversion at each call site
is close to mechanical:

```cpp
// Draws `text` left-aligned from (x, y) — same (x, y) = top-left convention
// canvas.text() already uses. Every character is looked up after an ASCII
// uppercase fold; the first character with no baked glyph (space excepted)
// aborts the WHOLE call and returns false before anything is drawn — no
// call ever emits half a string in Terminus and half missing.
bool drawTerminusText(Canvas& c, const std::string& text, float x, float y,
                      float targetPx, const Color& col);

// The advance width `text` would occupy at drawTerminusText's chosen strike
// and scale — for centering/right-alignment, mirroring canvas.textWidth().
// Returns -1.0f, never a real width, when the string cannot be represented
// (distinct from a legitimately empty string's width of 0). A caller that
// needs to center BEFORE drawing calls this first: a negative result means
// skip straight to the serif path without calling drawTerminusText at all.
float terminusTextWidth(const std::string& text, float targetPx);
```

Layout is a simple monospace advance per character (Terminus's whole reason
for existing is fixed-pitch cells) — no kerning table, no per-pair spacing.
`pickStrike()` (already implemented) continues to choose the 16px strike at
×1 below 24px target, else the 32px strike at the nearest integer scale; a
Settings header (`metrics_.text.header`, ≈30px at reference scale) lands on
the 32px strike ×1, and most body-sized labels (`metrics_.text.body`, ≈22px)
land on the 16px strike ×1. This quantization is the same one bar A's
Settings glyph already accepts; it is not solved further here — the
verification step below is where it gets *looked at*, and a third strike is
a follow-up if 16px body text reads too small in the captures.

**Bake script** (`tools/terminus/bake_glyphs.py`): `GLYPHS` becomes the
printable ASCII range `chr(0x20)` through `chr(0x7E)` inclusive (uppercase
letters only — lowercase is never baked, since `drawTerminusText` folds case
before lookup, and baking both would double the table for glyphs nothing
ever looks up). The "an all-zero-row glyph is an error" check is relaxed for
`chr(0x20)` (space) specifically, since a space's only job is to occupy
`cellW` pixels of advance.

**`panels::drawHeader` / `panels::drawButton`** each gain
`bool terminusChrome = false`. When true, the title/label draws through
`drawTerminusText` with the same "fall back to the current draw on a miss"
rule, so a button whose label happens to contain a character outside the
baked set (none currently do, per the inventory above) still renders
correctly instead of silently vanishing.

**The five Settings draw functions** (plus `drawBluetoothCodecSection`) pass
`terminusChrome = true` to both shared widgets and convert their own direct
`canvas.text`/`canvas.textStyled` calls to the same `drawTerminusText`-then-
fallback idiom bar A already established:

```cpp
if (!drawTerminusText(canvas, label, x, y, sz, color))
    canvas.textStyled(label, x, y, sz, color, FontStyle::Roman);
```

## Verification

- `terminus_glyph_test` gains an assertion list of the actual chrome strings
  enumerated above (the full, exact list is fixed during planning) — each
  checked to draw through `drawTerminusText` with **zero** fallback. This is
  the same "a missing glyph is a build-time failure" discipline the Settings
  `S` already has, extended to whole strings: if a future edit to Settings'
  copy introduces a character outside the baked ASCII set, this test catches
  it rather than a user discovering a silently-serif label.
- All 13 existing tests stay green; Debug and Release both build.
- Headless captures of all five Settings panels (Manage Folders, Audio
  Settings — including the Bluetooth codec section, EQ Settings, Folder
  Picker, Interface) at desktop size and both phone sizes, looked at by eye
  — specifically checking that 16px-strike body text is legible and that no
  string that should have converted is silently still serif.
- `EqSwitcher` and `drawSignalChain` captures byte-compared against the
  current baseline to confirm the opt-in flag left them untouched.

## Explicitly not done here

- No change to `widgets::drawScrollList` or any other `vk_canvas` submodule
  code — folder/device/profile row text stays exactly as it is.
- No change to EqSwitcher or the signal-chain scene's appearance.
- No third Terminus strike, even if 16px body text looks small in review —
  that would be a follow-up, decided after looking at the captures.
- No attempt to bake non-Latin scripts. The whole design rests on "ASCII
  chrome converts, everything else automatically doesn't."
