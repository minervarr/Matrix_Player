#!/usr/bin/env python3
"""Build matrix-icons.otf (and ui_icons.gen.h) from the SVGs in icons/.

Why a font: the app already runs an MTSDF text atlas every frame, so icons
packaged as glyphs ride that pipeline with no new GPU pass, no per-frame
Bezier evaluation, and draw-time tinting for free. Same approach Apple uses
for SF Symbols.

Why 4 em: atlas cell size is derived from the outline's own bounds
(msdf.cc, `w = ceil((bounds.r-bounds.l) * sizePxEm_/emSize + 2*range)`), so
authoring each glyph 4 em tall gets a 4x denser bake -- 384px of detail per
icon instead of 96px -- without touching the font engine's fixed EM.

Usage:
    pip install fonttools svgelements
    python3 build_icon_font.py

Outputs (both committed to the repo, so the C++ build never needs Python):
    ../../assets/fonts/icons/matrix-icons.otf
    ../../gui/src/ui_icons.gen.h
"""
import os
import sys

try:
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from svgelements import SVG, Path, Move, Line, Close, CubicBezier, QuadraticBezier, Arc
except ImportError as e:
    sys.exit(f"missing dependency: {e}\n  pip install fonttools svgelements")

HERE = os.path.dirname(os.path.abspath(__file__))
ICON_DIR = os.path.join(HERE, "icons")
OTF_OUT = os.path.join(HERE, "..", "..", "assets", "fonts", "icons", "matrix-icons.otf")
HDR_OUT = os.path.join(HERE, "..", "..", "gui", "src", "ui_icons.gen.h")

UPEM = 1000   # font units per em

# Icon name (icons/<name>.svg) -> C++ enumerator -> DESIGN BOX in ems.
#
# The box size is the resolution knob, and it is PER ICON on purpose. Atlas
# cell size derives from the outline's own bounds, so an N-em glyph is baked at
# N x 96px. Bigger is not better: a bake far denser than the size an icon is
# actually drawn at gets heavily MINIFIED, and a single bilinear tap can't
# represent that -- it smears. (msdf.cc:407 documents the same trap for text:
# EM=100 meant 6-9x minification and visibly "low quality" glyphs.)
#
# So match each icon to where it is really drawn (sizes from player_view.cc,
# authored at the 1080 reference and scaled up to 4x on an 8K display):
#
#   transport / essential buttons  71-284px (up to 960 essential @8K) -> 2 em
#   sidebar gear                   30-120px                           -> 1 em
#   warning strip                  37-148px                           -> 1 em
#
# Codepoints are Unicode Private Use Area, assigned by position. Append only;
# never renumber, or a stale MTSDF cache maps old codepoints to new artwork.
ICONS = [
    ("play",     "Play",     2),
    ("stop",     "Stop",     2),
    ("prev",     "Prev",     2),
    ("next",     "Next",     2),
    ("settings", "Settings", 1),
    ("warning",  "Warning",  1),
]
CP_BASE = 0xE000


def svg_to_contours(path_file, box_em):
    """SVG file -> list of contours, each a list of segments in FONT units.

    svgelements resolves the viewBox, real-world units (mm/in) and every
    nested Inkscape group transform into one px viewport, reported as
    svg.width/svg.height -- so normalising by THAT box works whatever the
    source document used. The box (not each icon's ink) is what maps onto
    the 4-em square, so icons keep their relative sizes exactly as the old
    36-unit grid gave them.
    """
    svg = SVG.parse(path_file)
    doc_w, doc_h = float(svg.width), float(svg.height)
    if doc_w <= 0 or doc_h <= 0:
        sys.exit(f"{path_file}: document has no usable width/height")
    box = UPEM * box_em
    # Fit the document box into the design square, preserving aspect, centred.
    k = box / max(doc_w, doc_h)
    off_x = (box - doc_w * k) * 0.5
    off_y = (box - doc_h * k) * 0.5

    def pt(p):
        # SVG is y-down, fonts are y-up: flip, and put the box bottom on the
        # baseline so every icon has one predictable origin (see ui_icons.cc).
        return (p.x * k + off_x, box - (p.y * k + off_y))

    contours = []
    for el in svg.elements():
        if not isinstance(el, Path) or len(el) == 0:
            continue
        for sub in el.as_subpaths():
            segs = []
            for seg in Path(sub):
                if isinstance(seg, Move):
                    segs.append(("move", pt(seg.end)))
                elif isinstance(seg, (Line, Close)):
                    if seg.start is not None and seg.end is not None:
                        segs.append(("line", pt(seg.end)))
                elif isinstance(seg, CubicBezier):
                    segs.append(("curve", pt(seg.control1), pt(seg.control2), pt(seg.end)))
                elif isinstance(seg, QuadraticBezier):
                    # CFF charstrings are cubic-only; exact degree elevation.
                    s, c, e = seg.start, seg.control, seg.end
                    c1 = type(s)(s.x + 2.0 / 3.0 * (c.x - s.x), s.y + 2.0 / 3.0 * (c.y - s.y))
                    c2 = type(e)(e.x + 2.0 / 3.0 * (c.x - e.x), e.y + 2.0 / 3.0 * (c.y - e.y))
                    segs.append(("curve", pt(c1), pt(c2), pt(seg.end)))
                elif isinstance(seg, Arc):
                    for c in seg.as_cubic_curves():
                        segs.append(("curve", pt(c.control1), pt(c.control2), pt(c.end)))
            if segs:
                contours.append(segs)
    return contours


