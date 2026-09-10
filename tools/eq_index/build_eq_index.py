#!/usr/bin/env python3
"""Build eq_profiles.json from an AutoEq results/ tree.

Run:  tools/eq_index/build_eq_index.py <path-to-AutoEq> [-o eq_profiles.json]

WHY THIS LIVES HERE
-------------------
eq_profiles.json is a 4.8 MB shipped asset and the script that made it lived in
a different project entirely, so nothing in this repository could say where the
catalogue came from or reproduce it. It can now.

WHAT THE PREVIOUS GENERATOR GOT WRONG, AND THIS DOES NOT
--------------------------------------------------------
1. It kept every measurement of every model with no notion that some are better
   than others, so the catalogue shipped 8666 rows for 6033 models -- 30% of it
   duplicates that the UI could not tell apart, because the row label shows the
   name and the form and never the source.

   AutoEq itself ranks them. dbtools/update_result_indexes.py holds a 50-entry
   ordered list, `ResultPath.priorities`, annotated with each source's measured
   unit-to-unit standard deviation, and oratory1990 is first in all three form
   factors. Its website emits its JSON in that order and its dropdown keeps the
   first of each repeated label -- that, and nothing cleverer, is why searching
   a headphone there gives you one result and not four.

   We read that list OUT OF the AutoEq checkout rather than copying it, so it
   cannot go stale behind us, and stamp each profile with its rank. Nothing is
   dropped: the rank lets the app show the best by default and all of them on
   request, which is exactly what the website does.

2. Its detect_form() looked for a path component that was exactly "over-ear",
   "in-ear" or "earbud". AutoEq puts the measurement RIG in that same component
   for sources that have more than one -- "711 in-ear", "HMS II.3 over-ear",
   "GRAS 43AG-7 over-ear" -- so none of those matched and 2827 profiles (33% of
   the catalogue) shipped with no form at all. Worse, two rigs of one form then
   collapsed onto the same (name, source, form) key and whichever sorted first
   won, which replaced AutoEq's accuracy ranking with alphabetical order.

   Splitting rig from form the way AutoEq's own ResultPath does fixes both.

NOTE FOR WHOEVER REGENERATES THIS: the (name, source, form) triple is a
PERSISTED key -- it is what the eq_assignments and eq_headphones tables store.
Restoring the missing forms changes that triple for every crinacle, Rtings and
HypetheSonics row, which is why EqProfileStore::findByKey grew a (name, source)
fallback. Do not remove it.
"""

import argparse
import json
import re
import sys
from pathlib import Path

FORMS = ("over-ear", "in-ear", "earbud")

FILTER_RE = re.compile(
    r"Filter\s+\d+:\s+ON\s+(PK|LSC|HSC)\s+Fc\s+([\d.]+)\s+Hz\s+"
    r"Gain\s+([-\d.]+)\s+dB\s+Q\s+([\d.]+)")
PREAMP_RE = re.compile(r"Preamp:\s+([-\d.]+)\s+dB")

# Anything the priority list does not name still ships; it just sorts last.
# A rank rather than a drop, because a source AutoEq has not ranked is not a
# source we know to be bad.
UNRANKED = 9999


def read_priorities(autoeq_root: Path):
    """AutoEq's own accuracy order, read from its source rather than copied.

    The literal in update_result_indexes.py is written worst-first and reversed
    on the spot with [::-1], so index 0 in the live attribute is the BEST. We
    reproduce that reversal here; getting it backwards would recommend the
    least reliable measurement of every headphone.
    """
    src = (autoeq_root / "dbtools" / "update_result_indexes.py").read_text(encoding="utf-8")
    m = re.search(r"priorities\s*=\s*\[(.*?)\]\[::-1\]", src, re.S)
    if not m:
        sys.exit("could not find ResultPath.priorities — has AutoEq's layout changed?")
    pairs = re.findall(r"\(\s*'((?:[^'\\]|\\.)*)'\s*,\s*'((?:[^'\\]|\\.)*)'\s*\)", m.group(1))
    pairs = [(a.replace("\\'", "'"), b.replace("\\'", "'")) for a, b in pairs][::-1]
    return {pair: i for i, pair in enumerate(pairs)}


def split_form_rig(form_rig: str):
    """"GRAS 43AG-7 over-ear" -> ("over-ear", "GRAS 43AG-7").

    The same subtraction ResultPath.__init__ does. A directory that is just a
    form has no rig, which is the common case.
    """
    for form in FORMS:
        if form_rig == form:
            return form, ""
        if form_rig.endswith(form):
            return form, form_rig[: -len(form)].strip()
    return "", form_rig.strip()


def parse_eq(path: Path):
    preamp, filters = 0.0, []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        p = PREAMP_RE.match(line)
        if p:
            preamp = float(p.group(1))
            continue
        f = FILTER_RE.match(line)
        if f:
            filters.append({"type": f.group(1), "fc": float(f.group(2)),
                            "gain": float(f.group(3)), "q": float(f.group(4))})
    return preamp, filters


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("autoeq", type=Path, help="path to an AutoEq checkout")
    ap.add_argument("-o", "--out", type=Path, default=Path("eq_profiles.json"))
    args = ap.parse_args()

    results = args.autoeq / "results"
    if not results.is_dir():
        sys.exit(f"no results/ under {args.autoeq}")

    priorities = read_priorities(args.autoeq)
    print(f"priority list: {len(priorities)} entries, best = "
          f"{min(priorities, key=priorities.get)}")

    profiles, seen, unranked = [], {}, set()
    for eq_file in sorted(results.rglob("*ParametricEQ.txt")):
        parts = eq_file.relative_to(results).parts
        if len(parts) < 3:
            continue
        source, form_rig, name = parts[0], parts[1], eq_file.parent.name
        form, rig = split_form_rig(form_rig)
        rank = priorities.get((source, form_rig), UNRANKED)
        if rank == UNRANKED:
            unranked.add((source, form_rig))

        # One row per (name, source, form, rig). Unlike the old key this keeps
        # the rig, so a source measuring one headphone on two rigs no longer
        # loses one of them to an arbitrary tie-break.
        key = (name, source, form, rig)
        if key in seen:
            continue
        preamp, filters = parse_eq(eq_file)
        if not filters:
            continue
        seen[key] = True
        profiles.append({"name": name, "source": source, "form": form, "rig": rig,
                         "rank": rank, "preamp": preamp, "filters": filters})

    # By name, then by rank: the app's "recommended" view is the first row of
    # each name, so the ordering IS the recommendation and no consumer has to
    # sort 8000 entries at load time. Ties settle on source for determinism --
    # a generator that emits a different file from the same input is a
    # generator nobody can check.
    profiles.sort(key=lambda p: (p["name"].lower(), p["rank"], p["source"]))

    args.out.write_text(json.dumps(profiles, separators=(",", ":")), encoding="utf-8")

    names = {p["name"] for p in profiles}
    print(f"{len(profiles)} profiles, {len(names)} models -> {args.out} "
          f"({args.out.stat().st_size / 1e6:.1f} MB)")
    print(f"  no form: {sum(1 for p in profiles if not p['form'])}")
    print(f"  unranked source/rig pairs: {len(unranked)}")
    for u in sorted(unranked):
        print(f"    {u[0]} | {u[1]}")


if __name__ == "__main__":
    main()
