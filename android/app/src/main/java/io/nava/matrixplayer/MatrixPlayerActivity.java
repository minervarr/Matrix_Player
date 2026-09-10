package io.nava.matrixplayer;

/**
 * The activity this app runs on, and the only reason it exists is text input.
 *
 * <p>Everything on screen is drawn by {@code gui/src/player_view.cc} through
 * Vulkan; the Java side of this app holds no UI at all. But an input method is
 * not a stream of key presses — it is a view the system focuses, composes into,
 * and reports whole strings back from — and {@code android.app.NativeActivity},
 * which this app used directly until now, has no such view. With it, the guided
 * search box and the AutoEQ profile search could be tapped, showed a caret, and
 * could never be typed into: no keyboard ever came up.
 *
 * <p>{@link io.nava.appshell.AppShellActivity} is that missing half, already
 * written and already compiled into this APK's native library — the off-screen
 * {@code EditText}, the {@code TextWatcher}, the {@code InputMethodManager}
 * calls and the IME-inset reporting. All that was needed to switch it on was a
 * subclass named in the manifest, because {@code GetObjectClass} in
 * {@code activity_bridge.cc} resolves the RUNTIME class: with stock
 * NativeActivity every up-call found no such method, cleared the exception, and
 * silently did nothing.
 *
 * <p>The static block is mandatory even though the library is already mapped.
 * NativeActivity brings it up with {@code dlopen()} from native code, and a
 * dlopen from native never registers the library with the JVM for symbol
 * lookup — without this line every {@code native} method on the superclass
 * throws {@code UnsatisfiedLinkError}. {@code AoasClient} and
 * {@code MediaSessionBridge} already load the same library for the same reason;
 * {@code System.loadLibrary} is refcounted, so a third call is harmless.
 *
 * <p>The library name must match {@code android.app.lib_name} in the manifest
 * and {@code APP_SHELL_APP_NAME} in {@code android/CMakeLists.txt}.
 */
public class MatrixPlayerActivity extends io.nava.appshell.AppShellActivity {
    static { System.loadLibrary("matrix_player_android"); }
}
