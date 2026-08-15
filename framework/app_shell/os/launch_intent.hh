#pragma once
#include <android_native_app_glue.h>
#include <string>

// activity.getIntent().getStringExtra(key).
//
// This is how a pure-NativeActivity app receives an argument at all: it cannot
// get SAF's ACTION_OPEN_DOCUMENT_TREE result back, because onActivityResult
// needs Java (see storage_permission.hh's header comment). Deliverable from a
// terminal with `adb shell am start ... --es <key> <value>`.
//
// Returns `fallback` when the extra is absent or empty. The KEY used to be
// hardcoded to "scan_root" here, which is a music player's word in a file that
// only knows about intents; AndroidHost is told the key now.
std::string read_string_extra(android_app* app, const char* key,
                              const std::string& fallback = {});
