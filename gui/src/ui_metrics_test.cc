// Asserts must stay live even though the app builds Release (NDEBUG).
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "ui_metrics.hh"

static bool nearlyEqual(float a, float b) { return std::fabs(a - b) < 0.001f; }

int main() {
    // ── The factor floors at 1.0 and grows only above the reference height ──
    assert(nearlyEqual(computeUiMetrics(700.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(900.0f).scale,  1.0f));
    assert(nearlyEqual(computeUiMetrics(1080.0f).scale, 1.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).scale, 1440.0f / 1080.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).scale, 2.0f));

    // ── Roles at the reference height: floor * ratio^n ──
    UiMetrics m = computeUiMetrics(1080.0f);
    assert(nearlyEqual(m.text.caption,   kMinReadableTextSizePx));
    assert(nearlyEqual(m.text.secondary, kMinReadableTextSizePx));
    assert(nearlyEqual(m.text.body,      kMinReadableTextSizePx * 1.18f));
    assert(nearlyEqual(m.text.title,     kMinReadableTextSizePx * 1.18f * 1.18f));
    assert(nearlyEqual(m.text.header,    kMinReadableTextSizePx * 1.18f * 1.18f * 1.18f));

    // ── The invariant the OLD system violated: adjacent roles keep an exact
    //    ratio at EVERY height, so the hierarchy never distorts. Under the old
    //    seven-percentage scale the smallest role clamped alone below ~1051px
    //    and these ratios collapsed toward 1.0. ──
    for (float h : { 400.0f, 700.0f, 900.0f, 1080.0f, 1440.0f, 2160.0f, 4320.0f }) {
        UiMetrics k = computeUiMetrics(h);
        assert(nearlyEqual(k.text.body   / k.text.caption, kUiTypeRatio));
        assert(nearlyEqual(k.text.title  / k.text.body,    kUiTypeRatio));
        assert(nearlyEqual(k.text.header / k.text.title,   kUiTypeRatio));
        // caption is the smallest role and never drops under the legibility floor
        assert(k.text.caption >= kMinReadableTextSizePx - 0.001f);
        // hairlines never vanish
        assert(k.stroke(1.0f) >= 1.0f);
        // caption and secondary are one size by design
        assert(nearlyEqual(k.text.caption, k.text.secondary));
    }

    // ── space() is linear in the factor; stroke() snaps to whole pixels ──
    assert(nearlyEqual(m.space(65.0f), 65.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).space(65.0f), 130.0f));
    assert(nearlyEqual(m.stroke(1.0f), 1.0f));
    assert(nearlyEqual(m.stroke(3.0f), 3.0f));
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(1.0f), 1.0f));  // round(1.333)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(2.0f), 3.0f));  // round(2.667)
    assert(nearlyEqual(computeUiMetrics(1440.0f).stroke(3.0f), 4.0f));  // round(4.0)
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(1.0f), 2.0f));
    assert(nearlyEqual(computeUiMetrics(2160.0f).stroke(3.0f), 6.0f));

    std::printf("ui_metrics_test: all assertions passed\n");
    return 0;
}
