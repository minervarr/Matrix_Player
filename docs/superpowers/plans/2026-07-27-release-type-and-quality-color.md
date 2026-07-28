# Release-Type Classification + Quality-Color Aura Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Classify each scanned album as Album/EP/Single/Remix, color-code album art and track lists by audio quality tier, and let the user browse by type from the sidebar — all cached in Matrix Player's own database, never touching the read-only external `.streamer` database.

**Architecture:** Port the sibling Android player's exact classification/color logic (verified directly against `/home/nava/Documents/Files/code/reference/media_player`) into `core/` (pure C++, no OS deps) for classification/quality-stat computation, persist the results as new columns on `Db`'s own `albums` table, add a small color-tier helper to `theme.hh`, and restructure the sidebar to expose four type filters plus a spatially-separated Settings gear.

**Tech Stack:** C++17, SQLite (via the existing vendored `sqlite3.c`), vk_canvas (Vulkan) for rendering — no new dependencies.

## Global Constraints

- `core/` has zero OS headers (the project's one rule, see root `CLAUDE.md`) — all classification/quality-stat code goes in `core/src/library.cpp` / `core/include/core/library.h`, no GUI/OS includes.
- Never write to or alter the schema of the external `.streamer/library.db` (`core/src/streamer_db.cpp`) — this feature only adds columns to `Db`'s own `matrix_player.db` (`core/src/db.cpp`). Verify this stays true after every task.
- No animation, square corners only (`UI_CORNER_RADIUS = 0`), green (`CLR_ACCENT`) reserved for UI state — the new quality-color tokens are an explicitly documented second palette, never used for hover/selection.
- No automated test suite exists in this repo (confirmed in root `CLAUDE.md`) — "run the test" steps below use small standalone `g++`-compiled harnesses in the scratchpad directory (not committed to the repo), plus the final task's full manual build+run against the real library at `/home/nava/Documents/Usick`.
- Full spec: `docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md`.

---

### Task 1: Release-type classification (pure core logic)

**Files:**
- Modify: `core/include/core/library.h` (add `Album::ReleaseType` enum + fields, declare `classifyReleaseType`)
- Modify: `core/src/library.cpp` (implement classification, ported from `AlbumDao.java:130-168` in the reference repo)
- Test: standalone harness at `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify.cpp`

**Interfaces:**
- Produces: `enum class Album::ReleaseType { Album = 0, Ep = 1, Single = 2, Remix = 3 };` and
  `Album::ReleaseType classifyReleaseType(const std::string& albumName, const std::vector<Track>& tracks);`
  declared in `core/include/core/library.h`, usable by any later task without further includes.

- [ ] **Step 1: Add the enum and fields to `Album`, and declare `classifyReleaseType`**

In `core/include/core/library.h`, inside `struct Album` (right after the existing `std::vector<Track> tracks;` line, before `void sortTracks();`):

```cpp
    // Release-type classification (Album/EP/Single/Remix) and quality-tier
    // inputs, computed once in buildAlbums() and cached in Db's own albums
    // table (never touches the external streamer db) — see
    // classifyReleaseType()/computeAlbumQualityStats() below.
    enum class ReleaseType { Album = 0, Ep = 1, Single = 2, Remix = 3 };
    ReleaseType releaseType   = ReleaseType::Album;
    int         avgSampleRate = 0;
    bool        hasDsd        = false;
```

And after the `Album` struct's closing `};`, add these two free-function declarations:

```cpp
// Classifies a release from its track list, mirroring the sibling Android
// player's AlbumDao.classifyRelease() exactly: track-count thresholds
// (1=Single, 2-4=EP, >4=Album), overridden by remix detection (album name
// or a strict majority of track titles matching remix patterns).
Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks);

// Mean sample rate across tracks with sampleRate > 0 (0 if none), and
// whether any track is DSD. hasDsd is always false today — this app has no
// DSD/DSF file decoding yet (see root CLAUDE.md's "Not yet wired: DoP") —
// the field exists for forward compatibility with the quality-color palette.
void computeAlbumQualityStats(const std::vector<Track>& tracks,
                              int& avgSampleRate, bool& hasDsd);
```

- [ ] **Step 2: Write the standalone test harness (will fail to compile — the functions don't exist yet)**

Create `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify.cpp`:

```cpp
#include "core/library.h"
#include <cassert>
#include <cstdio>

static Track mk(const std::string& title, int sampleRate = 44100) {
    Track t;
    t.title = title;
    t.sampleRate = sampleRate;
    return t;
}

int main() {
    // 1 track -> Single
    { std::vector<Track> tr = { mk("Solo") };
      assert(classifyReleaseType("Solo", tr) == Album::ReleaseType::Single); }

    // 3 tracks -> EP
    { std::vector<Track> tr = { mk("A"), mk("B"), mk("C") };
      assert(classifyReleaseType("Claire", tr) == Album::ReleaseType::Ep); }

    // 4 tracks -> EP (boundary)
    { std::vector<Track> tr = { mk("A"), mk("B"), mk("C"), mk("D") };
      assert(classifyReleaseType("Sentient EP", tr) == Album::ReleaseType::Ep); }

    // 5 tracks -> Album (boundary)
    { std::vector<Track> tr = { mk("A"), mk("B"), mk("C"), mk("D"), mk("E") };
      assert(classifyReleaseType("Big Album", tr) == Album::ReleaseType::Album); }

    // 34 tracks -> Album
    { std::vector<Track> tr; for (int i = 0; i < 34; i++) tr.push_back(mk("T" + std::to_string(i)));
      assert(classifyReleaseType("F*CK U SKRILLEX", tr) == Album::ReleaseType::Album); }

    // Album name matches remix regex -> Remix regardless of track count
    { std::vector<Track> tr = { mk("A"), mk("B"), mk("C"), mk("D"), mk("E"), mk("F") };
      assert(classifyReleaseType("Greatest Hits (Remixes)", tr) == Album::ReleaseType::Remix); }

    // All track titles are remixes -> Remix
    { std::vector<Track> tr = { mk("Song A (Radio Remix)"), mk("Song A (Club Rmx)") };
      assert(classifyReleaseType("Song A Remixed", tr) == Album::ReleaseType::Remix); }
    { std::vector<Track> tr2 = { mk("Song A (Radio Remix)"), mk("Song A (Club Rmx)") };
      assert(classifyReleaseType("Song A", tr2) == Album::ReleaseType::Remix); }

    // Strict majority (2 of 3) remix titles -> Remix
    { std::vector<Track> tr = { mk("Track One Remix"), mk("Track Two Rmx"), mk("Track Three") };
      assert(classifyReleaseType("Various", tr) == Album::ReleaseType::Remix); }

    // Exactly half (1 of 2) is NOT a strict majority -> not Remix (falls to count rule)
    { std::vector<Track> tr = { mk("Track One Remix"), mk("Track Two") };
      assert(classifyReleaseType("Various", tr) == Album::ReleaseType::Single ? false : true); }
    // (2 tracks, 1 remix: not majority -> EP, since trackCount<=4)
    { std::vector<Track> tr = { mk("Track One Remix"), mk("Track Two") };
      assert(classifyReleaseType("Various", tr) == Album::ReleaseType::Ep); }

    // Literal title "Remix" alone must NOT count as a remix track
    { std::vector<Track> tr = { mk("Remix"), mk("Intro") };
      assert(classifyReleaseType("Various", tr) == Album::ReleaseType::Ep); }

    // computeAlbumQualityStats: mean of positive sample rates, hasDsd always false
    { std::vector<Track> tr = { mk("A", 44100), mk("B", 48000), mk("C", 96000) };
      int avg = 0; bool dsd = true;
      computeAlbumQualityStats(tr, avg, dsd);
      assert(avg == (44100 + 48000 + 96000) / 3);
      assert(dsd == false); }

    // computeAlbumQualityStats: ignores sampleRate <= 0
    { std::vector<Track> tr = { mk("A", 0), mk("B", 44100) };
      int avg = 0; bool dsd = false;
      computeAlbumQualityStats(tr, avg, dsd);
      assert(avg == 44100); }

    printf("ALL CLASSIFY TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Attempt to compile the harness and confirm it fails**

Run:
```bash
g++ -std=c++17 -I/home/nava/Documents/Files/code/done/Matrix_Player/core/include \
  /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify.cpp \
  /home/nava/Documents/Files/code/done/Matrix_Player/core/src/library.cpp \
  -o /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify
```
Expected: compile error — `classifyReleaseType`/`computeAlbumQualityStats` undefined reference (declared in Step 1 but not yet implemented).

- [ ] **Step 4: Implement `classifyReleaseType` and `computeAlbumQualityStats` in `core/src/library.cpp`**

Add near the top of `core/src/library.cpp`, after the existing `namespace fs = std::filesystem;` line and before `parseAlbumFolder`:

```cpp
// ── Release-type classification (Album/EP/Single/Remix) ─────────────────────
// Ported verbatim from the sibling Android player's AlbumDao.java
// (isRemixTrack/isRemixAlbum/classifyRelease) — see the design spec at
// docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md.
namespace {

bool isRemixTrackTitle(const std::string& title) {
    if (title.empty()) return false;
    std::string lower = title;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    size_t b = lower.find_first_not_of(" \t");
    size_t e = lower.find_last_not_of(" \t");
    std::string trimmed = (b == std::string::npos) ? "" : lower.substr(b, e - b + 1);
    if (trimmed == "remix" || trimmed == "mix" ||
        trimmed == "the remix" || trimmed == "the mix") return false;
    if (lower.find("remix") != std::string::npos) return true;
    if (lower.find("rmx") != std::string::npos) return true;
    static const std::regex kWordMix(R"(\b\w+\s+mix\b)", std::regex::icase);
    static const std::regex kParenMix(R"(\(.*mix.*\))", std::regex::icase);
    static const std::regex kBracketMix(R"(\[.*mix.*\])", std::regex::icase);
    if (std::regex_search(title, kWordMix))    return true;
    if (std::regex_search(title, kParenMix))   return true;
    if (std::regex_search(title, kBracketMix)) return true;
    return false;
}

bool isRemixAlbum(const std::string& albumName, const std::vector<Track>& tracks) {
    static const std::regex kRemixName(R"(\b(remix|remixes|remixed|rmx)\b)", std::regex::icase);
    if (!albumName.empty() && std::regex_search(albumName, kRemixName)) return true;
    int trackCount = (int)tracks.size();
    if (trackCount == 0) return false;
    int remixCount = 0;
    for (auto& t : tracks)
        if (isRemixTrackTitle(t.title)) remixCount++;
    return remixCount == trackCount || (remixCount >= 2 && remixCount * 2 > trackCount);
}

} // namespace

Album::ReleaseType classifyReleaseType(const std::string& albumName,
                                       const std::vector<Track>& tracks) {
    if (isRemixAlbum(albumName, tracks)) return Album::ReleaseType::Remix;
    int trackCount = (int)tracks.size();
    if (trackCount == 1) return Album::ReleaseType::Single;
    if (trackCount <= 4) return Album::ReleaseType::Ep;
    return Album::ReleaseType::Album;
}

void computeAlbumQualityStats(const std::vector<Track>& tracks,
                              int& avgSampleRate, bool& hasDsd) {
    long long sum = 0;
    int count = 0;
    for (auto& t : tracks) {
        if (t.sampleRate > 0) { sum += t.sampleRate; count++; }
    }
    avgSampleRate = count > 0 ? (int)(sum / count) : 0;
    hasDsd = false;  // no DSD/DSF decode yet — see the declaration's comment
}
```

- [ ] **Step 5: Recompile and run the harness — confirm it passes**

Run the same `g++` command from Step 3, then run the produced binary. Expected output: `ALL CLASSIFY TESTS PASSED` with exit code 0.

- [ ] **Step 6: Delete the scratch harness (not part of the repo)**

```bash
rm -f /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify.cpp \
      /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_classify
```

- [ ] **Step 7: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add core/include/core/library.h core/src/library.cpp
git status
```
(Use `git_wrapper commit` per this repo's convention — see root `CLAUDE.md`'s Committing section — with a message describing the new classification/quality-stat functions.)

---

### Task 2: Wire classification into `buildAlbums()`

**Files:**
- Modify: `core/src/library.cpp` (`buildAlbums`, currently lines 107-137)

**Interfaces:**
- Consumes: `classifyReleaseType`, `computeAlbumQualityStats` from Task 1 (`core/include/core/library.h`).
- Produces: every `Album` returned by `scanLibrary`/`scanLibraryIncremental`/`scanLibraryParallel` now has `releaseType`/`avgSampleRate`/`hasDsd` populated (all three funnel through `buildAlbums`).

- [ ] **Step 1: Add the classification/stats calls in `buildAlbums`**

In `core/src/library.cpp`, inside `buildAlbums` (the `static std::vector<Album> buildAlbums(...)` function), find:

```cpp
        if (!metaAlbum.empty()) album.displayName = metaAlbum;
        album.sortTracks();
        albums.push_back(std::move(album));
```

Replace with:

```cpp
        if (!metaAlbum.empty()) album.displayName = metaAlbum;
        album.releaseType = classifyReleaseType(album.displayName, album.tracks);
        computeAlbumQualityStats(album.tracks, album.avgSampleRate, album.hasDsd);
        album.sortTracks();
        albums.push_back(std::move(album));
```

- [ ] **Step 2: Rebuild `matrix_core` to confirm it compiles**

```bash
cmake --build /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native -- matrix_core
```
Expected: builds cleanly (no errors). This is the verification step for this task — `buildAlbums` is a file-local `static` function, so Task 1's exported functions are what's independently testable; this task's own correctness is that the call sites match the signatures from Task 1 exactly (verified by successful compilation) and is re-verified end-to-end in Task 8 against the real library.

- [ ] **Step 3: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add core/src/library.cpp
```
(Commit via `git_wrapper commit`.)

---

### Task 3: Persist `release_type`/`avg_sample_rate`/`has_dsd` in `Db`'s own `albums` table

**Files:**
- Modify: `core/src/db.cpp` (`SCHEMA`, `MIGRATIONS`, `Db::saveAlbums`, `Db::loadAlbums`)
- Test: standalone harness at `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema.cpp`

**Interfaces:**
- Consumes: `Album::releaseType` (`Album::ReleaseType`), `Album::avgSampleRate` (int), `Album::hasDsd` (bool) from Task 1.
- Produces: `Db::saveAlbums`/`Db::loadAlbums` round-trip these three fields; this is the ONLY place they're persisted, and only in `Db`'s own SQLite file — `core/src/streamer_db.cpp` is untouched by this task.

- [ ] **Step 1: Add the columns to the base `SCHEMA` (fresh installs)**

In `core/src/db.cpp`, inside the `albums` `CREATE TABLE` block of the `SCHEMA` string (currently lines 25-34), add three lines right before the closing `);`:

```cpp
CREATE TABLE IF NOT EXISTS albums (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT,
    artist       TEXT,
    art_path     TEXT,
    display_name TEXT DEFAULT '',
    quality      TEXT DEFAULT '',
    mode         TEXT DEFAULT '',
    country      TEXT DEFAULT '',
    release_type    INTEGER DEFAULT 0,
    avg_sample_rate INTEGER DEFAULT 0,
    has_dsd         INTEGER DEFAULT 0
);
```

- [ ] **Step 2: Add migrations for existing installs (the user's own `matrix_player.db` already exists without these columns)**

In `core/src/db.cpp`, append to the `MIGRATIONS[]` array (currently ending at line 60 with `"ALTER TABLE albums ADD COLUMN country TEXT DEFAULT '';",`):

```cpp
    "ALTER TABLE albums ADD COLUMN release_type INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN avg_sample_rate INTEGER DEFAULT 0;",
    "ALTER TABLE albums ADD COLUMN has_dsd INTEGER DEFAULT 0;",
```

- [ ] **Step 3: Update `Db::saveAlbums` to write the three new columns**

Replace the current `Db::saveAlbums` body's SQL and bindings:

```cpp
void Db::saveAlbums(const std::vector<Album>& albums) {
    if (!impl_->db) return;
    sqlite3_exec(impl_->db, "BEGIN;", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "DELETE FROM albums;", nullptr, nullptr, nullptr);
    const char* sql = "INSERT INTO albums (name, artist, art_path, "
                      "display_name, quality, mode, country, "
                      "release_type, avg_sample_rate, has_dsd) VALUES (?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    for (auto& a : albums) {
        sqlite3_bind_text(stmt, 1, a.name.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, a.artist.c_str(),      -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, a.artPath.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, a.displayName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, a.quality.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, a.mode.c_str(),        -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, a.country.c_str(),     -1, SQLITE_TRANSIENT);
        sqlite3_bind_int (stmt, 8, (int)a.releaseType);
        sqlite3_bind_int (stmt, 9, a.avgSampleRate);
        sqlite3_bind_int (stmt, 10, a.hasDsd ? 1 : 0);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);
}
```

- [ ] **Step 4: Update `Db::loadAlbums` to read the three new columns**

Replace the current `Db::loadAlbums` body:

```cpp
std::vector<Album> Db::loadAlbums() {
    std::vector<Album> out;
    if (!impl_->db) return out;
    const char* sql = "SELECT name, artist, art_path, "
                      "display_name, quality, mode, country, "
                      "release_type, avg_sample_rate, has_dsd FROM albums;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Album a;
        auto col = [&](int i) -> std::string {
            auto* s = (const char*)sqlite3_column_text(stmt, i);
            return s ? s : "";
        };
        a.name        = col(0);
        a.artist      = col(1);
        a.artPath     = col(2);
        a.displayName = col(3);
        a.quality     = col(4);
        a.mode        = col(5);
        a.country     = col(6);
        a.releaseType   = (Album::ReleaseType)sqlite3_column_int(stmt, 7);
        a.avgSampleRate = sqlite3_column_int(stmt, 8);
        a.hasDsd        = sqlite3_column_int(stmt, 9) != 0;
        // Rows written before the display_name migration: fall back to the
        // raw folder name so the grid never renders empty titles.
        if (a.displayName.empty()) a.displayName = a.name;
        out.push_back(std::move(a));
    }
    sqlite3_finalize(stmt);
    return out;
}
```

- [ ] **Step 5: Write the round-trip test harness (will fail to compile first — the new columns/bindings don't exist until Steps 1-4 are done; if you're implementing top-to-bottom they'll already exist, so instead run it once before Step 1's edits are applied to a *scratch copy* of db.cpp is unnecessary — just run it after Steps 1-4 and confirm PASS, since this is schema/round-trip code where "does it compile against the old 7-column INSERT" isn't a meaningful red state)**

Create `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema.cpp`:

```cpp
#include "core/db.h"
#include <cassert>
#include <cstdio>
#include <cstdio>

