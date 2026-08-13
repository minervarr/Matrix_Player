#include "ui_orientation.hh"

UiOrientation autoOrientationFor(int windowW, int windowH) {
    return (windowH > windowW) ? UiOrientation::Vertical : UiOrientation::Horizontal;
}

UiOrientation UiOrientationState::resolve(int windowW, int windowH) const {
    return autoEnabled ? autoOrientationFor(windowW, windowH) : manual;
}

void UiOrientationState::toggleManual(int windowW, int windowH) {
    const UiOrientation current = resolve(windowW, windowH);
    manual = (current == UiOrientation::Horizontal) ? UiOrientation::Vertical
                                                    : UiOrientation::Horizontal;
    autoEnabled = false;
}
