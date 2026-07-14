# git_wrapper — Manual

A small command-line tool that wraps `git commit` and `git push` so two mistakes
can't happen again:

- a commit ever showing a contributor other than **nava <nava@noreply.com>**
  (no `Co-Authored-By:` trailers, no stray author/committer identity), and
- pushing a repository with submodules in the wrong order (parent before its
  submodules), which breaks fresh clones.

You drive it with three verbs: `commit`, `push`, `save`.

---

## 1. Build

From the tool's folder:

```bat
cd C:\Users\incxiuefb\Documents\Files\clone\git_wrapper
Build.bat
```

`Build.bat` runs `build_msvc.ps1` (vswhere → vcvars64 → Ninja → cl) and produces:

```
git_wrapper\build\git_wrapper.exe
```

Requirements: Visual Studio Build Tools with the **Desktop development with C++**
workload, Ninja at `C:\PPProgam\ninja_win\bin\ninja.exe`, and `git` on `PATH`.

---

## 2. Install (put it on PATH)

So you can type `git_wrapper` from any repository, add its build folder to PATH
**once**:

- Press Start, type "Edit the system environment variables" → *Environment
  Variables…*
- Under *User variables* select **Path** → *Edit* → *New*, paste:
  ```
  C:\Users\incxiuefb\Documents\Files\clone\git_wrapper\build
  ```
- OK out of all dialogs, then open a **new** terminal.

Verify:

```bat
git_wrapper help
```

(Alternatively, just copy `build\git_wrapper.exe` into a folder that is already
on PATH.)

---

## 3. Commands

Run these from **inside** the repository you want to act on (any subfolder is
fine).

### `commit` — stage everything and commit as nava

```bat
git_wrapper commit "Short summary of the change"
```

- Runs `git add -A` first, then commits.
- Author **and** committer are forced to `nava <nava@noreply.com>`, whatever the
  repo/global git config says.
- The message is cleaned: any `Co-Authored-By:` line, "Generated with … Claude"
  line, or 🤖 line is removed. The rest of your text is kept verbatim.
- If there is nothing to commit, it says so and exits 0 (no error).

### `push` — submodules first, then the parent

```bat
git_wrapper push
git_wrapper push --force
```

- Pushes each **submodule that has unpushed commits** to its `origin` first,
  then the parent repo — the safe order.
- Skips submodules that are pinned/detached (third-party dependencies) or already
  up to date.
- Uses `git push -u origin <current-branch>`, so the first push also sets the
  upstream.
- `--force` (or `-f`) uses `--force-with-lease` — the safe force that refuses to
  overwrite remote commits you haven't fetched. You need this after rewriting
  history (see §5).
- If a submodule push fails, it stops **before** touching the parent.
- It refuses to push if an un-pushed commit still carries a `Co-Authored-By`
  trailer (e.g. one made with plain `git`), telling you to fix it first.

### `save` — commit then push in one step

```bat
git_wrapper save "Short summary of the change"
git_wrapper save "Summary" --force
```

Equivalent to `commit` followed by `push`. If the commit step fails, it does not
push.

### `help`

```bat
git_wrapper help
```

---

## 4. Everyday workflow

```bat
:: hack on code ...
git_wrapper save "Add Windows resize handling"
```

That stages everything, commits as nava (clean message), and pushes — submodules
first if any changed. That's the whole loop.

If you'd rather review before pushing:

```bat
git_wrapper commit "Add Windows resize handling"
git status          :: or `git log -1`, `git show`
git_wrapper push
```

---

## 5. Pushing after a history rewrite

If history was rewritten (as it was to remove co-authors), the remote still has
the old commits, so a normal push is rejected. Use force — the wrapper still
pushes the submodule first:

```bat
git_wrapper push --force
```

This runs `--force-with-lease` in the submodule, then the parent. Push order and
safety are handled for you; you don't push the two repos by hand.

> Local backup branches (e.g. `backup-before-trailer-strip`) are **not** pushed —
> `push` only sends the current branch to `origin`. They stay local until you
> delete them.

---

## 6. Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (including "nothing to commit"). |
| 1 | A git operation failed, or a repo is in detached HEAD and can't be pushed. |
| 2 | Usage error (no command, unknown command, or missing commit message). |
| 3 | Push aborted: an un-pushed commit carries a `Co-Authored-By` trailer. |

---

## 7. Troubleshooting

- **"detached HEAD — check out a branch to push"** — the repo (or a submodule
  you own) isn't on a branch. Run `git switch main` (or the right branch) and
  retry. Third-party submodules are pinned/detached on purpose and are skipped
  automatically.
- **"has un-pushed commits with a Co-Authored-By trailer"** — a commit was made
  outside this wrapper and still credits someone else. Rewrite that commit (only
  nava may appear), then push again.
- **force-with-lease rejected** — the remote moved since you last fetched. Run
  `git fetch`, confirm you still want to overwrite, and retry.
- **submodule push fails with no `origin`** — give the submodule a remote
  (`git -C <path> remote add origin <url>`), or if it's a third-party dep you
  don't own, you shouldn't be pushing it (it should be pinned/detached).

---

## 8. Changing the identity

The forced name/email are the two constants at the top of `main.cc`:

```cpp
static const char* kName  = "nava";
static const char* kEmail = "nava@noreply.com";
```

Edit them, then rebuild with `Build.bat`.

---

## 9. Why a temp file for the message

Windows' program-spawn layer does not quote arguments, so a commit message with
spaces or characters like `&`, `%`, or quotes would be mangled if passed on the
command line. The wrapper writes the (sanitized) message to a temporary file and
uses `git commit -F`, so any message text is safe.