int main() {
    const char* path = "/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_schema.db";
    remove(path);

    Db db;
    bool ok = db.open(path);
    assert(ok);

    Album a;
    a.name = "test-codename";
    a.artist = "Test Artist";
    a.displayName = "Test Album";
    a.releaseType = Album::ReleaseType::Ep;
    a.avgSampleRate = 96000;
    a.hasDsd = false;
    db.saveAlbums({ a });

    auto loaded = db.loadAlbums();
    assert(loaded.size() == 1);
    assert(loaded[0].name == "test-codename");
    assert(loaded[0].releaseType == Album::ReleaseType::Ep);
    assert(loaded[0].avgSampleRate == 96000);
    assert(loaded[0].hasDsd == false);

    printf("DB SCHEMA ROUND-TRIP TEST PASSED\n");
    remove(path);
    return 0;
}
```

- [ ] **Step 6: Compile and run the harness**

```bash
g++ -std=c++17 -I/home/nava/Documents/Files/code/done/Matrix_Player/core/include \
  -I/home/nava/Documents/Files/code/done/Matrix_Player/third_party \
  /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema.cpp \
  /home/nava/Documents/Files/code/done/Matrix_Player/core/src/db.cpp \
  -lsqlite3 -o /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema
/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema
```
Expected: `DB SCHEMA ROUND-TRIP TEST PASSED`, exit code 0.

- [ ] **Step 7: Delete the scratch harness**

```bash
rm -f /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema.cpp \
      /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_db_schema
