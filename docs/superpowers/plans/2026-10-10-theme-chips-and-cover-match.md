# Theme chips, folder import, and cover match — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** The Themes page is a chip grid grouped Night then Day, a folder of theme files can be copied in at once, and Dynamic mode (off unless chosen) rests on Matrix and follows the playing cover.

**Architecture:** Order, the cover histogram, the match, and `chooseTheme` live in `theme_catalog.cc` and are asserted by `theme_test` with no window. `PlayerWindow` only calls them and applies the result through `applyTheme`. Dynamic never writes `ui_theme`.

**Tech Stack:** C++17, the existing Terminus settings chrome, assert-based Debug `theme_test`, no new libraries. OKLab is Ottosson's published transform.

**Spec:** `docs/superpowers/specs/2026-10-10-theme-chips-and-cover-match-design.md`

## Global Constraints

- Static is the default. `ui_theme_mode` is `static` or `dynamic`; anything else, including absent, is static. Dynamic never writes `ui_theme`.
- `chooseTheme(mode, playing, coverReady, matchId, savedId, heldId)` returns `savedId` or `matrix` when static; `matrix` when dynamic and not playing; the listed `matchId` or else `matrix` when dynamic, playing, and the cover is ready (no fall-through to `heldId`); `heldId` when dynamic, playing, the cover is not ready, and `heldId` is listed; otherwise `matrix`.
- `heldId` is cleared when playback ends. Playback ends in `onStop` and in the `onTimer` drain block that calls `media_session::end()` after `kDrainIdleTicksToEnd`. That block leaves `isPlaying_` true. `themePlaying_` is set only after `output_->start()` succeeds, and cleared on those two ends and on the three `onPlay` returns that set `isPlaying_` false.
- A new cover holds the current dynamic palette until its colors exist. The same `artPath` does not switch. No pass through Matrix between albums.
- Chip stroke marks the saved static id (`matrix` when that string is empty), not the live match.
- Groups are computed: Night then Day by `themeLuma(bgMain) > 0.45`. Quiet accents (OKLab chroma `< 0.04`) first, by id; then hue `atan2(b, a)` with negative angles shifted by `2π`, then id. `Palette::group` is not read by the page.
- Chip is `space(64)`, page fill, inset type bar, accent square, hairline in the type color when `themeChannelDist(accent, page) < 48`. Name underneath in `metrics_.text.secondary`, dim, `terminusEllipsize`d to the chip width. Hover is `CLR_HOVER` behind the chip. Columns from the content width, gap `space(SP_SM)`.
- `thRowRc_[i]` is the chip of `themeAt(i)`. `thBody_` is the viewport under the pinned mode row. Wheel pitch is one chip row.
- Mode row: `STATIC MODE - CLICK TO SWITCH` / `DYNAMIC MODE - CLICK TO SWITCH`, `TAP` when `Host::inputIsTouch()`. Dynamic adds a dim line naming the live palette. Hit rect covers both lines.
- Import copies one directory, extensions `.theme` and `.toml` case-insensitively, into `app_paths::stateDir() + "themes"`. An existing destination stays and counts as already there. Then `reloadImportedThemes`. Contrast failures and built-in ids stay refused and count as left out. Return and Escape from that picker land on Themes. The flag clears as the picker leaves. `kBuiltins` is not rewritten.
- Cover samples: step `max(1, w/32)`, `max(1, h/32)`, skip alpha `< 128`, fewer than 16 kept samples means not ready. 8×8×8 OKLab histogram, bin `L*64 + a*8 + b`. Ground is the highest count, tie lower index. Signal is the highest mean chroma among other bins with at least 8% of samples and mean chroma at least `0.03`; ties higher count, then lower index. Score is OKLab distance of `bgMain` to the ground plus `0.35` times accent-to-signal when a signal exists. Equal scores (within `1e-9`) take the lesser id. Only the ground's side is scored.
- Artist jobs (`kArtistArtKey`) are not covers. Transport art decodes once in `loadTransportArtTexture` and samples that buffer. No second decoder and no GPU read.
- Settings stay Terminus. The music surface stays New Computer Modern. Contrast rules, frozen literals, and the original eight ids are unchanged.
- Do not commit with `git_wrapper` or `git add -A`. The working tree already holds unrelated edits.

