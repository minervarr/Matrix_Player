#pragma once
// Android-only companion to bt_codec.hh, mirroring media_session_android.hh.
//
// Compiled ONLY by android/CMakeLists.txt; desktop builds link
// os/bt_codec_null.cc instead.
#include <android_native_app_glue.h>

// Called once from android/src/main.cc, before PlayerWindow::create(): the JNI
// upcalls need the activity's VM and its classloader.
void matrixBtCodecSetApp(android_app* app);
