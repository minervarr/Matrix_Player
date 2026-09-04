package io.nava.matrixplayer;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.media.MediaMetadata;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;
import android.view.KeyEvent;

/**
 * The foreground service that keeps playback alive, and the MediaSession that
 * makes Android treat this process as a music player.
 *
 * <h2>Why this exists</h2>
 *
 * <p>Matrix Player is a NativeActivity. Without a foreground service nothing in
 * the process tells Android it is playing anything: the process goes cached the
 * moment the listener leaves the app, and on target SDK 36 a cached process is
 * frozen. Playback died at the first track boundary that needed any work.
 *
 * <p>So this service does the three things no amount of C++ can do for itself —
 * hold a {@code mediaPlayback} foreground service, publish a MediaSession, and
 * own the audio focus — and then gets out of the way. It makes no decision
 * about audio. Every transport press here is forwarded to native and answered
 * by PlayerWindow, the same methods the on-screen buttons call. If logic starts
 * accumulating in this file it is in the wrong file — the same rule
 * AoasClient.java is held to.
 *
 * <h2>The notification is bar B</h2>
 *
 * <p>Not a generic media notification: the app's transport bar, in the shape
 * Android gives. Bar B is
 * {@code [pad][artwork][gap][TYPE] < # > [MODE][gap][clock][pad]} — artwork and
 * clock as the masses, release type and DSP tag as the labels, three buttons
 * centred. Here the artwork is the large icon, the title is bar B's own ordinal
 * line, the DSP tag and the length are the sub-text, and the three buttons are
 * the three actions. Exactly three is exactly what MediaStyle shows in the
 * collapsed form, so both forms read the same.
 *
 * <p><b>There is no play/pause action, deliberately.</b> This player has no
 * pause: an interruption ends the track and the next play starts it from zero.
 * A pause button is the thing that would let you resume mid-track, which is
 * precisely the behaviour being refused. Stop holds the fulcrum, as it does in
 * the bar.
 *
 * <h2>Why the session token is NOT attached to the notification</h2>
 *
 * <p>{@code MediaStyle.setMediaSession()} is what promotes a notification into
 * SystemUI's <em>media panel</em> — the carousel in the shade and on the lock
 * screen. That panel does not draw the notification's actions. It draws its own
 * layout from the PlaybackState: three fixed semantic slots, PREVIOUS,
 * PLAY-PAUSE, NEXT, with custom actions relegated to the expanded row. Stop is
 * not one of those slots and there is no way to make it one.
 *
 * <p>Which produced exactly the behaviour that was reported: on the first play
 * the notification is still an ordinary MediaStyle notification and shows our
 * three buttons grouped together — Prev, Stop, Next, bar B's order. A moment
 * later the panel adopts it and re-draws it its own way: Prev jumps to the far
 * left, Next to the far right, and Stop disappears into a row you have to
 * expand to see. Nothing here asked for that and nothing here could prevent it,
 * because those slots belong to SystemUI.
 *
 * <p>So the token is not attached. Without it the notification is never adopted,
 * and it keeps the three actions this file gives it, in this order, for as long
 * as it is on screen. The MediaSession itself is untouched and stays active —
 * it is what routes headset and Bluetooth buttons (see onMediaButtonEvent
 * below) and what tells a car head unit what is playing, and neither of those
 * has ever depended on the notification.
 *
 * <p>What is genuinely given up: the media carousel entry, and with it the
 * output-switcher chip. On the lock screen the transport appears as a normal
 * notification rather than as the big media card. That is the price of owning
 * the three buttons, and it was paid deliberately.
 */
public final class PlaybackService extends Service {

    private static final String TAG = "MatrixPlayback";
    /**
     * Stop, for any controller that reads the session rather than the
     * notification — Android Auto, a watch, a desktop companion. The shade does
     * not need it (the notification carries its own Stop action, see
     * buildNotification), but a session that advertises no way to stop is a
     * session that cannot be stopped from anywhere else.
     */
    private static final String CUSTOM_STOP = "io.nava.matrixplayer.STOP";
    private static final String CHANNEL_ID = "playback";
    private static final int NOTIFICATION_ID = 1;

    static final String ACTION_STOP = "io.nava.matrixplayer.STOP";
    static final String ACTION_NEXT = "io.nava.matrixplayer.NEXT";
    static final String ACTION_PREV = "io.nava.matrixplayer.PREV";