## Review Focus

- Natural end leaves `isPlaying_` true. Dynamic must still return to Matrix, once, from the drain-idle block. Pinned by `chooseTheme(..., playing=false)` and by calling `themePlaybackEnded()` only while `themePlaying_` is still set.
- `onPlay` sets `isPlaying_` true before `configure` / `start` can fail. `themePlaying_` is armed only after `start()` returns true. The three failure returns call `themePlaybackEnded()`.
- Transport art does not pass through `onArtDecoded`. Sampling only there would miss a cover that was never a grid tile. One `decodeImageScaled` inside `loadTransportArtTexture` fills the same path cache, including when the GPU texture is already current and the colors are not.
- A cover-ready match that is not listed applies Matrix and does not keep `heldId`. Pinned by `chooseTheme`.
- Flat black ties `chicago95`, `hyper`, and `ink`. The dark-ground assertion is the side, not an id.

---

### Task 1: Catalogue order, cover match, chooseTheme, copy

**Files:**
- Modify: `gui/src/theme.hh` (declarations after `reloadImportedThemes`)
- Modify: `gui/src/theme_catalog.cc` (definitions after `reloadImportedThemes`, skip counter inside `reloadImportedThemes`)
- Test: `gui/src/theme_test.cc`

**Interfaces:**
- Consumes: `themeAt`, `themeCount`, `themeLuma`, `themeChannel`, `reloadImportedThemes`
- Produces:
  - `enum class ThemeMode { Static, Dynamic }`
  - `struct CoverColors { bool ready; bool hasSignal; ColorRef ground; ColorRef signal; }`
  - `struct ThemeCopyResult { int copied; int already; }`
  - `void themeDisplayOrder(std::vector<int>& out)`
  - `bool themeIsDay(const Palette&)`
  - `bool themeAccentQuiet(const Palette&)`
  - `double themeAccentHue(const Palette&)`
  - `CoverColors coverColorsFromRgba(const uint8_t* rgba, int w, int h)`
  - `int matchTheme(const CoverColors&, const Palette* pals, int n)` — index, or `-1`
  - `const char* chooseTheme(ThemeMode, bool playing, bool coverReady, const char* matchId, const char* savedId, const char* heldId)` — an argument pointer or the literal `"matrix"`
  - `int themeSkipCount()`
  - `ThemeCopyResult copyThemeFiles(const std::string& fromDir, const std::string& toDir)`

- [x] **Step 1: Write the failing test**

Append to `theme_test.cc`, and assert `themeSkipCount() == 3` beside `added == 2`, `== 3` after the second reload, and `== 2` after the removal reload (matrix.theme + garbage.theme remain).

Assertions, using the built-in catalogue (`themeCount() == 24`):

- Display order: every index once; Night before Day; quiet before chromatic on each side; hue non-decreasing inside each chromatic run; equal hue breaks by id; Ink quiet Night, Paper quiet Day, Matrix chromatic Night.
- Mostly `#1E1E2E` with a 10×10 `#CBA6F7` block on 32×32 picks `catppuccin`. Mostly `#2E3440` with `#88C0D0` picks `nord`. Flat 16×16 white has no signal and picks `paper`. Flat black's winner is not Day. A day-only list does not match black. A night-only list does not match white. Two identical Paper clones, ids `bbb` then `aaa`, white image, picks `aaa`.
- Alpha 0, alpha 100, and 3×3 opaque are not ready. 4×4 opaque is ready. Null pixels are not ready.
- `chooseTheme`: static returns the saved id, or `matrix` when empty or unlisted. Dynamic and not playing returns `matrix` even with a held id (the natural end). Dynamic, playing, cover ready returns the listed match, and `matrix` rather than the held id when the match is unlisted. Dynamic, playing, cover not ready returns the held id, or `matrix` when it is empty or unlisted.
- `copyThemeFiles`: `a.toml` and `B.THEME` copy; `notes.txt` and `sub/skip.toml` do not; a second call reports `already == 2`, `copied == 0`.

- [x] **Step 2: Run test to verify it fails**

Run: `ninja -C build/linux_debug theme_test && ./build/linux_debug/gui/theme_test`

