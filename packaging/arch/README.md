# Arch packages

Four microarchitecture variants, built in one pass, meant to be **uploaded**.
Each is self-contained: a person downloads exactly one file and installs it.

```bash
scripts/linux/build.sh --packages     # -> dist/linux/*.pkg.tar.zst
```

or directly:

```bash
cd packaging/arch
makepkg                               # all four
makepkg --pkg matrix-player-v3        # just one
```

Builds the last **pushed** commit on `main`, not your working tree. Commit and
push first with `./git_wrapper save` — it pushes submodules before the parent,
and a parent commit pointing at unpushed submodule commits fails in
`prepare()`.

## For whoever downloads one

```bash
sudo pacman -U matrix-player-v3-0.1.0.rNN.gXXXXXXX-1-x86_64.pkg.tar.zst
```

Which one to pick:

| Package | Needs | Who |
|---|---|---|
| `matrix-player-universal` | nothing | **any** x86-64 CPU — the safe choice |
| `matrix-player-v3` | AVX2, BMI2 | Intel Haswell (2013+), AMD Zen (2017+) |
| `matrix-player-v4` | AVX-512 | Intel Skylake-X / Ice Lake+, AMD Zen 4+ |
| `matrix-player-zen4` | Zen 4 tuning | AMD Ryzen 7000 / EPYC Genoa |

They can check what their CPU supports without installing anything:

```bash
/lib/ld-linux-x86-64.so.2 --help | grep -A4 'Subdirectories of glibc-hwcaps'
```

Levels marked `(supported, searched)` will run. If in doubt, `universal`.

All four `provide` and `conflict` with `matrix-player`, so pacman refuses to
hold two at once and swapping variants is a single `pacman -U`.

## Why self-contained rather than a shared data package

Splitting the 124 MB of fonts into a `matrix-player-data` package would cut the
upload from ~520 MB to ~148 MB. It is deliberately **not** done: `pacman -U` on
a single downloaded file would then fail with an unsatisfiable dependency
unless the person also found and downloaded the data package. "Download one
file, it works" is worth more than the bytes.

## What gets installed

```
/opt/matrix_player/            root:root, read-only
  matrix_player  fonts/  assets/shaders/  eq_profiles.json
/usr/bin/matrix_player         -> symlink into the above
/usr/lib/udev/rules.d/70-matrix-player-usb.rules
/usr/share/applications/matrix-player.desktop
/usr/share/icons/hicolor/scalable/apps/matrix-player.svg
/usr/share/licenses/matrix-player-<variant>/

~/.matrix_player/              theirs; survives `pacman -R`
  matrix_player.db  matrix_player.log  ui-atlas.<fingerprint>.msdf.cache
```

**`/usr/bin/matrix_player` is a symlink, not a wrapper script.**
`readlink("/proc/self/exe")` resolves symlinks, so `app_paths::exeDir()` still
reports `/opt/matrix_player/`. Nothing else is needed.

**The read-only/writable split is a source feature, not a packaging hack.**
`-DMATRIX_STATE_HOME=.matrix_player` (see `gui/src/app_paths.hh`) sends the DB,
log and atlas cache to `$HOME`. Leave it unset — as every ordinary build does —
and all three stay beside the binary. `build/linux/` and the `--share` tarballs
are unaffected. Not XDG: a plain dotdir, depending on no specification.

**The atlas cache has to be per-user.** `bakeFallbackGlyphs()` bakes whichever
CJK/Hangul/Kana codepoints the listener's own library contains and re-bakes on
rescan. ~45 MB, growing with their music — never shareable between users, which
is why no permission arrangement in `/opt` would have been right.

**There are no `install()` rules in the tree**, so `package()` does all the work
by hand. Do not switch to `cmake --install`: soxr and freetype register their
own install rules into the top-level install set, so it would install *their*
headers and libraries and not `matrix_player`.

## Build flags

| Flag | Why |
|---|---|
| `-DMATRIX_STATE_HOME=.matrix_player` | The whole reason a read-only `/opt` install works. |
| `-DMATRIX_ARCH_LEVEL=` *(per variant)* | Empty for universal; `v3`/`v4`/`znver4` otherwise. |
| `-DVCE_SLANGC=...` | `slangc` is required at build time; no `.spv` is committed, and `VceShaders.cmake` otherwise falls back to a hardcoded Windows path. AUR has it as `shader-slang-bin` (`/opt/shader-slang-bin/bin/slangc`, not on `PATH`). |
| `-DAUDIO_ENGINE_BUILD_TOOLS=OFF` | Defaults to ON and would build eight smoke-test executables nothing packages. |

**`-march=native` is deliberately never used.** It bakes in whatever the build
machine supports and SIGILLs on anything older — fine for a local install,
never for an upload.

`-ffp-contract=off` is **not** passed here — the root `CMakeLists.txt` applies
it to every build. It is load-bearing for exactly these variants: at v3 and
above FMA exists, GCC's default contraction fuses the EQ's biquad accumulator,
and `dsp_null_test` then fails exact-equality against the frozen oracle.
Measured: `-march=native` without it fails test [2] at `dsp_null_test.cpp:481`.

## check()

Runs `dsp_null_test` once **per variant**, each in its own minimal Debug tree.
Cheap despite appearances — only that one target is built and `ae_core` is an
INTERFACE target, so it is a configure plus two compile steps, not a fifth
full build.

A variant this machine cannot execute dies with SIGILL and is reported
`SKIPPED`, never folded into "passed". On an i7-11800H all four actually run
and pass, zen4 included — it and Tiger Lake share AVX-512 and GCC emits nothing
Zen-only here. Build on a pre-AVX-512 CPU and v4/zen4 become build-only.

## Cost

Four full Release builds with LTO, roughly 30 minutes. There is no sharing to
be had: changing `-march` invalidates everything, including the in-tree libFLAC,
libmpg123, libusb, freetype, msdfgen and libjpeg-turbo. The split package does
at least share one clone, one submodule fetch and one mpg123 extraction across
all four.

## Known deviations from Arch policy

- **`prepare()` touches the network** (`git submodule update --init
  --recursive`). Ten submodules three levels deep; the full `git config
  submodule.*.url` dance is not worth writing for a release build run by hand.
  This is what makes it unsuitable for the AUR as written.
- **`makepkg` clones this repo into `packaging/arch/src/`.** Gitignored, but a
  copy of the tree does live inside the tree while building. Move this
  directory out of the repo if that bothers you — nothing depends on its
  location.

## mpg123

`libmpg123` is not in git. `initialize_files.py` normally downloads it, which
cannot happen inside `build()`, so the PKGBUILD lists the release tarball in
`source=()` and symlinks it into place. If the pin in `initialize_files.py`
changes, update `_mpg123ver` and the checksum here to match.

Silent-failure warning: if that symlink is ever wrong, `core/CMakeLists.txt`
only *warns* about the missing `ae_mp3` target and every uploaded package would
quietly play everything except MP3. `prepare()` therefore checks for
`src/libmpg123/libmpg123.c` and aborts rather than let it pass.