    /**
     * The running instance, or null. Same process as the activity by design —
     * keeping this service alive is what keeps the playback threads running —
     * so a plain reference is the whole IPC story. Written on the main thread
     * in onCreate/onDestroy and read there too; every caller reaches it through
     * MediaSessionBridge, which marshals to the main looper first.
     */
    private static PlaybackService sInstance;

    private MediaSession session;
    private AudioManager audioManager;
    private AudioFocusRequest focusRequest;
    private PowerManager.WakeLock wakeLock;
    private boolean foreground;

    /** Cache of the last artwork decode, keyed by the path it came from. */
    private String artPath;
    private Bitmap art;

    // --- lifecycle ----------------------------------------------------------

    @Override
    public void onCreate() {
        super.onCreate();
        sInstance = this;

        audioManager = getSystemService(AudioManager.class);
        createChannel();

        session = new MediaSession(this, "MatrixPlayer");
        session.setCallback(new MediaSession.Callback() {
            @Override public void onStop()             { MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP); }
            @Override public void onSkipToNext()       { MediaSessionBridge.dispatch(MediaSessionBridge.CMD_NEXT); }
            @Override public void onSkipToPrevious()   { MediaSessionBridge.dispatch(MediaSessionBridge.CMD_PREV); }
            @Override public void onPlay()             { MediaSessionBridge.dispatch(MediaSessionBridge.CMD_PLAY); }

            /**
             * A headset's play/pause button, and anything else that asks to
             * pause, is answered with Stop. There is no pause to give it, and
             * silently ignoring the press would be worse than honouring it as
             * the nearest thing the player actually does.
             */
            @Override public void onPause()            { MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP); }

            /** The Stop advertised on the session (see the PlaybackState below). */
            @Override public void onCustomAction(String action, android.os.Bundle extras) {
                if (CUSTOM_STOP.equals(action))
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP);
            }