```

- [ ] **Step 8: Audit that `.streamer` is still untouched**

```bash
grep -n "sqlite3_exec\|INSERT\|UPDATE\|DELETE\|ALTER" /home/nava/Documents/Files/code/done/Matrix_Player/core/src/streamer_db.cpp
```
Expected: no output (confirms this task added nothing write-related to `streamer_db.cpp`).

- [ ] **Step 9: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add core/src/db.cpp
```
(Commit via `git_wrapper commit`.)

---

### Task 4: Quality-color palette tokens + `qualityColorFor()` helper

**Files:**
- Modify: `gui/src/theme.hh`
- Modify: `docs/UI_DESIGN_SYSTEM.md` (document the new tokens)
- Test: standalone harness at `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color.cpp`

**Interfaces:**
- Produces: `CLR_QUALITY_DSD`/`CLR_QUALITY_DXD`/`CLR_QUALITY_HIRES`/`CLR_QUALITY_STANDARD` (`ColorRef`), `struct QualityColor { bool hasColor; ColorRef color; };`, and
  `inline QualityColor qualityColorFor(int sampleRate, bool isDsd);` — all in `gui/src/theme.hh`, usable by `player_view.cc` with no extra includes (Tasks 6-7 depend on this).

- [ ] **Step 1: Write the test harness first (will fail to compile — nothing exists yet)**

