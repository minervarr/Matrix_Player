#pragma once
#include <android_native_app_glue.h>
#include <string>

// Fallback scan root when the launch intent carries no "scan_root" extra.
inline constexpr const char* kDefaultScanRoot = "/storage/emulated/0/Music";

// activity.getIntent().getStringExtra("scan_root") — how this vertical slice
// picks its library folder without SAF's ACTION_OPEN_DOCUMENT_TREE result
// (a pure-NativeActivity app can't receive onActivityResult — see
// storage_permission.hh's header comment). Deliverable via
// `adb shell am start ... --es scan_root /path`. Falls back to
// kDefaultScanRoot if the extra is absent or empty.
std::string read_scan_root_extra(android_app* app);
