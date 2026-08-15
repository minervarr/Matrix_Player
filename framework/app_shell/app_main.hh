#pragma once

// The application's entry point, as app_shell's platform bootstraps see it.
//
// Each desktop host owns the real one — main() on Wayland, WinMain() on Win32 —
// because what has to happen before an app exists is the platform's business:
// DPI awareness, the crash handler, the log file, COM, the timer resolution.
// When that is done, the bootstrap calls this, and the application takes over.
//
// The app DEFINES it; app_shell only declares and calls it. Deliberately a
// plain free function with no arguments rather than a class to derive from:
// argv is already parsed by the platform on two of the three targets, and an
// app that wants it can read it the way it always could.
//
// Android has no equivalent and needs none — android_main() is the entry point
// there, and it constructs the app and the AndroidHost directly.
int app_shell_main();

// ── The two strings an application gives its shell ──────────────────────────
//
// A log file and a window class have to be CALLED something, and only the app
// knows what. Both are compile definitions with working defaults, so app_shell
// builds and runs on its own; Matrix Player sets them (see gui/CMakeLists.txt)
// to exactly the names it has always used, which is why its matrix_player.log
// and its "MatrixPlayerMain" window class did not change when this moved.
#ifndef APP_SHELL_LOG_NAME
#define APP_SHELL_LOG_NAME "app"
#endif
#ifndef APP_SHELL_LOG_NAME_W
#define APP_SHELL_LOG_NAME_W L"app"
#endif
#ifndef APP_SHELL_WINDOW_CLASS
#define APP_SHELL_WINDOW_CLASS L"AppShellMain"
#endif
