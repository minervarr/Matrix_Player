# A storage framework, and a scan that stops re-reading the library

**2026-09-04**

## The problem, as stated

Android and Linux share a kernel and do not share a filesystem story. On the
desktop everything the user owns is under `$HOME` and an app writes where it
likes; on a phone the same code meets permissions, scoped storage, and a media
layout with nowhere to put anything structured. The ask was to unify that behind
one library — `archive_engine` — so future apps get it by linking rather than by
re-deriving it, and so Matrix_Player's phone build feels like its desktop build.

Two things turned out to be true that the ask did not assume.

**`archive_engine` had no storage code.** It is four modules of downloader glue
— libcurl, TagLib, libarchive, plus md5/base64/`Result`. `sanitize_filename()`
was its entire filesystem surface, and that is a string function. So this was
new code inside a proven shape, not a refactor of something existing.

**The slowest thing about scanning was not an Android API.** See below.

## Why Android storage is slow, precisely

Not SAF. Neither this app nor `streamer` uses it, and both reject it in writing
in three places apiece — a `content://` URI is not a path, and every one of
these programs walks a tree with `std::filesystem`. Both already hold
`MANAGE_EXTERNAL_STORAGE` and use real POSIX paths.

The cost is that since Android 11, `/storage/emulated/0` is a **FUSE** mount.
Every `openat`, `stat` and `readdir` is a round trip through a userspace daemon
— [up to ~83% overhead even for a pure passthrough
FUSE](https://www.usenix.org/system/files/atc19-bijlani.pdf). [FUSE passthrough
(Android 12+)](https://source.android.com/docs/core/storage/fuse-passthrough)
and the later fuse-bpf work fix *read and write on an already-open descriptor*;
they do nothing for the metadata operations a library scan is actually made of.

So the win is not "open files faster". It is **stop opening files**.

## What was actually wrong

`gui/src/player_view.cc` ran `scanLibraryIncremental()` — which stats every file
and reports how many were unchanged — and then ran `scanLibraryParallel()`,
which walks the tree again and re-parses every file, and threw the incremental
result away.

That was not the redundancy it looks like. `scanLibraryIncremental()` returned
only the albums it had **re-parsed**, which is a partial view of the library and
unusable on its own, so the second scan was load-bearing. The counter it printed
was the lie: on a 2318-file library with nothing changed, it reported *2318
skipped* and parsed 2318 files.

The fix is that an unchanged file now contributes the `Track` the database
already holds. Metadata in a file nobody touched cannot have changed either.

## The design

### One vocabulary: `arc_fs`

```
arc/fs/paths.hh        Layout, and pure path arithmetic
arc/fs/walk.hh         enumeration that cannot throw
arc/fs/volume.hh       Direct / Fuse / None
arc/fs/media_index.hh  the seam for the platform's own catalogue
```

**`Layout` splits user data from machine data**, and that split is the whole
point:

| | Linux | Windows | Android |
|---|---|---|---|
| `home` | `$HOME` | `%USERPROFILE%` | `/storage/emulated/0/home` |
| `config` | `$HOME/.config/<app>` | `%APPDATA%\<app>` | `home/.config/<app>` |
| `data` | `$HOME/.local/share/<app>` | `%LOCALAPPDATA%\<app>` | `home/.local/share/<app>` |
| `state`, `cache` | XDG | under `data` | **the app's private dir** |

`state`/`cache` are deliberately *not* under `home` on Android. A SQLite
database is the worst thing to put behind FUSE and the thing an app is most
tempted to put there. Both apps already landed in the private directory by
accident; this makes it a stated rule enforced by the type.

`home` on Android is a real directory the user can see, because Android's shared
root is a fixed set of media buckets with nowhere for an app to keep anything
structured. Inside it, the same `.config/` and `.local/share/` the desktop
already uses — so a path-building expression stops needing to know the platform.

**`walk()` cannot throw.** That is why it exists rather than each caller reaching
for `std::filesystem`. It uses an explicit stack, not
`recursive_directory_iterator`, because that iterator's `error_code` overload
gives no portable way to step past the entry that failed — one bad directory
either aborts or truncates the rest of the walk.

**`MediaIndex` is an installed pointer, not an `#ifdef`.** `core/` does no JNI,
so the MediaStore implementation lives in `platform/android/` and installs
itself at startup. That choice bought the verification below.

### The scan, after

```
                 ┌─ MediaIndex::available()? ──┐
                 │                             │
             yes │                             │ no  (every desktop)
                 ▼                             ▼
        one query for the root            arc::fs::walk()
        (path + tags, no opens)           (readdir + one stat per entry)
                 │                             │
                 └──────────┬──────────────────┘
                            ▼
              cached and (size, mtime) match?
                 ┌──────────┴──────────┐
             yes │                     │ no
                 ▼                     ▼
        reuse the stored Track    open the file
        (0 syscalls)              (sampleRate / bitDepth —
                                   which no index reports)
```

Three ways the index declines, all landing on the walk: no backend, a refused
query (no storage grant yet), an empty result (a `.nomedia` folder, or files
copied over MTP moments ago). The walk is the floor; an unindexed folder is
slower, never invisible.

## Measurements

2318-file library, unchanged since the previous launch, this desktop:

| | files parsed | cold scan | warm scan |
|---|---|---|---|
| before | **2318** (reporting "2318 skipped") | 265 ms | ~195 ms |
| after | **0** | 188 ms | ~190 ms |

The warm-scan wall clock **did not improve here**, and that is worth stating
plainly rather than dressing up: the fixtures are small WAVs on a warm page
cache, where the re-parse was nearly free and the remaining cost is the walk
plus one stat per file. The saving is in file *opens*, which is what costs over
FUSE, and on FLAC, whose metadata parse is heavier than a WAV header.

The cold-scan improvement (265 → 188 ms) is real and comes from removing a
duplicate `stat` per file: the walk already stat'ed every entry, and the parsers
stat'ed each file again to fill `fileSize`/`fileMtime`.

**None of the Android numbers exist yet.** No device has run this.

## Verification

`core/tests/scan_source_test.cc` is the check that was going to need a phone.
Because the backend is an installed pointer, a fake one runs on the desktop and
the Android path is exercised for real, over real WAV files:

- both sources produce byte-identical album lists — including sample rate and
  bit depth, which the index never reported and which can therefore only come
  from the files being opened;
- all three fallbacks return the whole library, not a partial one;
- the cache hits on the index path (6 scanned, then 6 skipped);
- a `cover.jpg` the index lists does not become a track.

Alongside: ten desktop tests, `arc_fs`'s own three, and 20 `ui_capture` PNGs
byte-identical to a pre-change baseline at every step.

## The mtime subtlety

`Track::fileMtime` is an opaque change token, compared only against a value the
same device wrote earlier. The walk fills it with `std::filesystem` tick counts;
the index fills it with unix seconds. They are **not** interchangeable.

That is safe because a device uses one source or the other. The one case where
they meet — a device that had the index and loses it, say a revoked grant —
mismatches every token and re-parses the library once, after which it is stable.
One slow scan, then correct, which is the right trade.

The related trap, now closed: the parsers used to `stat` each file a second time
to fill those fields. If the walk's answer and the parser's answer had ever
disagreed, the cache would never hit and the whole library would re-parse every
launch — looking exactly like the cache simply not working.

## Bugs found on the way

- `Db::removeMusicRoot()` built its `LIKE` prefix with a hardcoded `'\'`. Paths
  are stored as `std::filesystem` writes them, so on Linux and Android the
  pattern matched nothing: removing a root deleted its `music_roots` row and
  **orphaned every one of its track rows**. Its pattern is now escaped too —
  `_` is a `LIKE` wildcard and is ordinary in a directory name, so removing
  `/music_a` would also have deleted everything under `/musicXa`.
- `forEachAudioFile()` used the throwing iterator inside a detached thread.
- `userHomeDir()` read `$HOME`, which on Android is `/` — so the in-app folder
  picker opened at the filesystem root.
- `archive_engine`'s standalone desktop build could not work on this machine:
  an unconditional empty `-DCMAKE_TOOLCHAIN_FILE=`, and libarchive 3.7.4's
  `cmake_minimum_required(2.8.12)` versus CMake 4.

## Not done

- **Nothing has run on a device.** The MediaStore backend compiles for three
  ABIs and its logic is proven against a fake; that is all.
- **Progressive display.** The design allows the grid to appear from index data
  before formats are known, with quality badges filling in behind. Today the
  scan still completes before the grid updates.
- **`generation()` is unused.** `MediaStore.getGeneration()` is implemented and
  would let an unchanged library skip the query entirely; nothing calls it yet.
- **`streamer` is unmigrated.** Its seam is already the right one —
  `config::set_platform_dirs(config_dir, download_dir)`, called once from
  `gui/src/os/android_main.cc` — so the change is two lines there, supplying
  `arc::fs::layout("streamer").state` and `home/Downloads/streamer`. Its own
  `scan()` (`src/library.cpp`) is a single-threaded
  `recursive_directory_iterator` inside one transaction, and is the natural
  second consumer of `arc::fs::walk()`.
