# Arch package

```bash
cd packaging/arch
makepkg -si
```

Builds the latest pushed `main` — **not** your working tree. Commit and push
first (`./git_wrapper save`, which pushes submodules before the parent; a
parent commit pointing at unpushed submodule commits fails in `prepare()`).

## What gets installed

```
/opt/matrix_player/            root:root, read-only
  matrix_player  fonts/  assets/shaders/  eq_profiles.json
/usr/bin/matrix_player         -> symlink into the above
/usr/lib/udev/rules.d/70-matrix-player-usb.rules
/usr/share/applications/matrix-player.desktop
/usr/share/icons/hicolor/scalable/apps/matrix-player.svg
/usr/share/licenses/matrix-player-git/

~/.matrix_player/              yours; survives `pacman -R`
  matrix_player.db  matrix_player.log  ui-atlas.<fingerprint>.msdf.cache
```

## Why it is shaped this way

**`/usr/bin/matrix_player` is a symlink, not a wrapper script.**
`readlink("/proc/self/exe")` resolves symlinks, so `app_paths::exeDir()` still
reports `/opt/matrix_player/` however the binary was invoked. Nothing else is
needed.

**The split between read-only and writable is a source feature, not a
packaging hack.** `-DMATRIX_STATE_HOME=.matrix_player` (see
`gui/src/app_paths.hh`) moves the DB, log and atlas cache to `$HOME`. Leave it
unset — as every ordinary build does — and all three stay beside the binary,
exactly as they always have. `build/linux/` and the `dist/linux/` tarballs are
unaffected.

Not XDG, deliberately: a plain dotdir in the `~/.ssh` tradition, depending on
no specification.

**The atlas cache has to be per-user.** `bakeFallbackGlyphs()` bakes whichever
CJK/Hangul/Kana codepoints the listener's own library contains, and re-bakes on
rescan. It is ~45 MB and it grows with the music. It was never shareable
between users, which is why no amount of permission juggling in `/opt` would
have been right.

**There are no `install()` rules in the tree**, so `package()` does all the
work by hand. Do not switch to `cmake --install`: soxr and freetype register
their own install rules into the top-level install set, so it would install
*their* headers and libraries and not `matrix_player`.

**Fonts come from the source tree, shaders from the build tree** — not from
`build/gui/`, where POST_BUILD leaves copies of both. A stale `*.msdf.cache`
sitting in `build/gui/fonts/` from an earlier run would otherwise be packaged,
adding ~45 MB of someone else's glyph coverage.

## Build flags

| Flag | Why |
|---|---|
| `-DMATRIX_ARCH_LEVEL=native` | Tuned for the build machine. **Not portable** — this package will SIGILL on an older CPU. Drop it for a portable package. |
| `-DMATRIX_STATE_HOME=.matrix_player` | The whole reason a read-only `/opt` install works. |
| `-DVCE_SLANGC=...` | `slangc` is required at build time; no `.spv` is committed, and `VceShaders.cmake` otherwise falls back to a hardcoded Windows path. Arch has it in AUR as `shader-slang-bin` (`/opt/shader-slang-bin/bin/slangc`, not on `PATH`). |
| `-DAUDIO_ENGINE_BUILD_TOOLS=OFF` | Defaults to ON and would build eight smoke-test executables nothing packages. |

`-ffp-contract=off` is **not** passed here — the root `CMakeLists.txt` applies
it to every build. That is load-bearing for `MATRIX_ARCH_LEVEL`: at v3 and
above FMA appears, GCC's default `-ffp-contract=fast` fuses the EQ's biquad
accumulator, and `dsp_null_test` fails on exact-equality against the frozen
oracle. `check()` runs that test with the same flags `build()` uses.

## Known deviations from Arch policy

- **`prepare()` touches the network** (`git submodule update --init
  --recursive`). Ten submodules three levels deep; the full `git config
  submodule.*.url` dance is not worth writing for a package built only
  locally. This is what makes it not AUR-ready as written.
- **`-march=native`** makes the package machine-specific.
- **`makepkg` clones this repo into `packaging/arch/src/`.** Gitignored, but it
  does mean a copy of the tree lives inside the tree while building. Move this
  directory out of the repo if that bothers you — nothing here depends on its
  location.

## mpg123

`libmpg123` is not in git. `initialize_files.py` normally downloads it, which
cannot happen inside `build()`, so the PKGBUILD lists the release tarball in
`source=()` and symlinks it into place. If the pin in `initialize_files.py`
changes, update `_mpg123ver` and the checksum here to match.

Silent-failure warning: if that symlink is ever wrong,
`core/CMakeLists.txt` only *warns* about the missing `ae_mp3` target and you
get a package that plays everything except MP3. `prepare()` therefore checks
for `src/libmpg123/libmpg123.c` and aborts rather than let it pass.
