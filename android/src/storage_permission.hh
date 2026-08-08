#pragma once
#include <android_native_app_glue.h>

// ---------------------------------------------------------------------------
// Storage access for a sideload-only (never Play Store) app that needs real
// filesystem paths — see docs/superpowers/specs/2026-08-08-android-native-port-design.md.
// Same JNI substrate/conventions as jni_util.hh/fullscreen.hh: every call
// here is a JNI hop into system-provided Java classes (Intent/Uri/Environment/
// Settings), never into a class this project writes.
// ---------------------------------------------------------------------------

// Fires Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION for this app's
// package via startActivity() — the "All files access" special-permission
// screen (API 30+). The user grants it once, by hand, in system Settings; no
// result is read back (see show_folder_picker_hint()'s comment on why a
// NativeActivity-only app can't receive an onActivityResult callback anyway)
// — re-check has_all_files_access() on the next APP_CMD_RESUME instead.
void request_all_files_access(android_app* app);

// Environment.isExternalStorageManager() (API 30+; false — not a crash — on
// older platforms where the method doesn't exist).
bool has_all_files_access(android_app* app);

// Fire-and-forget ACTION_OPEN_DOCUMENT_TREE (startActivity(), NOT
// startActivityForResult()). Its result Uri is never consumed: receiving an
// activity result requires overriding Activity.onActivityResult(), a Java
// virtual method — off the table under this project's zero-.java rule. This
// exists purely as a visibility/consent gesture (the user sees Android's own
// folder picker once), not as a way to choose the scan root — that comes
// from the launch intent's "scan_root" extra instead (see launch_intent.hh).
void show_folder_picker_hint(android_app* app);
