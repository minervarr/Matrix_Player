# Theme chips, folder import, and a cover match — design

**Goal:** The Themes page shows a chip per palette instead of a name and a
sentence, so a few hundred themes stay browsable. A folder of Alacritty
`.toml` files (or `.theme` files) can be copied in at once. A Dynamic mode,
off unless chosen, rests on Matrix and, while music is playing, switches to
the loaded palette whose page and accent are closest to the cover.

**Why now:** Twenty-four built-ins already fill `drawThemes` with a blurb
each. `REFERENCEonly/alacritty-theme/themes/` holds 177 files. The loader
already accepts every `.toml` and `.theme` in the themes directory beside
the database; the page is what does not scale. The cover match is only
worth having once that catalogue is large.

Static is the default. The chip a listener taps is saved as `ui_theme` and
stays through playback. Dynamic is the other mode.

## Two modes

One saved flag, `ui_theme_mode`, values `static` and `dynamic`. Absent or
anything else is static. Dynamic never writes `ui_theme`. The chip a
listener picked remains the static theme while Dynamic is showing something
else, and it is what comes back when Dynamic is turned off.

`chooseTheme` is a pure function. The UI thread calls it; nothing in the
audio or decode threads does.

```
chooseTheme(mode, playing, coverReady, matchId, savedId, heldId) -> id
```

| | id applied |
|---|---|
| static | `savedId`, or `matrix` when that id is empty or no longer listed |
| dynamic, not playing | `matrix` |
| dynamic, playing, cover ready | `matchId` when it is listed, otherwise `matrix` |
| dynamic, playing, cover not ready, `heldId` listed | `heldId` |
| dynamic, playing, cover not ready, no `heldId` | `matrix` |

`heldId` is the palette Dynamic last applied for a cover during this
playback. It is cleared when playback ends. It is not saved.

`playing` is true from the moment `onPlay` has started audio until one
of the two ends. `onStop` is one end. The other is the natural end: the
drain has held for `kDrainIdleTicksToEnd`, the block in `onTimer` that
already calls `media_session::end()`. That block leaves `isPlaying_`
true so the transport keeps showing the last track, so Dynamic cannot
watch `isPlaying_` for this. Both ends clear `heldId` and apply Matrix.

A new album while audio is still going does not pass through Matrix. The
current palette stays until that album's cover has colors, then it
switches once. The next track of the same `artPath` does not switch.
Seek, Next, and Prev inside one cover do not switch.

Tapping a chip sets the mode to static, saves `ui_theme`, and applies
that palette. The mode row toggles the flag, saves `ui_theme_mode`, and
applies whatever `chooseTheme` says now. Turning Dynamic on while a
record is already playing applies the match when that cover's colors
are already cached. When they are not, `heldId` is empty, so the page
stays on Matrix until the decode stores them.
Turning Dynamic off applies the saved static theme immediately.

Startup already loads `ui_theme` just after `reloadImportedThemes` in
`create()`. It also reads `ui_theme_mode`. Nothing is playing yet, so a
saved `dynamic` applies Matrix and leaves `ui_theme` stored for later.
A saved `static` applies `ui_theme` as it does today.

The Themes page uses the palette that is actually applied, including a
match that lands while the page is open. The stroke on a chip marks the
saved static id, not the live one, so the grid does not jump to follow
the cover.

## The page

`PlayerWindow::drawThemes` keeps the header (Themes, Return). Under it,
pinned, not scrolled, is the mode row, the same shape as the EQ mode row
on the settings home: one Terminus line, `STATIC MODE - CLICK TO SWITCH`
or `DYNAMIC MODE - CLICK TO SWITCH`, with `TAP` where
`Host::inputIsTouch()` is true. In Dynamic a second dim line names the
live palette (`themeAt` for `activeThemeId()`). The hit rect covers both
lines. The line fits the width the EQ row already fits (`space(718)` at
the body strike); the static/dynamic sentences are shorter than
`REFERENCE EQ MODE - CLICK TO SWITCH`.

The scroll body is two groups and a grid. The group heading stays what
it is: one Terminus strike above the names, primary ink, rule under it
in `CLR_TEXT_SECONDARY`. The headings are Night, then Day. Which side a
palette is on is `themeLuma(bgMain) > 0.45`, the same split
`fillFromAnsi` uses. `Palette::group` is not read by this page. The
built-in group strings (Monochrome, Night, Day, Terminal) stay in the
literals so `checkBuiltinThemes` and a saved `ui_theme` are untouched.

Inside a side, palettes whose accent has OKLab chroma below `0.04` come
first, ordered by id. The rest follow accent hue, then id. Hue is
Ottosson OKLab `atan2(b, a)`, with negative angles shifted by `2π`, so
the run starts at red and goes yellow, green, cyan, blue, pink. Ink's
accent is white, so it leads Night. Paper's accent is black, so it
leads Day. Matrix's accent is the green mark, so it sorts with the
greens on the Night side rather than with the grays.

A chip is a square of `space(64)` filled with `bgMain`. Inset, a bar in
`textPrimary` and a square in `accent`. When the accent is within
`themeChannelDist` 48 of the page, the square has a hairline in the type
color so it still reads. Under the chip, the name in
`metrics_.text.secondary`, dim, one line, `terminusEllipsize`d to the
chip width. The chosen static chip has a `stroke(2)` rule in
`CLR_TEXT_PRIMARY`. Hover is the existing `CLR_HOVER` fill behind the
chip. No blurb.