Create `/tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color.cpp`:

```cpp
#include "theme.hh"
#include <cassert>
#include <cstdio>

int main() {
    auto dsd = qualityColorFor(0, true);
    assert(dsd.hasColor && dsd.color == CLR_QUALITY_DSD);

    auto dxd = qualityColorFor(352800, false);
    assert(dxd.hasColor && dxd.color == CLR_QUALITY_DXD);

    auto dxdAbove = qualityColorFor(384000, false);
    assert(dxdAbove.hasColor && dxdAbove.color == CLR_QUALITY_DXD);

    auto hires = qualityColorFor(96000, false);
    assert(hires.hasColor && hires.color == CLR_QUALITY_HIRES);

    auto hiresBoundary = qualityColorFor(64000, false);
    assert(hiresBoundary.hasColor && hiresBoundary.color == CLR_QUALITY_HIRES);

    auto standard = qualityColorFor(44100, false);
    assert(standard.hasColor && standard.color == CLR_QUALITY_STANDARD);

    auto standard48 = qualityColorFor(48000, false);
    assert(standard48.hasColor && standard48.color == CLR_QUALITY_STANDARD);

    auto none = qualityColorFor(22050, false);
    assert(!none.hasColor);

    auto zero = qualityColorFor(0, false);
    assert(!zero.hasColor);

    // DSD wins regardless of sampleRate value passed alongside it
    auto dsdWins = qualityColorFor(44100, true);
    assert(dsdWins.hasColor && dsdWins.color == CLR_QUALITY_DSD);

    printf("QUALITY COLOR TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Compile and confirm it fails**

```bash
g++ -std=c++17 -I/home/nava/Documents/Files/code/done/Matrix_Player/gui/src \
  /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color.cpp \
  -o /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color
```
Expected: compile error — `qualityColorFor`/`CLR_QUALITY_*` undeclared.

- [ ] **Step 3: Add the tokens and helper to `gui/src/theme.hh`**

Append to the end of `gui/src/theme.hh` (after the existing `UI_SELECT_TINT_ALPHA` line):

```cpp

// ── Audio-quality color tiers ────────────────────────────────────────────────
// A second, deliberately scoped palette for OBJECTIVE audio-quality metadata
// (sample rate / DSD), shown as a border/"aura" on album art and track lists
// — never for UI state (state stays CLR_ACCENT-only, see design principle
// #4 in docs/UI_DESIGN_SYSTEM.md). Thresholds/colors ported verbatim from
// the sibling Android player's CategoryAdapter/GroupedFragment quality-tier
// logic (see docs/superpowers/specs/2026-07-27-release-type-and-quality-color-design.md).
static constexpr ColorRef CLR_QUALITY_DSD      = RGB(255, 255, 255);
static constexpr ColorRef CLR_QUALITY_DXD      = RGB(255, 165, 0);   // #FFA500, >=352.8kHz
static constexpr ColorRef CLR_QUALITY_HIRES    = RGB(0, 255, 255);   // #00FFFF, >=64kHz
static constexpr ColorRef CLR_QUALITY_STANDARD = RGB(255, 255, 0);   // >=44.1kHz (CD quality)

struct QualityColor {
    bool     hasColor = false;
    ColorRef color    = 0;
};

// sampleRate in Hz (e.g. 44100, not 44.1). isDsd wins over sampleRate tiers.
// Below 44.1kHz there's no tier — this deliberately mirrors the Android
// reference's TRANSPARENT ("no border") case, not an error.
inline QualityColor qualityColorFor(int sampleRate, bool isDsd) {
    if (isDsd)                return { true, CLR_QUALITY_DSD };
    if (sampleRate >= 352800) return { true, CLR_QUALITY_DXD };
    if (sampleRate >= 64000)  return { true, CLR_QUALITY_HIRES };
    if (sampleRate >= 44100)  return { true, CLR_QUALITY_STANDARD };
    return { false, 0 };
}
```

- [ ] **Step 4: Recompile and run the harness — confirm it passes**

Run the same `g++` command from Step 2, then run the binary. Expected: `QUALITY COLOR TESTS PASSED`, exit code 0.

- [ ] **Step 5: Delete the scratch harness**

```bash
rm -f /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color.cpp \
      /tmp/claude-1000/-home-nava-Documents-Files-code-done-Matrix-Player/2f19d568-6362-48f5-8722-1b5ed48925be/scratchpad/test_quality_color
```

- [ ] **Step 6: Document the new tokens in `docs/UI_DESIGN_SYSTEM.md`**

In `docs/UI_DESIGN_SYSTEM.md`, right after the "## 2. Color tokens" table's closing row (`| CLR_ERROR | ... |`) and before the "**Contrast note:**" paragraph, add a new subsection:

```markdown