def draw(contours, pen):
    for segs in contours:
        started = False
        for seg in segs:
            if seg[0] == "move":
                if started:
                    pen.closePath()
                pen.moveTo(seg[1])
                started = True
            elif not started:
                continue
            elif seg[0] == "line":
                pen.lineTo(seg[1])
            else:
                pen.curveTo(seg[1], seg[2], seg[3])
        if started:
            pen.closePath()


def bounds_of(contours):
    xs, ys = [], []
    for segs in contours:
        for seg in segs:
            for p in seg[1:]:
                xs.append(p[0])
                ys.append(p[1])
    return (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)


def check_overlap(name, contours):
    """Heuristic guard for the no-Skia constraint: msdfgen has no overlap
    resolver here, so contours must be disjoint or properly nested (holes).
    Bounding boxes that partially overlap are the signature of un-unioned art."""
    boxes = [bounds_of([c]) for c in contours]
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i], boxes[j]
            hit = not (a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1])
            nested = ((a[0] >= b[0] and a[1] >= b[1] and a[2] <= b[2] and a[3] <= b[3]) or
                      (b[0] >= a[0] and b[1] >= a[1] and b[2] <= a[2] and b[3] <= a[3]))
            if hit and not nested:
                print(f"  WARNING {name}: contours {i} and {j} partially overlap. "
                      f"This build has MSDFGEN_USE_SKIA=OFF (no overlap resolver) -- "
                      f"run Path > Union in Inkscape.")
                return