Columns are however many chips fit in the content width after the panel
pad, with `space(SP_SM)` between them. Rows run left to right. A narrow
window is one column. `thRowRc_[i]` is the chip rect of `themeAt(i)`,
catalogue index, not visual position, so the capture states
`3d-theme-paper` and `3e-theme-hyper` still find the row, scroll it into
`thBody_`, and click it. `thBody_` is the chip viewport under the pinned
mode row, not the mode row itself. Those two PNGs change and are
regenerated with the page. The wheel keeps `scrollDiscrete`; the pitch
is one chip row (square, name line, gap), not `panelRowH()`.

At the bottom of the scroll, one secondary button, `IMPORT THEMES`. Under
it, one dim line when the last reload or the last copy left files out:
the counts in words Terminus can draw, `N LEFT OUT` and `N ALREADY THERE`,
either or both. No per-file list.

## Import

The button opens the existing `SettingsPanel::FolderPicker`. A flag on
that visit says the commit is themes, not a music root. Select copies;
it does not call `commitAddFolder`. Return and Escape set
`activePanel_` back to Themes. They do not call `closeActivePanel()`,
which would leave the listener on the settings home.

Copy is one directory, not recursive. A regular file whose extension is
`.theme` or `.toml`, compared case-insensitively, is copied into
`app_paths::stateDir() + "themes"`. An existing destination is left in
place and counted as already there. Then `reloadImportedThemes` runs, as
it already does when the page opens. A file that fails
`paletteAcceptable`, or whose id is a built-in, is still refused there
and counted as left out. The flag is cleared as the picker leaves, so
the next music-folder visit still calls `commitAddFolder`. The
twenty-four literals in `kBuiltins` are not rewritten and not generated.

## Cover colors

Every successful album-art decode already has straight RGBA in
`ArtDecodeResult` before the texture upload (`artDecodeWorker` →
`onArtDecoded`). Artist jobs (`kArtistArtKey`) are not covers. For every
other result with pixels, the UI thread stores a `CoverColors` under the
file path and drops the buffer as it does now. A later decode of the
same path replaces the entry. If that path is the playing album's
`Album::artPath` and Dynamic is on, `chooseTheme` runs again.

Sampling reads at most a 32 by 32 grid: step `max(1, w/32)` and
`max(1, h/32)`. A sample with alpha below 128 is skipped. Fewer than 16
kept samples means the cover has no colors (`coverReady` false).

Kept samples are counted into an 8×8×8 histogram in OKLab. L is split
across `[0, 1]`, a and b across `[-0.4, 0.4]`, values outside clamped
into the end bins. Each bin keeps a count and the sum of the sample
sRGB bytes.

A bin's index is `L * 64 + a * 8 + b` with each axis in `0..7`. The
ground bin is the highest count, ties to the lower index. Its color is
the mean of those bytes, rounded. The signal bin is the highest OKLab
chroma among other bins that hold at least 8% of the kept samples and
whose mean chroma is at least `0.03`. Ties go to the higher count, then
the lower index. No such bin means there is no signal.

The match scores only the side the ground is on, again
`themeLuma(ground) > 0.45` for Day. Score is OKLab distance from the
theme's `bgMain` to the ground, plus `0.35` times the distance from
`accent` to the signal when a signal exists. Lowest score wins. Equal
scores go to the lesser id, bytewise, so catalogue order cannot change
the winner. The function takes the palettes as an argument; the test
does not need a directory of files.

`theme_test` asserts the display order of the built-ins as rules, not
as a hand-copied list of names: Night precedes Day, every quiet accent
precedes every chromatic accent on that side, and hue is non-decreasing
inside each chromatic run.

OKLab is Ottosson's published transform, no library. Linearize sRGB the
way `themeChannel` already does, then the two published matrices and a
cube root per LMS channel. Distance is Euclidean in Lab. Chroma is
`hypot(a, b)`.

An image that is mostly `#1E1E2E`, with `#CBA6F7` covering at least 8%
of the samples, picks `catppuccin` out of the built-ins. The same shape
in `#2E3440` and `#88C0D0` picks `nord`. A flat white image, which has
no signal, picks `paper`. A dark ground never picks a Day id, and a
light ground never picks a Night id. Those four are asserted in
`theme_test`.

The playing cover is `albums_[currentAlbum_].artPath` from `onPlay`, and
`albums_[album].artPath` from `applyTrackMetadata`. The grid, the
overlay scene, and the transport thumb all feed the same path cache
whenever they decode that file. No second decoder, and no read of a GPU
texture.

## Where the code lives

The order, the histogram, the match, and `chooseTheme` live next to the
catalogue (`theme_catalog.cc`, declared in `theme.hh`), so `theme_test`
links them without `player_view.cc`. `PlayerWindow` calls them. It does
not grow a second palette path: applying a result is `applyTheme`, which
already swaps every token, quality marks included.

`docs/UI_DESIGN_SYSTEM.md` replaces the Themes paragraph that lists four
hand groups and a blurb per name. `CLAUDE.md`'s Themes sentences gain
the two modes and the chip page. The contrast rules, the frozen
literals, and the original eight ids are unchanged.

## Order

The pure functions land first, with `theme_test` covering order, match,
and `chooseTheme`, including both ends of playback and a cover that is
not ready yet. The page comes next, then the `onPlay`,
`applyTrackMetadata`, `onStop`, and drain-idle call sites, then the
folder copy, then the two docs and the two captures.

## Out of scope

A palette invented from the cover. Per-track artwork. The artist
portrait. Recursive import. Editing `kBuiltins` by hand or by generator.
A third mode. Changing `paletteAcceptable`.