**Quality-color tier (a second, scoped palette).** Album art borders and
track-list "auras" (§8.2/§8.4) are colored by objective audio quality, not
UI state — this is the one deliberate exception to "one palette":

| Token | RGB | Tier |
|---|---|---|
| `CLR_QUALITY_DSD` | 255,255,255 | DSD |
| `CLR_QUALITY_DXD` | 255,165,0 | >=352.8kHz (DXD) |
| `CLR_QUALITY_HIRES` | 0,255,255 | >=64kHz (hi-res PCM) |
| `CLR_QUALITY_STANDARD` | 255,255,0 | >=44.1kHz (CD quality) |

Below 44.1kHz, no border is drawn. `qualityColorFor(sampleRate, isDsd)`
(`gui/src/theme.hh`) is the single place this mapping lives.
```

- [ ] **Step 7: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add gui/src/theme.hh docs/UI_DESIGN_SYSTEM.md
```
(Commit via `git_wrapper commit`.)

---

### Task 5: Sidebar restructure — four type filters + a separated Settings gear

**Files:**
- Modify: `gui/src/player_view.hh` (replace `activeNavItem_`; add `AlbumTypeFilter`, `settingsOpen_`, new nav rects)
- Modify: `gui/src/player_view.cc` (`drawUiIcon`, sidebar draw block, `recalcLayout`, `sidebarHitTest`, click handler, `rebuildGridIndices`, and every other `activeNavItem_` call site)

**Interfaces:**
- Consumes: nothing new from other tasks (this task is UI-state-only; it reads `Album::releaseType` from Task 1/2 in `rebuildGridIndices`).
- Produces: `bool settingsOpen_`, `enum class AlbumTypeFilter { Album, Ep, Single, Remix }`, `AlbumTypeFilter albumTypeFilter_` on `PlayerWindow` — later tasks (6, 7) don't consume these directly, but Task 8's manual verification exercises them.

- [ ] **Step 1: Replace `activeNavItem_` with `settingsOpen_` + `AlbumTypeFilter` in `player_view.hh`**

Find (currently `player_view.hh:444`):
```cpp
    int  activeNavItem_     = 0;  // 0=Albums, 1=Settings
```
Replace with:
```cpp
    // Sidebar is two independent things: which album TYPE is being browsed
    // (Albums/EPs/Singles/Remixes — filters the grid), and whether the
    // Settings gear is open (replaces the whole content area). They're
    // deliberately separate state: leaving Settings returns to whichever
    // type filter was active, never hardcoded back to Albums.
    enum class AlbumTypeFilter { Album, Ep, Single, Remix };
    AlbumTypeFilter albumTypeFilter_ = AlbumTypeFilter::Album;
    bool            settingsOpen_    = false;
    static constexpr int kSidebarGearHit = 4;  // sidebarHitTest() sentinel for the gear
```

- [ ] **Step 2: Replace `rcNavAlbums_`/`rcNavSettings_` with five rects in `player_view.hh`**

Find (currently `player_view.hh:296-297`):
```cpp
    LayoutRect rcNavAlbums_   = {};
    LayoutRect rcNavSettings_ = {};
```
Replace with:
```cpp
    LayoutRect rcNavAlbum_    = {};
    LayoutRect rcNavEp_       = {};
    LayoutRect rcNavSingle_   = {};
    LayoutRect rcNavRemix_    = {};
    LayoutRect rcNavGear_     = {};
```

- [ ] **Step 3: Update `recalcLayout()` in `player_view.cc`**

Find (currently `player_view.cc:1457-1458`):
```cpp
    rcNavAlbums_   = { 0, (int)(102 * us), sidebarW, (int)(142 * us) };
    rcNavSettings_ = { 0, (int)(142 * us), sidebarW, (int)(182 * us) };
```
Replace with:
```cpp
    float navRowH = 40.0f * us, navTop = 102.0f * us;
    rcNavAlbum_  = { 0, (int)(navTop),               sidebarW, (int)(navTop + navRowH) };
    rcNavEp_     = { 0, (int)(navTop + navRowH),     sidebarW, (int)(navTop + navRowH * 2) };
    rcNavSingle_ = { 0, (int)(navTop + navRowH * 2), sidebarW, (int)(navTop + navRowH * 3) };
    rcNavRemix_  = { 0, (int)(navTop + navRowH * 3), sidebarW, (int)(navTop + navRowH * 4) };
    rcNavGear_   = { 0, (int)(navTop + navRowH * 4 + 8.0f * us),
                        sidebarW, (int)(navTop + navRowH * 5 + 8.0f * us) };
```

- [ ] **Step 4: Add the `Settings` gear icon to `drawUiIcon`**

In `player_view.cc`, find the `enum class UiIcon { Play, Stop, Prev, Next };` line and replace with:
```cpp
enum class UiIcon { Play, Stop, Prev, Next, Settings };
```

In `drawUiIcon`'s `switch (icon)`, add a new case (after the existing `Next` case, before the closing `}` of the switch):
```cpp
    case UiIcon::Settings: {
        // 5-tooth gear: circular hub + 5 teeth at 72° increments, using the
        // same rotation transform vk_canvas already exposes — no new
        // primitive needed.
        float cx = X(18), cy = Y(18);
        float hubR = s * 10.0f / 36.0f;
        c.rect(cx - hubR, cy - hubR, hubR * 2, hubR * 2, col, hubR);
        float toothW = s * 7.0f / 36.0f, toothH = s * 9.0f / 36.0f;
        for (int i = 0; i < 5; i++) {
            float angle = i * (2.0f * 3.14159265f / 5.0f);
            c.setRotation(angle, cx, cy);
            c.rect(cx - toothW * 0.5f, cy - hubR - toothH * 0.55f,
                   toothW, toothH, col, toothW * 0.3f);
            c.clearRotation();
        }
        break;
    }
```

- [ ] **Step 5: Rewrite the sidebar nav draw block in `player_view.cc`**

