#pragma once
// Android-only companion to media_session.hh.
//
// Compiled ONLY by android/CMakeLists.txt, next to aoas_output.cc. Desktop
// builds never see this file; they link os/media_session_null.cc instead.
#include <android_native_app_glue.h>

// Called once from android/src/main.cc, before PlayerWindow::create(), for the
// same reason matrixAoasSetApp() is: the JNI upcalls need the activity's VM and
// its classloader, because app classes are invisible to FindClass() from a
// native thread (whose default loader is the system one).
void matrixMediaSessionSetApp(android_app* app);
