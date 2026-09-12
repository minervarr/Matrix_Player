// terminus_glyph_test -- the baked Terminus glyph and its run-length rects.
// Links src/terminus_glyph.cc and nothing else. Convention matches
// framework/vk_canvas/core/tests/*.cc: plain assert(), NDEBUG undefined.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <vector>

#include "terminus_glyph.hh"

// Paint runs into a grid and compare with the glyph's bits expanded by
// `scale`: every set bit covered exactly once, no unset bit covered at all.
static void checkExact(const terminus::Glyph& g, int scale) {
    std::vector<terminus::Run> rs;
    terminus::runs(g, scale, rs);
    const int W = g.cellW * scale, H = g.cellH * scale;
    std::vector<int> hit((size_t)W * H, 0);
    for (const terminus::Run& r : rs) {
        assert(r.w > 0 && r.h > 0);
        assert(r.x >= 0 && r.y >= 0 && r.x + r.w <= W && r.y + r.h <= H);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x) hit[(size_t)y * W + x]++;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const bool on = (g.rows[y / scale] >> (x / scale)) & 1u;
            assert(hit[(size_t)y * W + x] == (on ? 1 : 0));
        }
}

int main() {
    // The one glyph bar A draws exists in both strikes and is not blank.
    const terminus::Pick small = terminus::pickStrike(U'S', 20.0f);
    const terminus::Pick big   = terminus::pickStrike(U'S', 30.0f);
    assert(small.glyph && small.glyph->cellW == 8  && small.glyph->cellH == 16);
    assert(big.glyph   && big.glyph->cellW   == 16 && big.glyph->cellH   == 32);

    // Integer scale only, chosen from the target height.
    assert(small.scale == 1);                                   // < 24 -> 16 px x1
    assert(big.scale == 1);                                     // 30 -> 32 px x1
    assert(terminus::pickStrike(U'S', 48.0f).scale == 2);       // round(1.5)
    assert(terminus::pickStrike(U'S', 100.0f).scale == 3);      // round(3.125)
    assert(terminus::pickStrike(U'S', 1.0f).scale >= 1);        // never 0

    // A glyph that was not baked is reported, not drawn as garbage. `Q` was
    // the example while only `S`/`F` were baked; the whole printable ASCII
    // set is baked now, so the negative case has to be genuinely outside it.
    assert(terminus::pickStrike(U'é', 30.0f).glyph == nullptr);   // é

    // The runs reproduce the bitmap exactly, at every scale we might use.
    for (int s = 1; s <= 3; ++s) {
        checkExact(*small.glyph, s);
        checkExact(*big.glyph, s);
    }

    // ── Running text: fold, always-resolve, wrap, ellipsis ─────────────────
    //
    // Settings is a Terminus surface: every string draws. Unrepresentable
    // scalars fold (em dash to '-', middle dot to '/') or become '?'. Width
    // is never -1 -- that was the old chrome-fallback signal, and it is what
    // mixed Computer Modern into the panels.
    assert(terminusTextWidth("", 20.0f) == 0.0f);
    assert(terminusTextWidth("Done", 20.0f) > 0.0f);
    assert(terminusTextWidth("done", 20.0f) == terminusTextWidth("DONE", 20.0f));  // case-folded

    assert(terminusFoldCp(U'-') == '-');
    assert(terminusFoldCp(0x2014) == '-');          // em dash
    assert(terminusFoldCp(0x2013) == '-');          // en dash
    assert(terminusFoldCp(0x00B7) == '/');          // middle dot
    assert(terminusFoldCp(0x2022) == '/');          // bullet
    assert(terminusFoldCp(0x2219) == '/');          // bullet operator
    assert(terminusFoldCp(0x00C2) == '?');          // Â -- the lead byte of the EQ mojibake
    assert(terminusFoldCp(U'é') == '?');

    assert(terminusFold("plug in headphones \xE2\x80\x94 there is no device to pick here.")
           == "PLUG IN HEADPHONES - THERE IS NO DEVICE TO PICK HERE.");
    // Double-encoded middle dot: UTF-8 for Â (C3 82) + UTF-8 for · (C2 B7).
    assert(terminusFold("  \xC3\x82\xC2\xB7  ") == "  ?/  ");
    assert(terminusFold("caf\xC3\xA9") == "CAF?");
    assert(terminusTextWidth("caf\xC3\xA9", 20.0f) == terminusTextWidth("CAF?", 20.0f));
    assert(terminusTextWidth("SBC is a lossy encode \xE2\x80\x94 this route can never be bit-perfect.",
                             20.0f) ==
           terminusTextWidth("SBC IS A LOSSY ENCODE - THIS ROUTE CAN NEVER BE BIT-PERFECT.", 20.0f));

    {
        const float maxW = 8.0f * 20;   // 20 cells of the 16 px strike
        auto lines = terminusWrap(
            "16-bit output. Not a bit-perfect path for deeper sources.", 20.0f, maxW);
        assert(lines.size() >= 2);
        for (const auto& ln : lines)
            assert(terminusTextWidth(ln, 20.0f) <= maxW + 0.01f);
    }
    {
        std::string e = terminusEllipsize(
            "Sennheiser HD 650 (over-ear) / oratory1990", 20.0f, 8.0f * 10);
        assert(e.size() >= 3);
        assert(e.compare(e.size() - 3, 3, "...") == 0);
        assert(terminusTextWidth(e, 20.0f) <= 8.0f * 10 + 0.01f);
    }

    // Every literal chrome string actually wrapped in Settings (Tasks 3-6)
    // must round-trip with ZERO fallback -- the same "a missing glyph is a
    // build-time failure" discipline the Settings S already has, extended to
    // whole strings. If a future edit adds a character outside the baked
    // ASCII set to one of these, this line is what catches it.
    static const char* kChromeStrings[] = {
        "Music Folders", "No music folders added yet.", "Remove Selected", "Done",
        "Close",
        "Audio Output Settings", "Output backend:", "USB DAC:",
        "No USB audio devices found.", "Device:", "Mode:", "Starting port:",
        "No running JACK server found (or no physical playback ports).",
        "Headphones:", "No paired A2DP device. Pair and connect a pair of headphones first.",
        "Android chooses the output route itself, and follows it when you",
        "16-bit output. Not a bit-perfect path for deeper sources.",
        "Release the device in your sound server first; only one app can stream to it.",
        "Playback goes through the AOAS service, which owns the USB",
        "and silent across app switches. AOAS must be installed and",
        "signed with the same key as this app. There is no device to pick here.",
        "Apply",
        "Bluetooth codec", "No Bluetooth headphones connected.",
        "This phone has not granted codec control. Two ways to get it:",
        "Pair as companion device", "...or, from a computer, once:", "Forget",
        "Apply saves this against these headphones and re-applies it whenever they reconnect.",
        "EQ / AutoEQ Profiles", "My Drivers", "All Profiles", "Best only", "Every source",
        "No saved drivers match.", "No profiles match.",
        "Select", "Pin", "Unpin", "Remove", "Assign to Device", "Clear",
        "Select Music Folder", "No subfolders here.", "Cancel", "Select This Folder",
        "Interface", "Scrolling", "Flicking a list throws it, and it slows to a stop on its own.",
    };
    for (const char* s : kChromeStrings)
        assert(terminusTextWidth(s, 20.0f) >= 0.0f);

    printf("terminus_glyph_test: all assertions passed\n");
    return 0;
}