Find the block from `struct NavItem { const char* label; LayoutRect rc; int idx; };` through the closing `}` of its `for` loop (currently `player_view.cc:851-871`):
```cpp
        struct NavItem { const char* label; LayoutRect rc; int idx; };
        NavItem items[] = {
            { "Albums",   rcNavAlbums_,   0 },
            { "Settings", rcNavSettings_, 1 },
        };
        for (auto& item : items) {
            bool active = (activeNavItem_ == item.idx);
            bool hovered = (hoverSidebarItem_ == item.idx && !active);
            Rect r = toRect(item.rc);
            if (active) {
                // Selected: accent-tint fill + left bar, full height + square —
                // matches the hover highlight exactly (one selection family).
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(r.x + 4, r.y, 3.0f, r.h, toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            canvas.text(item.label, r.x + 20, r.y + r.h * 0.5f - textSizes_.nav * 0.5f,
                       textSizes_.nav, toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }
```
Replace with:
```cpp
        struct NavItem { const char* label; LayoutRect rc; AlbumTypeFilter filter; };
        NavItem items[] = {
            { "Albums",  rcNavAlbum_,  AlbumTypeFilter::Album  },
            { "EPs",     rcNavEp_,     AlbumTypeFilter::Ep     },
            { "Singles", rcNavSingle_, AlbumTypeFilter::Single },
            { "Remixes", rcNavRemix_,  AlbumTypeFilter::Remix  },
        };
        for (auto& item : items) {
            bool active = (!settingsOpen_ && albumTypeFilter_ == item.filter);
            bool hovered = (hoverSidebarItem_ == (int)item.filter && !active && !settingsOpen_);
            Rect r = toRect(item.rc);
            if (active) {
                // Selected: accent-tint fill + left bar, full height + square —
                // matches the hover highlight exactly (one selection family).
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(r.x + 4, r.y, 3.0f, r.h, toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            canvas.text(item.label, r.x + 20, r.y + r.h * 0.5f - textSizes_.nav * 0.5f,
                       textSizes_.nav, toColor(active ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }

        // Settings gear — spatially separated below a hairline, never mixed
        // into the content-type list above (the user's explicit ask: a
        // music player should read as albums-and-music first, configuration
        // second).
        canvas.rect((float)rcNavGear_.left, (float)rcNavGear_.top, sb.w, 1, toColor(CLR_SEPARATOR));
        {
            bool hovered = (hoverSidebarItem_ == kSidebarGearHit && !settingsOpen_);
            Rect r = toRect(rcNavGear_);
            if (settingsOpen_) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h,
                            toColor(CLR_ACCENT, UI_SELECT_TINT_ALPHA), UI_CORNER_RADIUS);
                canvas.rect(r.x + 4, r.y, 3.0f, r.h, toColor(CLR_ACCENT), UI_CORNER_RADIUS);
            } else if (hovered) {
                canvas.rect(r.x + 4, r.y, r.w - 8, r.h, toColor(CLR_HOVER), UI_CORNER_RADIUS);
            }
            LayoutRect gearIconRc = { rcNavGear_.left + 16, (int)(r.y + r.h * 0.5f - 9),
                                      rcNavGear_.left + 34, (int)(r.y + r.h * 0.5f + 9) };
            drawUiIcon(canvas, gearIconRc, UiIcon::Settings,
                      toColor(settingsOpen_ ? CLR_ACCENT : CLR_TEXT_SECONDARY));
        }
```

- [ ] **Step 6: Rewrite `sidebarHitTest`**

Find (currently `player_view.cc:1889-1893`):
```cpp
int PlayerWindow::sidebarHitTest(int x, int y) const {
    if (ptInRect(rcNavAlbums_, x, y)) return 0;
    if (ptInRect(rcNavSettings_, x, y)) return 1;
    return -1;
}
```
Replace with:
```cpp
int PlayerWindow::sidebarHitTest(int x, int y) const {
    if (ptInRect(rcNavAlbum_, x, y))  return (int)AlbumTypeFilter::Album;
    if (ptInRect(rcNavEp_, x, y))     return (int)AlbumTypeFilter::Ep;
    if (ptInRect(rcNavSingle_, x, y)) return (int)AlbumTypeFilter::Single;
    if (ptInRect(rcNavRemix_, x, y))  return (int)AlbumTypeFilter::Remix;
    if (ptInRect(rcNavGear_, x, y))   return kSidebarGearHit;
    return -1;
}
```

- [ ] **Step 7: Rewrite the sidebar click handler**

Find (currently `player_view.cc:2025-2033`):
```cpp
    // Sidebar
    if (ptInRect(rcSidebar_, x, y)) {
        int nav = sidebarHitTest(x, y);
        if (nav >= 0 && nav != activeNavItem_) {
            activeNavItem_ = nav;
            invalidate();
        }
        return;
    }
```
Replace with:
```cpp
    // Sidebar
    if (ptInRect(rcSidebar_, x, y)) {
        int nav = sidebarHitTest(x, y);
        if (nav == kSidebarGearHit) {
            if (!settingsOpen_) { settingsOpen_ = true; invalidate(); }
        } else if (nav >= 0 &&
                   (settingsOpen_ || albumTypeFilter_ != (AlbumTypeFilter)nav)) {
            settingsOpen_ = false;
            albumTypeFilter_ = (AlbumTypeFilter)nav;
            rebuildGridIndices();
            invalidate();
        }
        return;
    }
```

- [ ] **Step 8: Migrate every remaining `activeNavItem_` call site to `settingsOpen_`**

Each of these is a mechanical substitution — `activeNavItem_ == 0` becomes `!settingsOpen_`, `activeNavItem_ != 0` and `activeNavItem_ == 1` both become `settingsOpen_` (only two states exist now: type-browsing or settings):

| File:line (before this task) | Before | After |
|---|---|---|
| `player_view.cc:881` | `if (activeNavItem_ == 0 && !trackPanelOpen_) {` | `if (!settingsOpen_ && !trackPanelOpen_) {` |
| `player_view.cc:983` | `} else if (activeNavItem_ != 0 && activePanel_ != SettingsPanel::None) {` | `} else if (settingsOpen_ && activePanel_ != SettingsPanel::None) {` |
| `player_view.cc:987` | `} else if (activeNavItem_ != 0) {` | `} else if (settingsOpen_) {` |
| `player_view.cc:1044` | `if (activeNavItem_ == 0 && trackPanelOpen_) {` | `if (!settingsOpen_ && trackPanelOpen_) {` |
| `player_view.cc:1876` | `if (!trackPanelOpen_ \|\| activeNavItem_ != 0) return -1;` | `if (!trackPanelOpen_ \|\| settingsOpen_) return -1;` |
| `player_view.cc:1954` | `} else if (trackPanelOpen_ && activeNavItem_ == 0 && ptInRect(rcTrackPanel_, x, y)) {` | `} else if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {` |
| `player_view.cc:1957` | `if (activeNavItem_ == 0)` | `if (!settingsOpen_)` |
| `player_view.cc:2038` | `if (trackPanelOpen_ && activeNavItem_ == 0 && ptInRect(rcTrackPanel_, x, y)) {` | `if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {` |
| `player_view.cc:2054` | `if (activeNavItem_ == 0 && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {` | `if (!settingsOpen_ && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {` |
| `player_view.cc:2061` | `if (activeNavItem_ == 1 && ptInRect(rcGrid_, x, y)) {` | `if (settingsOpen_ && ptInRect(rcGrid_, x, y)) {` |
| `player_view.cc:2086` | `if (activeNavItem_ == 0 && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {` | `if (!settingsOpen_ && !trackPanelOpen_ && ptInRect(rcGrid_, x, y)) {` |
| `player_view.cc:2115` | `if (trackPanelOpen_ && activeNavItem_ == 0 && ptInRect(rcTrackPanel_, x, y)) {` | `if (trackPanelOpen_ && !settingsOpen_ && ptInRect(rcTrackPanel_, x, y)) {` |