def main():
    glyph_order = [".notdef"]
    charstrings, metrics, cmap, report = {}, {}, {}, []

    max_box = UPEM * max(b for _, _, b in ICONS)
    pen = T2CharStringPen(max_box, {})
    charstrings[".notdef"] = pen.getCharString()
    metrics[".notdef"] = (max_box, 0)

    for idx, (name, enum, box_em) in enumerate(ICONS):
        src = os.path.join(ICON_DIR, f"{name}.svg")
        if not os.path.exists(src):
            sys.exit(f"missing {src}")
        contours = svg_to_contours(src, box_em)
        if not contours:
            sys.exit(f"{src}: no path outlines found (Path > Object to Path?)")
        check_overlap(name, contours)

        box = UPEM * box_em
        gname = f"icon.{name}"
        pen = T2CharStringPen(box, {})
        draw(contours, pen)
        charstrings[gname] = pen.getCharString()
        metrics[gname] = (box, 0)
        glyph_order.append(gname)
        cp = CP_BASE + idx
        cmap[cp] = gname

        l, b, r, t = bounds_of(contours)
        report.append((name, enum, cp, box_em, (r - l) / UPEM, (t - b) / UPEM))
        print(f"  {name:9} U+{cp:04X}  {len(contours)} contour(s)  box {box_em} em  "
              f"-> {box_em * 96}px cell   ink {(r-l)/UPEM:.2f} x {(t-b)/UPEM:.2f} em")

    fb = FontBuilder(UPEM, isTTF=False)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap(cmap)
    fb.setupCFF("MatrixIcons", {"FullName": "Matrix Icons", "Weight": "Regular"},
                charstrings, {})
    fb.setupHorizontalMetrics(metrics)
    # Ascent covers the largest box so no consumer clips the glyphs.
    fb.setupHorizontalHeader(ascent=max_box, descent=0)
    fb.setupNameTable({
        "familyName": "Matrix Icons",
        "styleName": "Regular",
        "psName": "MatrixIcons-Regular",
        "version": "1.0",
    })
    fb.setupOS2(sTypoAscender=max_box, sTypoDescender=0,
                usWinAscent=max_box, usWinDescent=0)
    fb.setupPost()

    os.makedirs(os.path.dirname(OTF_OUT), exist_ok=True)
    fb.save(OTF_OUT)
    print(f"wrote {os.path.relpath(OTF_OUT, HERE)}")

    with open(HDR_OUT, "w") as f:
        f.write(
            "// GENERATED by tools/icon_font/build_icon_font.py -- do not edit.\n"
            "//\n"
            "// Icon glyphs live in assets/fonts/icons/matrix-icons.otf at Private\n"
            "// Use Area codepoints and are baked into the shared MTSDF atlas by\n"
            "// PlayerWindow::bakeIconGlyphs().\n"
            "//\n"
            "// Each icon is authored inside its own square DESIGN BOX, in ems,\n"
            "// whose bottom edge sits on the baseline and whose left edge sits at\n"
            "// x=0. The box (not the ink) maps onto the target rect, so icons keep\n"
            "// their relative sizes -- and the box size sets the bake density, so\n"
            "// it is chosen per icon to match where that icon is actually drawn.\n"
            "// See the ICONS table in build_icon_font.py.\n"
            "#pragma once\n\n"
        )
        for name, enum, cp, box_em, iw, ih in report:
            f.write(f"// {name}: {box_em} em box -> {box_em * 96}px cell, "
                    f"ink {iw:.3f} x {ih:.3f} em\n")
            f.write(f"static constexpr unsigned kIconCp{enum}  = 0x{cp:04X};\n")
            f.write(f"static constexpr float    kIconBox{enum} = {float(box_em):.1f}f;\n")
        f.write("\n// Every icon codepoint, for bake + tests.\n")
        f.write("static constexpr unsigned kIconCodepoints[] = {\n")
        for _, enum, _, _, _, _ in report:
            f.write(f"    kIconCp{enum},\n")
        f.write("};\n\n")
        f.write("// Smallest box any icon uses. ui_icons_test asserts every box is at\n"
                "// least this, guarding against an icon being authored at a density\n"
                "// so low it softens at the sizes the app draws it.\n")
        f.write(f"static constexpr float kIconBoxEmMin = "
                f"{float(min(b for _, _, b in ICONS)):.1f}f;\n")

        # Fingerprint over everything that changes baked GEOMETRY. It becomes
        # part of the atlas cache filename, so redrawing an icon (or changing
        # its box) automatically misses the old cache and re-bakes.
        #
        # This exists because the bake is gated on MsdfFont::hasCodepoint(),
        # which only knows whether a codepoint is PRESENT — not whether its
        # artwork changed. Without it, editing an icon left the stale glyph in
        # the cache and it drew at the old size: moving the transport icons to
        # a 2 em box and settings/warning to 1 em, against a leftover 4 em
        # bake, rendered the gear and warning 3.3x oversize and spilling out of
        # their rectangles. "Remember to delete the cache" is not a fix.
        h = 0xCBF29CE484222325
        for name, enum, cp, box_em, iw, ih in report:
            for tok in (name, str(cp), f"{box_em:.3f}", f"{iw:.6f}", f"{ih:.6f}"):
                for ch in tok.encode():
                    h = ((h ^ ch) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        fp = h & 0xFFFFFFFF
        f.write("\n// Fingerprint of the baked icon geometry — see build_icon_font.py.\n"
                "// Part of the atlas cache filename (ui_fonts.hh), so changing any\n"
                "// icon invalidates the cache automatically instead of silently\n"
                "// reusing glyphs baked at the old size.\n")
        f.write(f'static constexpr char kIconSetFingerprint[] = "{fp:08x}";\n')
    print(f"wrote {os.path.relpath(HDR_OUT, HERE)}  (fingerprint {fp:08x})")


if __name__ == "__main__":
    main()
