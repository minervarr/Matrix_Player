#pragma once
#include <android_native_app_glue.h>
#include <memory>

#include "android_platform.hh"  // AndroidSurfaceProvider, AndroidAssetReader
#include "orientation.hh"       // vce::platform::Orientation

class Renderer;
class AndroidPlayerView;

// NativeActivity event loop + Vulkan lifecycle — structurally the Android
// sibling of gui/src/os/windows_host.cc / linux_host.cc, built on the same
// android_main()/ALooper_pollOnce shape framework/vk_canvas/platform/android/
// app.cc already proves out.
//
// Deliberately NOT an implementation of gui/src/host.hh's Host: that
// interface's init() is hard-typed to the concrete desktop PlayerWindow
// class (a 1000+ line sidebar/grid/EQ/settings-panel god-object with no
// touch UI use for this app) with no interface seam another owner type could
// implement — see docs/superpowers/specs/2026-08-08-android-native-port-design.md.
// AndroidHost dispatches into AndroidPlayerView instead, translating
// android_native_app_glue's ALooper/AInputEvent model the way
// LinuxHost::pump()/InputSink translate Wayland's.
class AndroidHost {
public:
    explicit AndroidHost(android_app* state);
    ~AndroidHost();

    void attach(AndroidPlayerView* view) { view_ = view; }

    // Runs until the activity is destroyed.
    void run();

private:
    static void handleAppCmd(android_app* app, int32_t cmd);
    static int32_t handleInputEvent(android_app* app, AInputEvent* event);

    void onWindowInit();
    void onWindowTerm();
    void onGainedFocus();
    void onResume();
    void onPause();

    void drawFrame();

    android_app*       state_ = nullptr;
    AndroidPlayerView*  view_  = nullptr;

    std::unique_ptr<AndroidSurfaceProvider> surface_;
    std::unique_ptr<AndroidAssetReader>     assets_;
    std::unique_ptr<Renderer>               renderer_;

    vce::platform::Orientation orientation_;

    int64_t lastFrameNs_ = 0;
};