After this step, `grep -n "activeNavItem_" gui/src/player_view.cc gui/src/player_view.hh` must return **no results** — confirms every call site migrated (Step 1 already removed the declaration, so leftover references would now be compile errors anyway).

- [ ] **Step 9: Filter `rebuildGridIndices()` by `albumTypeFilter_`**

Find (currently `player_view.cc:1818-1832`):
```cpp
void PlayerWindow::rebuildGridIndices() {
    gridIndices_.clear();
    gridIndices_.reserve(albums_.size());
    for (int i = 0; i < (int)albums_.size(); i++) {
        if (!searchQuery_.empty()) {
            const Album& a = albums_[i];
            bool hit = containsNoCase(a.displayName, searchQuery_) ||
                       containsNoCase(a.artist, searchQuery_);
            for (size_t t = 0; !hit && t < a.tracks.size(); t++)
                hit = containsNoCase(a.tracks[t].title, searchQuery_);
            if (!hit) continue;
        }
        gridIndices_.push_back(i);
    }
}
```
Replace with:
```cpp
void PlayerWindow::rebuildGridIndices() {
    gridIndices_.clear();
    gridIndices_.reserve(albums_.size());
    for (int i = 0; i < (int)albums_.size(); i++) {
        const Album& a = albums_[i];
        if ((int)a.releaseType != (int)albumTypeFilter_) continue;
        if (!searchQuery_.empty()) {
            bool hit = containsNoCase(a.displayName, searchQuery_) ||
                       containsNoCase(a.artist, searchQuery_);
            for (size_t t = 0; !hit && t < a.tracks.size(); t++)
                hit = containsNoCase(a.tracks[t].title, searchQuery_);
            if (!hit) continue;
        }
        gridIndices_.push_back(i);
    }
}
```

- [ ] **Step 10: Rebuild the full app and confirm it compiles and links**

```bash
cmake --build /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native -- matrix_player
```
Expected: builds cleanly. (This task is GUI-state wiring with no pure-logic subset to unit-test in isolation — its correctness is verified by clean compilation now and manual interaction in Task 8.)

- [ ] **Step 11: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add gui/src/player_view.hh gui/src/player_view.cc
```
(Commit via `git_wrapper commit`.)

---

### Task 6: Grid tile quality-color border

**Files:**
- Modify: `gui/src/player_view.cc` (grid draw loop)

**Interfaces:**
- Consumes: `qualityColorFor` (Task 4), `Album::avgSampleRate`/`Album::hasDsd` (Task 1/2).

- [ ] **Step 1: Add the border draw call in the grid tile loop**

In `player_view.cc`, find the now-playing/selected ring block (currently lines 920-929):
```cpp
                    // Now-playing: unmistakable green glow border (stronger
                    // than hover, stronger than selection).
                    if (nowPlaying) {
                        canvas.rect(x - 9, y - 9, a + 18, a + 18, toColor(CLR_ACCENT, 0.20f), 12.0f);
                        canvas.rect(x - 6, y - 6, a + 12, a + 12, toColor(CLR_ACCENT, 0.45f), 10.0f);
                        canvas.rect(x - 3, y - 3, a + 6,  a + 6,  toColor(CLR_ACCENT),        8.0f);
                    } else if (selectedAlbumIdx_ == idx) {
                        canvas.rect(x - 3, y - 3, a + 6, a + 6, toColor(CLR_ACCENT, 0.8f), 8.0f);
                    }

                    drawArtOrPlaceholder(canvas, getGridArtTexture(idx), x, y, a, a);
```
Replace with (adds a quality-color frame hugging the art's exact bounds — inside the accent glow rings, so it never competes with them):
```cpp
                    // Now-playing: unmistakable green glow border (stronger
                    // than hover, stronger than selection).
                    if (nowPlaying) {
                        canvas.rect(x - 9, y - 9, a + 18, a + 18, toColor(CLR_ACCENT, 0.20f), 12.0f);
                        canvas.rect(x - 6, y - 6, a + 12, a + 12, toColor(CLR_ACCENT, 0.45f), 10.0f);
                        canvas.rect(x - 3, y - 3, a + 6,  a + 6,  toColor(CLR_ACCENT),        8.0f);
                    } else if (selectedAlbumIdx_ == idx) {
                        canvas.rect(x - 3, y - 3, a + 6, a + 6, toColor(CLR_ACCENT, 0.8f), 8.0f);
                    }

                    // Quality-color frame — objective audio-quality metadata,
                    // hugging the art's own bounds (not the outer state rings
                    // above, which sit further out and never overlap this).
                    QualityColor qc = qualityColorFor(alb.avgSampleRate, alb.hasDsd);
                    if (qc.hasColor) {
                        float bw = 2.0f * uiScale_;
                        canvas.rect(x - bw, y - bw, a + bw * 2, bw, toColor(qc.color));
                        canvas.rect(x - bw, y + a, a + bw * 2, bw, toColor(qc.color));
                        canvas.rect(x - bw, y - bw, bw, a + bw * 2, toColor(qc.color));
                        canvas.rect(x + a, y - bw, bw, a + bw * 2, toColor(qc.color));
                    }

                    drawArtOrPlaceholder(canvas, getGridArtTexture(idx), x, y, a, a);
```

- [ ] **Step 2: Rebuild and confirm it compiles**

```bash
cmake --build /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native -- matrix_player
```
Expected: builds cleanly.

- [ ] **Step 3: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add gui/src/player_view.cc
```
(Commit via `git_wrapper commit`.)

---

### Task 7: Track-panel quality "aura" (homogeneous) / per-row border (mixed)

**Files:**
- Modify: `gui/src/player_view.cc` (album view / track panel draw block)

**Interfaces:**
- Consumes: `qualityColorFor` (Task 4), `Track::sampleRate`.

