# Release-type classification + quality-color aura + sidebar restructure

## Context

Matrix Player currently treats every scanned folder as an undifferentiated
"album" and has no concept of audio-quality visualization beyond a plain text
badge (`formatQualityBadge`, `player_view.cc:767`). The user recalls a feature
from a sibling Android music player (same author, "minervarr") that:

1. Classified each release as Album / EP / Single / Remix and let the user
   browse by that type.
2. Colored album covers and track lists by audio quality tier (sample
   rate/bit depth/DSD), with a "homogeneous vs mixed" rule: if every track in
   an album shares the same quality tier, the whole track list gets one
   colored border ("aura"); if tracks differ, each row gets its own color
   instead.
3. Cached the expensive-to-compute inputs (classification, aggregate sample
   rate) in the app's own database for efficiency.

The reference Android app was cloned to
`/home/nava/Documents/Files/code/reference/media_player` (outside this
project) and inspected directly — the exact logic below is transcribed from
its source, not reconstructed from memory, and verified by reading the files
directly (not solely trusting a sub-agent's report).

A prerequisite the user explicitly called out: Matrix Player's own database
(`matrix_player.db`, via the `Db` class) must never be mixed with or write to
the external `.streamer/library.db` (see `core/streamer_db.h`, added in a
previous session for artist bio/image). This was independently audited during
this session — `StreamerDb` opens with `SQLITE_OPEN_READONLY` and contains no
`INSERT`/`UPDATE`/`DELETE` anywhere; `Db` and `StreamerDb` are separate
classes/files/connections with no shared code path. This spec's new columns
only ever go into `Db`'s own `albums` table — confirmed compliant, and this
constraint is repeated here so implementation doesn't regress it.

## Reference logic (verbatim from the Android app, for exact porting)

**Classification** (`AlbumDao.java:130-168`):
```java
private int classifyRelease(SQLiteDatabase db, long albumId, String albumName, int trackCount) {
    if (isRemixAlbum(db, albumId, albumName, trackCount)) return 3; // REMIX
    if (trackCount == 1) return 2;   // SINGLE
    if (trackCount <= 4) return 1;   // EP
    return 0;                        // ALBUM
}
```
Remix detection: album name matches `\b(remix|remixes|remixed|rmx)\b`
(case-insensitive), OR count per-track title matches (`isRemixTrack`: title
contains "remix"/"rmx", or matches `\b\w+\s+mix\b`, `\(.*mix.*\)`,
`\[.*mix.*\]`, EXCLUDING literal titles "remix"/"mix"/"the remix"/"the mix")
and `remixCount == trackCount || (remixCount >= 2 && remixCount * 2 > trackCount)`.

**Quality-color palette** (`CategoryAdapter.java:70-86`,
`GroupedFragment.java:340-350` — identical thresholds in both):
```
DSD (format DSF/DFF/DS*)      -> WHITE
sampleRate >= 352800           -> #FFA500 (orange)  — DXD / very-high-rate PCM
sampleRate >= 64000             -> #00FFFF (cyan)    — hi-res PCM (88.2/96/176.4/192kHz)
sampleRate >= 44100             -> YELLOW              — standard CD-quality PCM
else                             -> no border (transparent)
```

**Homogeneity / "aura" logic** (`GroupedFragment.java:337-376`): iterate the
album's tracks, compute each track's tier color; if every track's color
matches, the *whole* track-list container gets a single colored stroke; the
moment one track's tier differs, `isMixed = true` and each row gets its own
tier-color border instead — never both, never neither.

**Caching** (`MatrixPlayerDatabase.java:85-99`, `AlbumDao.java:38-42`): the
`albums` table stores `avg_sample_rate` (SQL `AVG` across tracks) and
`has_dsd` (SQL `MAX(CASE...)`), computed once during a library rebuild — not
recomputed per render. The homogeneity check itself is NOT cached — it's
recomputed live each time an album's detail view opens, from the in-memory
track list, since that's O(track count) and cheap.

## Design for the C++ port

### 1. Sidebar restructure

Today `activeNavItem_` (`player_view.hh:444`) is a binary 0=Albums/1=Settings
switch, and the sidebar draws exactly two nav items (`player_view.cc:853-854`).
Split this into two independent pieces of state:

- A **content-type filter** replacing the single "Albums" item with four:
  **Albums / EPs / Singles / Remixes** — same visual family as today (grey
  hover pill, accent-tint pill + left bar when active), filtering the
  existing grid by each album's cached `release_type`. All four share the
  same grid-drawing code path (`drawFrame()`'s grid block,
  `player_view.cc:881-982`); the only change is which `gridIndices_` get
  included in the current filter.
- A **Settings gear**, spatially separated at the bottom of the sidebar below
  a `CLR_SEPARATOR` hairline — never inside the content-type list. Clicking
  it opens the existing Settings flow unchanged. Returning from Settings goes
  back to whichever content-type filter was active before (not hardcoded to
  Albums).
- New icon: `UiIcon::Settings` in `drawUiIcon` (`player_view.cc:648-670`) —
  same 36-unit-grid vector style as Play/Stop/Prev/Next. A 5-tooth gear
  silhouette: a central hub (rounded rect, near-circle) plus 5 trapezoidal
  teeth placed at 72° increments, computed via `cos`/`sin` around the icon's
  center (no `Canvas::setRotation` needed — the tooth vertices are just
  computed directly per angle and drawn as `triangle`/`rect` calls, matching
  how existing icons are hand-built from primitives). Solid silhouette, no
  center hole (simpler, reads better at small sizes); a hollow-center variant
  is a trivial follow-up since the exact background color behind each call
  site is always known.

State changes needed: replace `activeNavItem_`'s single int with (a) an
`enum class AlbumTypeFilter { Album, Ep, Single, Remix }` plus an
`albumTypeFilter_` member driving grid filtering, and (b) a separate
`settingsOpen_` bool toggled only by the gear. Existing `activeNavItem_ != 0`
checks throughout `player_view.cc` (grid hit-testing, drawFrame branches,
mouse handling — see the ~15 call sites found via grep) get migrated to check
`settingsOpen_` instead; the four content filters replace the implicit
"activeNavItem_ == 0" meaning "Albums."

### 2. Classification: computed once, cached in `Db`'s own `albums` table

Add to `Album` struct (`core/include/core/library.h`): `int releaseType` (0
Album/1 EP/2 Single/3 Remix), `int avgSampleRate`, `bool hasDsd`. Computed in
`buildAlbums()` (`core/src/library.cpp:107-137`, the shared tail every scan
variant already funnels through) using the exact thresholds/regex above
(C++ `<regex>` for the remix patterns, translated 1:1 from the Java
`Pattern`s). `avgSampleRate`/`hasDsd` computed by iterating the album's
already-collected `Track` list (mean of `sampleRate` where `> 0`; `hasDsd`
stays permanently `false` today since this app has no DSD/DSF file decoding
yet per `CLAUDE.md`'s "Not yet wired: DoP" — the field is kept for forward
compatibility with the existing palette, not dead code, just unreachable
until DSD decode lands).

`Db::saveAlbums`/`loadAlbums` (`core/src/db.cpp:156-280`) gain the three new
columns via the existing `ALTER TABLE` migration pattern
(`core/src/db.cpp:53-61`), same as every prior schema change.

### 3. Quality-color palette: new documented theme tokens

Add to `gui/src/theme.hh`, following the existing documented-exception
pattern used for `CLR_ERROR`/`CLR_WARNING`:
```cpp
static constexpr ColorRef CLR_QUALITY_DSD      = RGB(255, 255, 255);
static constexpr ColorRef CLR_QUALITY_DXD      = RGB(255, 165, 0);   // #FFA500
static constexpr ColorRef CLR_QUALITY_HIRES    = RGB(0, 255, 255);   // #00FFFF
static constexpr ColorRef CLR_QUALITY_STANDARD = RGB(255, 255, 0);   // yellow
```
with a comment documenting this as a deliberate, scoped second palette for
*objective audio-quality metadata*, never for UI state/hover (which stays
green-only per `UI_DESIGN_SYSTEM.md` rule 1). `docs/UI_DESIGN_SYSTEM.md`
gets a new subsection documenting these tokens and where they're drawn,
per the doc's own "update both the code and this doc" rule.

A small helper (new, e.g. in `player_view.cc` near `formatQualityBadge`):
```cpp
// Returns {false,...} for "no border" (below CD quality), matching the
// Android reference's TRANSPARENT case.
struct QualityColor { bool hasColor; ColorRef color; };
static QualityColor qualityColorFor(int sampleRate, bool isDsd);
```

### 4. Where it's drawn

- **Grid tile** (`player_view.cc`, grid draw loop ~897-980): a thin
  (2-3px) border hugging the art's exact bounds, using the album's cached
  `avgSampleRate`/`hasDsd`. This sits *inside* the existing outer
  hover/selection/now-playing rings (drawn at `x-6..x-9` offsets, i.e.
  outside the art) — a new, non-competing layer, not a replacement.
- **Album view / track list** (`player_view.cc:1122+` track row loop): at
  album-open time, iterate `album.tracks`, compute each track's tier via
  `qualityColorFor(t.sampleRate, false)` (DSD not reachable yet), and either:
  - all same tier and it has a color → draw one square-cornered border
    (4 `canvas.rect` hairline calls, the same technique already used for the
    settings-row outlined box at `player_view.cc:1029-1032` — square, not
    Android's 16dp rounded corners, per this project's
    `UI_CORNER_RADIUS = 0` rule) around the track-list region
    (`trackListLeft_`/`trackListRight_`/`trackListTop_`/`tracksBottom`,
    already computed at `player_view.cc:1115-1171`), or
  - mixed → no list-wide border; each row gets its own thin border in its
    own track's tier color instead.

### 5. Database boundary (explicitly verified, not just assumed)

All three new columns (`release_type`, `avg_sample_rate`, `has_dsd`) are
added only to `Db`'s own `albums` table (`core/src/db.cpp`). `StreamerDb`
(`core/src/streamer_db.cpp`) is untouched by this feature — confirmed via
direct grep audit that it opens `SQLITE_OPEN_READONLY` and contains zero
write statements, and that `Db`/`StreamerDb` share no code, connection, or
schema. This spec adds no new coupling between them.

## Non-goals / explicitly out of scope

- Artist/Folder browsing tabs (the Android reference has them; not requested
  here).
- DSD/DSF file decoding (unrelated prior TODO item; `hasDsd` stays `false`
  until that lands, harmlessly).
- Caching the per-track homogeneity check itself (Android doesn't either —
  it's recomputed live from in-memory tracks, which is cheap).
- Using `.streamer`'s own `release_type`/`product_type` values as a
  classification hint — deliberately not layered in; this spec replicates
  the Android heuristic exactly, as requested ("de la misma forma").

## Verification plan

No automated test suite exists in this repo (per `CLAUDE.md`). Manual plan:

1. Build (`scripts/linux/build.sh --debug`); confirm compile/link.
2. Run against the real library (`/home/nava/Documents/Usick`); confirm the
   sidebar shows Albums/EPs/Singles/Remixes + a bottom-separated gear, and
   that Settings still opens/closes correctly and returns to the prior filter.
3. Cross-check classification against known albums via
   `sqlite3 .../.streamer/library.db "SELECT id,title,tracks_count FROM albums;"`
   — e.g. `yx5nv8hwqxkoc` "Claire" (3 tracks) should classify as EP,
   `lpgj56m5yxrzb` "Player Of Games" (1 track) as Single, multi-track albums
   as Album, and any album with "Remix"-titled tracks as Remix.
4. Confirm an album with uniform sample rate shows one list-wide aura border
   in the correct tier color, and (if a mixed-quality album exists or is
   constructed for testing) confirm per-row borders instead.
5. Confirm grid tiles show the quality border without visually conflicting
   with hover/selection/now-playing rings.
6. Re-confirm `.streamer/library.db`'s mtime/hash is unchanged after a full
   session (same check as the prior bio/image feature).
