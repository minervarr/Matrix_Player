package io.nava.matrixplayer;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * The seam between PlayerWindow and Android's media plumbing.
 *
 * <p>Native calls in, this class holds the state and drives PlaybackService;
 * transport presses come back out through {@link #dispatch}. It makes no
 * decision about audio — the same rule AoasClient.java is held to.
 *
 * <h2>Two threading facts this file exists to absorb</h2>
 *
 * <p><b>Native does not run on Android's main thread.</b> {@code android_main}
 * is its own thread, so every call from C++ arrives off the main looper.
 * Services, notifications and MediaSession callbacks all want the main thread,
 * so everything here is posted to it. Nothing in C++ should have to know that.
 *
 * <p><b>And the reply travels the other way.</b> {@link #dispatch} runs on the
 * main thread (a notification button, a headset press, a focus loss) and hands
 * the command to native, which does <em>not</em> act on it there — it posts an
 * AppEvent and lets the app thread answer, which is the same road the AOAS
 * ownership callback already travels.
 */
public final class MediaSessionBridge {

    private static final String TAG = "MatrixPlayback";

    static {
        // The .so is mapped already (NativeActivity dlopen()ed it), but a
        // dlopen from native never registers the library with the JVM, so
        // without this every native method below throws UnsatisfiedLinkError.
        // Same reason AoasClient.java carries the same line.
        System.loadLibrary("matrix_player_android");
    }

    private MediaSessionBridge() {}

    // Mirrors media_session::Command in gui/src/media_session.hh. Kept as plain
    // ints because that is all that crosses JNI.
    static final int CMD_STOP = 0;
    static final int CMD_NEXT = 1;
    static final int CMD_PREV = 2;
    static final int CMD_PLAY = 3;

    /** Immutable snapshot of what is playing, as the service needs to draw it. */
    static final class State {
        final boolean playing;
        final String title, artist, album, artPath, dspTag;
        final long positionMs, durationMs;

        State(boolean playing, String title, String artist, String album,
              String artPath, String dspTag, long positionMs, long durationMs) {
            this.playing    = playing;
            this.title      = title   != null ? title   : "";
            this.artist     = artist  != null ? artist  : "";
            this.album      = album   != null ? album   : "";
            this.artPath    = artPath != null ? artPath : "";
            this.dspTag     = dspTag  != null ? dspTag  : "";
            this.positionMs = positionMs;
            this.durationMs = durationMs;
        }
    }

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static volatile Context sActivity;
    private static volatile Context sAppContext;
    private static volatile State sState =
            new State(false, "", "", "", "", "", 0, 0);

    /**
     * Called from C++ with the NativeActivity, exactly as AoasClient is.
     *
     * <p>Two contexts are kept, and the difference matters. The ACTIVITY is
     * needed for exactly one thing — requestPermissions(), which only an
     * Activity can ask — and it is the thing that can die: the listener presses
     * Back, the activity is destroyed, and the reference here becomes a handle
     * on a dead object while the foreground service keeps the process alive and
     * the music playing. Starting a service through a destroyed Activity is at
     * best undefined. The APPLICATION context outlives every activity and is
     * what the service calls below use.
     */
    public static void setActivity(Context activity) {
        sActivity = activity;
        if (activity != null) sAppContext = activity.getApplicationContext();
    }

    /** The context the service calls use: it cannot go stale. */
    private static Context serviceContext() {
        Context ctx = sAppContext;
        return ctx != null ? ctx : sActivity;
    }

    /** The service reads the model through this rather than being pushed to. */
    static State snapshot() { return sState; }

    // --- called from native -------------------------------------------------

    /**
     * Playback started. Starts the foreground service, which is what stops
     * Android freezing this process the moment the listener leaves the app.
     *
     * <p>startForegroundService(), not startService(): the service promotes
     * itself immediately in onStartCommand. That is legal here because the
     * listener pressed play while the app was in front of them — the same call
     * would be refused from the background, and AOAS deliberately avoids it for
     * an unrelated reason (its connectedDevice type is illegal until it holds a
     * USB permission; mediaPlayback has no such precondition).
     */
    public static void begin() {
        sMain.post(() -> {
            Context ctx = serviceContext();
            if (ctx == null) { Log.w(TAG, "begin() with no activity"); return; }
            requestNotificationPermission(sActivity);
            try {
                ctx.startForegroundService(new Intent(ctx, PlaybackService.class));
            } catch (Exception e) {
                // A background-start refusal is the realistic case. Playback
                // still works while the app is on screen; it is the screen-off
                // survival that is lost, so say so rather than crashing.
                Log.w(TAG, "could not start the playback service", e);
            }
        });
    }

    /**
     * What is playing, and how far in. Called on every track change and on the
     * player's 250 ms position tick; the C++ side throttles the position so
     * this is not four Binder round trips a second.
     */
    public static void update(String title, String artist, String album,
                              String artPath, String dspTag,
                              long positionMs, long durationMs) {
        sState = new State(true, title, artist, album, artPath, dspTag,
                           positionMs, durationMs);
        sMain.post(() -> {
            // A LAST RESORT, not the mechanism. refresh() does nothing when the
            // service is gone, and the service going while music still plays
            // used to make the notification vanish until the listener stopped
            // and started inside the app. That hole is closed on the C++ side —
            // onTimer() no longer mistakes a track change for the end of the
            // music — and this is here so a future mistake of the same shape
            // degrades instead of disappearing.
            //
            // It cannot be depended on, and that is worth stating rather than
            // discovering: from Android 12 onward a foreground service may not
            // be STARTED from the background, so this heals the case where the
            // app is on screen and fails in exactly the case that matters —
            // listening with the screen off. Keeping the service alive is the
            // fix; this is the apology.
            if (!PlaybackService.isRunning()) {
                Context ctx = serviceContext();
                if (ctx == null) return;
                try {
                    ctx.startForegroundService(new Intent(ctx, PlaybackService.class));
                } catch (Exception e) {
                    Log.w(TAG, "playback service is gone and cannot be restarted", e);
                }
                return;   // onStartCommand() will apply() the state set above
            }
            PlaybackService.refresh();
        });
    }

    /** Playback stopped: the notification, the focus and the wake lock all go. */
    public static void end() {
        State prev = sState;
        sState = new State(false, prev.title, prev.artist, prev.album,
                           prev.artPath, prev.dspTag, 0, prev.durationMs);
        sMain.post(PlaybackService::shutdown);
    }

    // --- called from the main thread, on its way to native ------------------

    /** A transport press, from the notification, a headset, or a focus loss. */
    static void dispatch(int command) {
        nativeOnTransportCommand(command);
    }

    /**
     * POST_NOTIFICATIONS became a runtime permission in API 33. Without it the
     * foreground service still runs — playback survives — but its notification
     * is hidden, so the transport is unreachable from the lock screen.
     *
     * <p>Asked for without reading the answer, deliberately: receiving the
     * result needs Activity.onRequestPermissionsResult, a Java virtual method
     * this app has nowhere to override (its activity is the platform's own
     * NativeActivity). Re-asking is harmless — Android shows the dialog once
     * and then answers from its own record — and the next begin() re-checks.
     */
    private static void requestNotificationPermission(Context ctx) {
        if (ctx == null) return;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return;
        if (!(ctx instanceof Activity)) return;
        if (ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED) return;
        try {
            ((Activity) ctx).requestPermissions(
                    new String[]{ Manifest.permission.POST_NOTIFICATIONS }, 0x4D50);
        } catch (Exception e) {
            Log.w(TAG, "could not ask for POST_NOTIFICATIONS", e);
        }
    }

    static native void nativeOnTransportCommand(int command);
}