- [ ] **Step 1: Compute homogeneity before the track-row loop**

In `player_view.cc`, find (currently lines 1119-1122):
```cpp
            // Duration column width measured once (widest realistic stamp),
            // so titles reserve real space instead of a guessed constant.
            float durColW = canvas.textWidthStyled("88:88", textSizes_.secondary, FontStyle::Math);
            for (int i = 0; i < (int)album.tracks.size(); i++) {
```
Replace with:
```cpp
            // Duration column width measured once (widest realistic stamp),
            // so titles reserve real space instead of a guessed constant.
            float durColW = canvas.textWidthStyled("88:88", textSizes_.secondary, FontStyle::Math);

            // Quality-color "aura": if every track shares the same tier, the
            // whole list gets one border below; otherwise each row gets its
            // own (drawn per-row in the loop). Recomputed live each time the
            // album view opens — cheap (O(track count), already in RAM),
            // exactly like the Android reference does (it doesn't cache
            // this check either — only the per-album inputs are cached).
            bool qualityMixed = false;
            QualityColor unifiedQuality{};
            bool unifiedSet = false;
            for (auto& t : album.tracks) {
                QualityColor tc = qualityColorFor(t.sampleRate, false);
                if (!unifiedSet) { unifiedQuality = tc; unifiedSet = true; }
                else if (tc.hasColor != unifiedQuality.hasColor || tc.color != unifiedQuality.color) {
                    qualityMixed = true;
                    break;
                }
            }

            for (int i = 0; i < (int)album.tracks.size(); i++) {
```

- [ ] **Step 2: Draw the per-row border when mixed**

Find (currently lines 1136-1138):
```cpp
                } else if (hoverTrackIdx_ == i) {
                    canvas.rect(rpx, rowY, rpw, (float)trackRowHeight_, toColor(CLR_HOVER), UI_CORNER_RADIUS);
                }
```
Replace with:
```cpp
                } else if (hoverTrackIdx_ == i) {
                    canvas.rect(rpx, rowY, rpw, (float)trackRowHeight_, toColor(CLR_HOVER), UI_CORNER_RADIUS);
                }

                if (qualityMixed) {
                    QualityColor tc = qualityColorFor(album.tracks[i].sampleRate, false);
                    if (tc.hasColor) {
                        float bw = 1.5f * uiScale_;
                        canvas.rect(rpx, rowY, rpw, bw, toColor(tc.color));
                        canvas.rect(rpx, rowY + trackRowHeight_ - bw, rpw, bw, toColor(tc.color));
                        canvas.rect(rpx, rowY, bw, (float)trackRowHeight_, toColor(tc.color));
                        canvas.rect(rpx + rpw - bw, rowY, bw, (float)trackRowHeight_, toColor(tc.color));
                    }
                }
```

- [ ] **Step 3: Draw the whole-list aura border when homogeneous**

Find (currently line 1171, right after the track-row `for` loop closes):
```cpp
            float tracksBottom = y + (float)album.tracks.size() * trackRowHeight_;
```
Replace with:
```cpp
            float tracksBottom = y + (float)album.tracks.size() * trackRowHeight_;

            if (!qualityMixed && unifiedQuality.hasColor) {
                float lb = 2.0f * uiScale_;
                float lx = (float)trackListLeft_, rx = (float)trackListRight_;
                canvas.rect(lx - lb, y - lb, (rx - lx) + lb * 2, lb, toColor(unifiedQuality.color));
                canvas.rect(lx - lb, tracksBottom, (rx - lx) + lb * 2, lb, toColor(unifiedQuality.color));
                canvas.rect(lx - lb, y - lb, lb, (tracksBottom - y) + lb * 2, toColor(unifiedQuality.color));
                canvas.rect(rx, y - lb, lb, (tracksBottom - y) + lb * 2, toColor(unifiedQuality.color));
            }
```

- [ ] **Step 4: Rebuild and confirm it compiles**

```bash
cmake --build /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native -- matrix_player
```
Expected: builds cleanly.

- [ ] **Step 5: Commit**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git add gui/src/player_view.cc
```
(Commit via `git_wrapper commit`.)

---

### Task 8: Full manual verification against the real library

**Files:** none (verification only)

- [ ] **Step 1: Full rebuild**

```bash
cmake --build /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native -- matrix_player
```
Expected: clean build.

- [ ] **Step 2: Cross-check classification against known albums**

```bash
sqlite3 /home/nava/Documents/Usick/.streamer/library.db \
  "SELECT id,title,tracks_count,release_type FROM albums WHERE id IN ('yx5nv8hwqxkoc','yley5zl05nu8b','lpgj56m5yxrzb','i846oxcvfim0a');"
```
Expected rows and how they should classify under this app's own (track-count-based) logic:
- `yx5nv8hwqxkoc` "Claire", 3 tracks → **EP**
- `yley5zl05nu8b` "Sentient EP", 4 tracks → **EP** (also independently tagged `epmini` by the source, a useful cross-check)
- `lpgj56m5yxrzb` "Player Of Games", 1 track → **Single**
- `i846oxcvfim0a`, 34 tracks → **Album**

- [ ] **Step 3: Run the app and verify the sidebar**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player/build/linux_native/gui
timeout 15 ./matrix_player
```
While it's running (or use a longer/no timeout for interactive use), confirm visually:
- Sidebar shows **Albums / EPs / Singles / Remixes**, each filtering the grid correctly (cross-check counts against Step 2's known albums).
- A gear icon sits below a hairline separator at the bottom of the sidebar, visually separated from the four type items.
- Clicking the gear opens Settings exactly as before; leaving Settings returns to whichever type filter was active.
- Grid tiles show a thin colored border matching each album's dominant sample rate (yellow for 44.1/48kHz FLACs, cyan for 88.2/96kHz+, etc.) — check `avgSampleRate` is being populated (a `0` for any album disables the border, so an all-transparent grid post-migration would indicate the migration/save/load path has a bug worth investigating before treating this as done).
- Opening an album with tracks of a single quality tier shows one border around the whole track list; if you can identify or construct a mixed-quality album, confirm each row instead gets its own border.

- [ ] **Step 4: Confirm `.streamer/library.db` is still untouched**

```bash
md5sum /home/nava/Documents/Usick/.streamer/library.db
```
Run before and after the session in Step 3; the hash must be identical.

- [ ] **Step 5: Final commit (if any stray changes remain uncommitted)**

```bash
cd /home/nava/Documents/Files/code/done/Matrix_Player
git status --short
```
If everything from Tasks 1-7 was already committed per-task, this should show nothing to commit — this step is a safety net, not expected to do new work.