            /**
             * The headset's own buttons, taken raw.
             *
             * <p>Overriding this is not belt-and-braces — without it a physical
             * play/pause button does NOTHING. The framework's default
             * implementation translates a media key into onPlay()/onPause()
             * only for actions the session ADVERTISES, and this session
             * deliberately advertises neither: a Pause control drawn in the
             * shade that actually stops is worse than no control. Verified on
             * a moto g06 — with ACTION_PAUSE dropped,
             * {@code cmd media_session dispatch play-pause} reached nothing,
             * while {@code next} still worked because ACTION_SKIP_TO_NEXT is
             * advertised.
             *
             * <p>So the keys are read here instead, before that translation,
             * and the two questions come apart: what the shade DRAWS stays the
             * three buttons of bar B, and what a headset can SEND is everything
             * a headset sends. Returning true consumes the key; anything not
             * listed falls through to super so the framework keeps whatever it
             * would otherwise do.
             *
             * <p>Every button that means "stop making noise" — pause, stop, and
             * the single-press headset hook — answers with Stop, because that
             * is the only thing this player does. Play restarts the track from
             * zero, which is the same promise the transport bar makes.
             */
            @Override public boolean onMediaButtonEvent(Intent intent) {
                if (intent == null) return super.onMediaButtonEvent(intent);
                final KeyEvent key = intent.getParcelableExtra(Intent.EXTRA_KEY_EVENT,
                                                               KeyEvent.class);
                // Act on the press, not the release, or every button fires twice.
                if (key == null || key.getAction() != KeyEvent.ACTION_DOWN)
                    return super.onMediaButtonEvent(intent);

                switch (key.getKeyCode()) {
                case KeyEvent.KEYCODE_MEDIA_PAUSE:
                case KeyEvent.KEYCODE_MEDIA_STOP:
                case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:
                case KeyEvent.KEYCODE_HEADSETHOOK:
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP);
                    return true;
                case KeyEvent.KEYCODE_MEDIA_PLAY:
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_PLAY);
                    return true;
                case KeyEvent.KEYCODE_MEDIA_NEXT:
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_NEXT);
                    return true;
                case KeyEvent.KEYCODE_MEDIA_PREVIOUS:
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_PREV);
                    return true;
                default:
                    return super.onMediaButtonEvent(intent);
                }
            }
        });
        session.setActive(true);

        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null) {
            wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "MatrixPlayer:playback");
            wakeLock.setReferenceCounted(false);
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent != null ? intent.getAction() : null;
        if (ACTION_STOP.equals(action)) {
            MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP);
        } else if (ACTION_NEXT.equals(action)) {
            MediaSessionBridge.dispatch(MediaSessionBridge.CMD_NEXT);
        } else if (ACTION_PREV.equals(action)) {
            MediaSessionBridge.dispatch(MediaSessionBridge.CMD_PREV);
        } else {
            // A plain start: go foreground now. startForegroundService() gives
            // us five seconds to promote, and unlike AOAS (whose connectedDevice
            // type is illegal until it holds a USB permission) mediaPlayback is
            // legal the moment the listener presses play.
            enterForeground();
            // ...and then say what is playing, in the same breath. The
            // notification above is built from whatever snapshot exists at this
            // instant, which on the first play of a process is empty — so
            // without this the shade shows a nameless, artless card until the
            // player's next 250 ms tick, and the session has no metadata for a
            // headset or a head unit to read.
            apply();
        }
        // NOT sticky. A restarted service with no PlayerWindow behind it would
        // put a transport notification on screen for audio that is not playing
        // and cannot be started from here.
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;   // started, never bound: nothing outside this app calls in
    }

    @Override
    public void onDestroy() {
        abandonFocus();
        releaseWakeLock();
        if (session != null) {
            session.setActive(false);
            session.release();
            session = null;
        }
        if (art != null) { art.recycle(); art = null; artPath = null; }
        sInstance = null;
        super.onDestroy();
    }

    // --- what MediaSessionBridge drives ------------------------------------

    /** Is the service alive — i.e. can refresh() actually do anything. */
    static boolean isRunning() { return sInstance != null; }

    /** Push the bridge's current state onto the session and the notification. */
    static void refresh() {
        PlaybackService self = sInstance;
        if (self != null) self.apply();
    }

    /** Stop playing: drop the notification, the focus and the wake lock. */
    static void shutdown() {
        PlaybackService self = sInstance;
        if (self == null) return;
        self.abandonFocus();
        self.releaseWakeLock();
        if (self.session != null) {
            self.session.setPlaybackState(new PlaybackState.Builder()
                    .setState(PlaybackState.STATE_STOPPED, 0, 1.0f)
                    .setActions(PlaybackState.ACTION_PLAY)
                    .build());
        }
        if (self.foreground) {
            self.stopForeground(STOP_FOREGROUND_REMOVE);
            self.foreground = false;
        }
        self.stopSelf();
    }

    private void apply() {
        final MediaSessionBridge.State st = MediaSessionBridge.snapshot();
        if (!st.playing) return;

        acquireWakeLock();
        requestFocus();

        if (session != null) {
            // NO METADATA_KEY_DURATION, and that omission is the whole point.
            // SystemUI draws a scrubber whenever it has a duration and a
            // position, and this player has no seeking on its transport and no
            // wish for one — the bar shows a clock, not a timeline you drag.
            // The duration is still SHOWN, as text, in the sub-text below.
            session.setMetadata(new MediaMetadata.Builder()
                    .putString(MediaMetadata.METADATA_KEY_TITLE, st.title)
                    .putString(MediaMetadata.METADATA_KEY_ARTIST, st.artist)
                    .putString(MediaMetadata.METADATA_KEY_ALBUM, st.album)
                    .putBitmap(MediaMetadata.METADATA_KEY_ALBUM_ART, artwork(st.artPath))
                    .build());

            // PLAYBACK_POSITION_UNKNOWN for the same reason: a position is the
            // other half of what makes a scrubber appear, and nothing on this
            // side wants one. STATE_PLAYING is what marks the session live, and
            // that is kept.
            //
            // ACTION_PAUSE is NOT advertised. It used to be, so that a headset's
            // single button had somewhere to land — but the cost was a Pause
            // control drawn in the shade that actually stopped, which is worse
            // than a button that is simply absent. The CALLBACK stays:
            // onPause() still answers a media key with Stop, and setActions()
            // governs what the shade draws, not what dispatchMediaButtonEvent
            // delivers. So the button on the headset keeps working and the
            // misleading one on screen is gone.
            //
            // These actions describe the SESSION, not the notification. The
            // shade draws Prev/Stop/Next from the Notification.Actions in
            // buildNotification() and never reads this mask (see the class
            // comment on why the session token is not attached). What reads it
            // is everything else that can control a player without seeing its
            // notification — a car head unit over AVRCP, Android Auto, a watch —
            // and to those a session must SAY what it can do or it can do
            // nothing. ACTION_STOP plus the custom action beside it are that
            // statement; the custom action carries a label and an icon, which
            // the bare semantic action does not.
            session.setPlaybackState(new PlaybackState.Builder()
                    .setState(PlaybackState.STATE_PLAYING,
                              PlaybackState.PLAYBACK_POSITION_UNKNOWN, 1.0f)
                    .setActions(PlaybackState.ACTION_STOP
                              | PlaybackState.ACTION_SKIP_TO_NEXT
                              | PlaybackState.ACTION_SKIP_TO_PREVIOUS)
                    .addCustomAction(new PlaybackState.CustomAction.Builder(
                            CUSTOM_STOP, "Stop", R.drawable.ic_transport_stop).build())
                    .build());
        }

        enterForeground();
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && foreground) nm.notify(NOTIFICATION_ID, buildNotification(st));
    }

    // --- notification -------------------------------------------------------

    private void createChannel() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm == null || nm.getNotificationChannel(CHANNEL_ID) != null) return;
        NotificationChannel ch = new NotificationChannel(
                CHANNEL_ID, "Playback", NotificationManager.IMPORTANCE_LOW);   // silent: furniture
        ch.setDescription("The transport for whatever is playing.");
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private void enterForeground() {
        if (foreground) return;
        final Notification n = buildNotification(MediaSessionBridge.snapshot());
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK);
        } else {
            startForeground(NOTIFICATION_ID, n);
        }
        foreground = true;
    }

    /** "REF EQ \u00b7 3:25", or just the tag when the length is unknown. */
    private static String subText(MediaSessionBridge.State st) {
        if (st.durationMs <= 0) return st.dspTag;
        final long total = st.durationMs / 1000;
        final String len = String.format(java.util.Locale.US, "%d:%02d",
                                         total / 60, total % 60);
        return st.dspTag.isEmpty() ? len : st.dspTag + " \u00b7 " + len;
    }

    private Notification buildNotification(MediaSessionBridge.State st) {
        Notification.Builder b = new Notification.Builder(this, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(st.title.isEmpty() ? "Matrix Player" : st.title)
                .setContentText(st.artist)
                // Bar B's DSP tag — EXACT / EXACT* / ALTERED — and the track's
                // LENGTH, in the one slot a media notification has for a third
                // line. The badge says what the chain actually achieves and
                // should not stop saying it because the screen is off; the
                // length is here because the session deliberately publishes no
                // position and no duration, so there is no scrubber to read it
                // off. A label, not a timeline — the same choice bar B makes.
                .setSubText(subText(st))
                .setLargeIcon(artwork(st.artPath))
                .setContentIntent(openAppIntent())
                .setVisibility(Notification.VISIBILITY_PUBLIC)
                .setOngoing(true)
                .setShowWhen(false);

        // Left to right, exactly bar B's order around its fulcrum.
        b.addAction(action(R.drawable.ic_transport_prev, "Previous", ACTION_PREV));
        b.addAction(action(R.drawable.ic_transport_stop, "Stop",     ACTION_STOP));
        b.addAction(action(R.drawable.ic_transport_next, "Next",     ACTION_NEXT));

        // MediaStyle for the shape — large artwork, sub-text, a compact row of
        // exactly the actions named here — but deliberately WITHOUT
        // setMediaSession(). Attaching the token hands the layout to SystemUI's
        // media panel, which replaces these three buttons with its own
        // PREV / PLAY-PAUSE / NEXT slots and drops Stop. See the class comment.
        b.setStyle(new Notification.MediaStyle()
                .setShowActionsInCompactView(0, 1, 2));
        return b.build();
    }

    private Notification.Action action(int icon, String label, String intentAction) {
        Intent i = new Intent(this, PlaybackService.class).setAction(intentAction);
        PendingIntent pi = PendingIntent.getService(
                this, intentAction.hashCode(), i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        return new Notification.Action.Builder(
                android.graphics.drawable.Icon.createWithResource(this, icon), label, pi).build();
    }

    /**
     * Touching the notification — anywhere on it, the artwork included — brings
     * the app BACK. It must never restart it: android_main() owns PlayerWindow
     * and the audio, so a rebuilt activity is silence.
     *
     * <p>Getting there took three tries and the reasons are worth keeping.
     *
     * <p><b>NEW_TASK | CLEAR_TOP</b> was the first, and CLEAR_TOP without
     * SINGLE_TOP on a {@code standard} activity does not bring the existing one
     * forward — it finishes it and creates another.
     *
     * <p><b>getLaunchIntentForPackage() untouched</b> was the second, and it
     * still killed playback. That intent is ACTION_MAIN/CATEGORY_LAUNCHER +
     * NEW_TASK, which looks like the launcher's own — but PackageManager also
     * calls setPackage() on it, and the launcher does not. Android decides
     * "this is the task root's intent, just bring the task forward" with
     * {@link Intent#filterEquals}, which <em>compares the package</em>. One
     * mismatched field and it takes the other branch instead: add a second
     * instance, reset the task, finish the running activity. Two overlapping
     * android_main() threads in one pid, and the music gone.
     *
     * <p>So: build the intent here, leave the package unset so it matches the
     * launcher's byte for byte, and add SINGLE_TOP, which asks in so many words
     * for the existing top activity to receive this intent rather than be
     * replaced. The manifest's {@code launchMode="singleTask"} makes the same
     * promise from the other side; either alone is enough, and having both
     * means a future edit to one cannot quietly resurrect this bug.
     */
    private PendingIntent openAppIntent() {
        Intent i = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (i == null) {
            i = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        }
        i.setPackage(null);
        i.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(this, 0, i,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    /**
     * Album art for the notification, decoded at a size a notification actually
     * uses. Covers in this library run to several thousand pixels square, and
     * handing one of those to the notification manager unscaled is both a large
     * allocation and a Binder parcel that can fail outright.
     */
    private Bitmap artwork(String path) {
        if (path == null || path.isEmpty()) { return null; }
        if (path.equals(artPath) && art != null) return art;

        try {
            BitmapFactory.Options probe = new BitmapFactory.Options();
            probe.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, probe);
            if (probe.outWidth <= 0) return null;

            int sample = 1;
            while (probe.outWidth / (sample * 2) >= 512) sample *= 2;

            BitmapFactory.Options opts = new BitmapFactory.Options();
            opts.inSampleSize = sample;
            Bitmap decoded = BitmapFactory.decodeFile(path, opts);
            if (decoded == null) return null;

            if (art != null) art.recycle();
            art = decoded;
            artPath = path;
            return art;
        } catch (Throwable t) {
            // OutOfMemory included: a missing cover is a cosmetic loss, and it
            // must never be the thing that takes playback down.
            Log.w(TAG, "could not decode artwork: " + path, t);
            return null;
        }
    }

    // --- audio focus --------------------------------------------------------

    /**
     * Any loss of focus stops playback, transient included — a phone call
     * counts. That is this player's stated philosophy: an interruption ends the
     * track, and the next play starts it from zero rather than resuming into
     * the middle of something whose thread you have already lost.
     *
     * <p>{@code setWillPauseWhenDucked(true)} is what keeps that promise
     * against the one case Android would otherwise handle for us: without it a
     * notification sound makes the system DUCK this stream, quietly altering
     * every sample for a few seconds. This player does not alter samples. With
     * it, Android sends a transient loss instead and we stop.
     */
    private void requestFocus() {
        if (audioManager == null || focusRequest != null) return;

        AudioAttributes attrs = new AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                .build();

        focusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(attrs)
                .setWillPauseWhenDucked(true)
                .setOnAudioFocusChangeListener(change -> {
                    if (change == AudioManager.AUDIOFOCUS_GAIN) return;
                    Log.i(TAG, "audio focus lost (" + change + ") — stopping");
                    MediaSessionBridge.dispatch(MediaSessionBridge.CMD_STOP);
                })
                .build();

        final int res = audioManager.requestAudioFocus(focusRequest);
        if (res != AudioManager.AUDIOFOCUS_REQUEST_GRANTED) {
            Log.w(TAG, "audio focus refused (" + res + ")");
            focusRequest = null;
        }
    }

    private void abandonFocus() {
        if (audioManager == null || focusRequest == null) return;
        audioManager.abandonAudioFocusRequest(focusRequest);
        focusRequest = null;
    }

    // --- wake lock ----------------------------------------------------------
    //
    // PARTIAL only: the CPU stays up so the decode and gapless threads keep
    // running with the screen off. It never holds the SCREEN on — that is a
    // different lock and this app has no reason to ask for it.

    private void acquireWakeLock() {
        if (wakeLock != null && !wakeLock.isHeld()) wakeLock.acquire();
    }

    private void releaseWakeLock() {
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
    }
}
