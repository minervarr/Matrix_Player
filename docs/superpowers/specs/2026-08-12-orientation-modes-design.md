# Orientation modes: Horizontal and Vertical

*2026-08-12*

## What changes, in one sentence

`UiMode{Essential, Complete}` — a *window size asked of the OS* — is replaced by
`Orientation{Horizontal, Vertical}` — a *layout the view chooses from the shape it
already has* — and the sidebar plus transport bar become **two symmetric bars of
equal thickness** facing each other across a centred grid.

Both orientations exist on **all three platforms**. A vertical monitor on the desktop
is as much a first-class case as a phone held upright; a phone in landscape gets the
horizontal layout. Vertical is not "the mobile one".

## Why the current design has to go

`UiMode` is not an interface. Both hosts use it only to pick a window rectangle
(`gui/src/os/linux_host.cc:63`, `gui/src/os/windows_host.cc:109`), and the drawing code
consults it in about six places to *hide* things. There is no UI designed for a mode;
there is one UI, trimmed.

That framing is also what makes the Alt-key window commands dead on Linux:
`LinuxHost::snapToEdge()` and `adaptToCurrentMonitor()` are empty bodies
(`gui/src/os/linux_host.cc:104`, `:110`), because a Wayland client cannot position
itself or ask which monitor it is on. The observed "Alt+G/H/L are slow and don't resize"
on KDE Plasma is not slowness — nothing was ever requested. The mode toggle *is* slow,
for a different reason: `applyUiMode()` asks the compositor to go fullscreen, the
`configure` comes back asynchronously, and `onHostResized()`
(`gui/src/player_view.cc:739`) then recreates the swapchain before laying out.

Orientation derived from the window's own dimensions needs none of this. There is no
size to ask for, no monitor to query, and no compositor round-trip. The same rule works
unchanged on Wayland, Windows and Android, where a device rotation arrives as an
ordinary resize.

## The frame

Two bars of the same thickness — `space(130)`, the current transport bar's height
(`gui/src/player_view.cc:2247`) — with the album grid centred between them.

| | Bar A — *where I am* | Bar B — *what is playing* |
|---|---|---|
| Vertical | top | bottom |
| Horizontal | left | right |

Bar A carries navigation, search, settings, and the AutoEQ quick-switcher. Bar B is
today's transport bar.

Today's sidebar is `space(277)` wide (`:2253`), 2.1× the transport bar. Collapsing it to
130 is not a tweak: nothing in it survives with text. 130 is, however, almost exactly the
transport bar's artwork square (`artSide = transportH - 2*tPad`, `:2373`) — the thickness
is an icon-rail width by construction.

### Horizontal is Vertical rotated 90° counter-clockwise

Every position specified below was checked against this single transform, and it holds:
the left end of the top bar maps to the bottom of the left bar, the right end maps to the
top, and text rotated the same way reads bottom-to-top. **There are not two layouts. There
is one layout and a rotation.**

`Canvas::setRotation(radians, pivotX, pivotY)` already exists
(`framework/vk_canvas/core/canvas.hh:195`) and text honours it (`:175`). `image()` does
**not** (`:124`), so nothing in either bar may depend on a rotated bitmap. The transport
artwork is square and does not need one.

## Bar A, end to end

In **Reference EQ**, from the outer end inward:

```
[ filter letters ][ S search ][ S settings ] · · · · · [ AutoEQ box ]
```

In **bit-perfect** there is no AutoEQ box, and the whole group **centres** in the bar,
recomputed from the live window size — not from a stored constant.

The navigation group therefore jumps between centred and pegged when the EQ mode changes.
This is a direct consequence of the rule and is **instant, not animated**.

### The letters

Filters are their initials as plain text — A, E, S, C, L, R for the six release types,
P for Playlists — with purpose-drawn icons a later possibility. Settings and Search are
also letters, and collide with Singles on `S`.

Collisions are resolved by **lightness**, using the ordered ladder that already exists in
`gui/src/theme.hh:12-25`:

| role | token | value | contrast |
|---|---|---|---|
| filters | `CLR_TEXT_PRIMARY` | 242 | 17.7:1 |
| search | `CLR_TEXT_SECONDARY` | 170 | 8.5:1 |
| settings | `CLR_TEXT_DIM` | 128 | **4.58:1 — the floor** |

The precedence rule, for any future collision: **content filters are lightest; then
search; then settings.** Search outranks settings because search is about the music and
settings is not.

**The budget is three steps, and it is now full.** In CIE L\* those values are ≈95 / 70 / 54.
Two text labels need roughly ΔL\* ≥ 10-12 to be told apart at a glance; the current steps
are 25 and 16, comfortably above it. But the bottom of the range is fixed by legibility,
not taste — 128 is already at WCAG AA with almost no margin, and `theme.hh` says so in
place. Between 242 and that floor, five steps fit at ΔL\* = 12 and three fit at the current
comfortable spacing. A fourth `S` would have nowhere to go.

