#include "ui_metrics.hh"

#include <algorithm>
#include <cmath>

float UiMetrics::stroke(float authored) const {
    return std::max(1.0f, std::round(authored * scale));
}

UiMetrics computeUiMetrics(float contentHeight) {
    UiMetrics m;

    // floorScale = 1.0 (not UiScale's 0.5 default): "never smaller than the
    // reference". The app force-fullscreens in Complete mode and gates the
    // mode choice on monitor height (see kMinWindowContentH), so shrinking
    // below the reference is not a case worth serving — and allowing it is
    // what let the old type scale collapse flat.
    UiScale s{ kUiReferenceHeight, /*floorScale=*/1.0f };
    m.scale = s.factor(contentHeight);

    const float caption = kMinReadableTextSizePx * m.scale;
    m.text.caption   = caption;
    m.text.secondary = caption;
    m.text.body      = caption * kUiTypeRatio;
    m.text.title     = m.text.body  * kUiTypeRatio;
    m.text.header    = m.text.title * kUiTypeRatio;
    return m;
}
