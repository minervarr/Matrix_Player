#pragma once
#include <android_native_app_glue.h>

// ---------------------------------------------------------------------------
// Display-cutout ("notch") safe-area query for a pure-NativeActivity app (no
// Java UI). Same JNI substrate and conventions as
// framework/vk_canvas/platform/android/{jni_util.hh,fullscreen.hh} — see
// safe_area.cc's header comment for why this needs JNI at all when the app
// otherwise writes zero Java.
// ---------------------------------------------------------------------------

struct SafeAreaInsets {
    int top = 0, left = 0, right = 0, bottom = 0;
};

// activity.getWindow().getDecorView().getRootWindowInsets().getDisplayCutout()
// .getSafeInset{Top,Left,Right,Bottom}() — exact pixels. API 28+; on an older
// device, a device with no cutout, or before the first layout pass, this
// degrades to all-zero rather than failing (every step is guarded the same
// way query_nav_bar_height() already is in fullscreen.hh).
//
// Re-query on every window/content-rect change (APP_CMD_WINDOW_RESIZED /
// APP_CMD_CONTENT_RECT_CHANGED) — do NOT cache the result from startup. Once
// the window goes edge-to-edge (immersive), a rotation or a foldable's fold
// state can change the safe area without the app restarting.
SafeAreaInsets query_safe_area_insets(android_app* app);