Expected: link fails with undefined references, or the binary is not produced.

- [x] **Step 3: Implement the catalogue functions**

OKLab in the anonymous namespace of `theme_catalog.cc`. `gThemeSkips` resets at the start of `reloadImportedThemes` and counts unreadable files, parse failures, built-in ids, and duplicate ids. Non-theme files are not counted. A directory that cannot be created leaves the skip count at 0.

- [x] **Step 4: Run test to verify it passes**

Run: `ninja -C build/linux_debug theme_test && ./build/linux_debug/gui/theme_test`

Expected: exit 0.

- [x] **Step 5: Do not commit**

`git_wrapper` runs `git add -A` and would take the unrelated dirty tree.

### Task 2: Themes page, import picker, playback wiring

**Files:**
- Modify: `gui/src/player_view.hh` (members and the four methods)
- Modify: `gui/src/player_view.cc` (`drawThemes`, hit testing, wheel, startup, `onPlay`, `onStop`, drain idle, `applyTrackMetadata`, `onArtDecoded`, `loadTransportArtTexture`, folder picker)
- Modify: `docs/UI_DESIGN_SYSTEM.md` (Themes paragraph)
- Modify: `CLAUDE.md` (Themes sentences)

**Interfaces:**
- Consumes: Task 1's functions, `applyTheme`, `loadSetting` / `saveSetting`
- Produces: `applyPlaybackTheme()`, `themePlaybackEnded()`, `noteCoverColors(path, rgba, w, h)`, `openThemeImport()`. Members `themeDynamic_`, `themePlaying_`, `themeSavedId_`, `themeHeldId_`, `themeArtPath_`, `themeAlready_`, `fpForThemes_`, `thModeRc_`, `thImportRc_`, `thHoverMode_`, `thHoverImport_`, `thChipPitch_`, `coverColors_`.

- [x] **Step 1: Wire the page and the call sites**

`applyPlaybackTheme` builds a `Palette` array from `themeAt`, calls `matchTheme` and `chooseTheme`, stores `themeHeldId_` only when dynamic, playing, the cover is ready, and the returned id equals the match id, then `applyTheme`. It does not save `ui_theme`.

Startup loads `ui_theme` into `themeSavedId_` and `ui_theme_mode` into `themeDynamic_`, then `applyPlaybackTheme()` with nothing playing.

A chip sets static, saves both keys, clears `themeHeldId_`, and applies that id. The mode row toggles `ui_theme_mode` only. Turning dynamic off clears `themeHeldId_`.

`closeActivePanel` while `fpForThemes_` returns to Themes and clears the flag. Select on that visit copies, reloads, stores `themeAlready_`, sets the panel to Themes, and clears the flag without `commitAddFolder`. `onAddFolder` clears the flag. The picker title is `Select Theme Folder` on that visit.

`noteCoverColors` skips empty paths. `onArtDecoded` calls it for every non-artist result that has pixels, before the key-specific continues. `loadTransportArtTexture` decodes with `ImageFit::kPerAxis` and `mips=true` when the texture must be rebuilt, and still decodes when the texture is current but this path has no `CoverColors` yet.

`themePlaying_ = true` and `themeArtPath_` update sit after `output_->start()` succeeds. `applyTrackMetadata` updates `themeArtPath_` and calls `applyPlaybackTheme` only when the path changed. `themePlaybackEnded` runs from `onStop`, from the three failed-start returns, and once from the drain-idle block.

- [x] **Step 2: Update the two docs**

Replace the Themes paragraph in `docs/UI_DESIGN_SYSTEM.md` and the Themes sentences in `CLAUDE.md` so they describe chips, Night/Day, the mode row, import, and Dynamic. Leave contrast rules and the original eight ids as they are.

- [x] **Step 3: Build and run theme_test again**

Run: `ninja -C build/linux_debug theme_test matrix_player && ./build/linux_debug/gui/theme_test`

Expected: exit 0, and `matrix_player` links.

- [x] **Step 4: Regenerate the two theme captures if the capture tool runs**

States `3d-theme-paper` and `3e-theme-hyper`. If the tool cannot open a surface, say so. Do not rename the states.

- [x] **Step 5: Do not commit**