Three identical letters separated only by brightness are hard to learn on day one. What
teaches them is **fixed position**, not colour: settings is always at the same end, search
is always beside the filter group. Colour confirms what position already established. A
hover label removes the remaining cost without adding width.

### Opening search

Pressing the search letter:

- the **filter letters collapse**;
- the **AutoEQ box hides** as well;
- **Settings stays**, anchored to its own side — **bottom** in horizontal, **left** in
  vertical — because interrupting a filter to change a setting must not cost what was typed;
- an **`X` takes the far end** — top in horizontal, right in vertical — in the place the
  search letter occupied;
- the **search field spans the middle**, between the two anchors.

Because the AutoEQ box hides, the open-search bar is **identical in bit-perfect and in
Reference EQ**. It is drawn once.

The `X` restores everything.

### Search is the guided search that already exists — unchanged

The field is not a sentence parser. `core/src/facets.cpp` stands as it is: the listener
types, suggestions are computed against the real library, an accepted suggestion becomes a
chip, chips of the same group OR and different groups AND, and `explainEmpty()` still names
the chip that killed a result. Nothing about the two-kinds-of-nothing rule, the O(n)
`suggest()` work, or `facets_test.cc` changes. The rail only gives it a different shape to
live in.

### The AutoEQ box

A bounded rectangle at Bar A's settings-side end, visually separated so it reads as its
own region. It holds two things: a discreet **`X`** meaning *no profile* — unlabelled,
legible from context — and the **active profile's name**. In horizontal orientation the
name is rotated to read bottom-to-top.

Touching the name **unfurls the list from the anchor to the far end of the bar**. It is not
clipped to a few rows: with a typical inventory of about seven profiles, showing them all
and letting the eye travel beats scrolling.

This supersedes today's sidebar block, which clamps to the space available —
`kEqHpMaxRows` (`gui/src/player_view.hh:1071`) and `drawHeadphoneBlock()`'s "drop rows from
the LIST rather than hide the block" rule exist only because the sidebar had three rows
below Settings. An unfurling list has no such ceiling, so **the clamp goes away and the
saved list is shown whole**. The pin/most-used/most-recent ordering, the 60-second credit
gate, `eqCreditBaselineMs_`, and `clearEqProfile()`'s deletion of the `"global"` row are
untouched — those are about *which* profiles exist, not about how many fit.

### One unfurling list, not two

The AutoEQ list and the search-suggestion list are the same mechanic: anchor at a point in
the bar, unfurl to the far end, scroll only if it overflows. They are one widget, written
once and used twice.

## What this requires in the code

### Orientation replaces UiMode

`UiMode` is deleted. `Host::init(PlayerWindow*, UiMode)`, `applyUiMode()`,
`adaptToCurrentMonitor()` and `snapToEdge()` lose their mode argument or go entirely; the
two Linux no-ops disappear with them rather than remaining as dead documentation.

`Orientation` is **derived from the window's own width and height**, defaulting to
automatic. A hotkey (desktop) or a button (mobile) overrides it manually, and a setting
disables the automatic behaviour. No monitor API, no sensor, no compositor request — a
rotated phone and a dragged window edge are the same event.

### The presentation layer comes out of player_view.cc

`gui/src/player_view.cc` is 7198 lines carrying layout, drawing, hit-testing, playback
orchestration and the gapless coordinator together. The two bars are extracted into a
presentation module that talks only to `Canvas` and to a **view model of already-resolved
data** (what is playing, the track list, the chips, the artwork), and returns **intents**
("play track N", "select profile P") rather than acting.

That module is the file Android consumes **literally unchanged**. A symlink or a copy of
`player_view.hh` cannot work — it drags in `Host`, `Db`, `AudioOutput` and `FolderWatcher`.
The shared file has to be the one that drags in nothing, which is the same discipline that
already keeps `core/`, `variants.cpp` and `facets.cpp` portable. This is what turns the
Android target from a vertical slice into the same app in a different shape.

## Out of scope

- **The simple variant** of each orientation (thumbnail plus play button, and its
  interaction) — deliberately deferred to its own design.
- **The four settings panels.** They keep their full-page overlay unchanged; only the way
  they are reached moves into the rail.
- Purpose-drawn icons replacing the initials. The letters are the minimum that works.

## Testing

- `facets_test`, `variants_test`, `stats_test`, `ui_metrics_test`, `ui_icons_test` and
  `ui_text_test` must all still pass, untouched — this change adds no reason for any of
  them to move.
- Orientation selection is pure arithmetic on window dimensions and gets its own
  assert-based test, in the same convention: it must not need a window to answer.
- Bar A's layout — where each anchor lands, in each orientation, in each EQ mode, with
  search open and closed — is likewise arithmetic over a rectangle and is tested directly,
  without a Canvas. This is the part most likely to break silently.
- `tools/ui_capture` reaches states by synthesising clicks on the rects `recalcLayout()`
  computed, so it follows the new layout as long as the rects stay the source of truth.
  Captures in both orientations are the visual check.
