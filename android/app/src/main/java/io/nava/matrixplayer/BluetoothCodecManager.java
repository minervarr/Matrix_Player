package io.nava.matrixplayer;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothA2dp;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothCodecConfig;
import android.bluetooth.BluetoothCodecStatus;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothProfile;
import android.companion.AssociationInfo;
import android.companion.AssociationRequest;
import android.companion.BluetoothDeviceFilter;
import android.companion.CompanionDeviceManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.IntentSender;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;

/**
 * A2DP codec control: which codec, rate, depth and LDAC quality the phone
 * negotiates with a pair of Bluetooth headphones.
 *
 * <h2>What this is not</h2>
 *
 * <p>It is <b>not</b> part of the audio path and has nothing to do with AAudio.
 * AAudio hands PCM to AudioFlinger, AudioFlinger hands PCM to the Bluetooth
 * stack, and the <i>stack</i> encodes to SBC/AAC/aptX/LDAC. Nothing that can be
 * set on an AAudioStream touches the codec. It is a property of the ROUTE, which
 * is why the player treats it the way it treats a device's AutoEQ assignment
 * rather than the way it treats an output backend.
 *
 * <h2>How a normal app is allowed to do this at all</h2>
 *
 * <p>{@code setCodecConfigPreference} and {@code getCodecStatus} are hidden
 * {@code @SystemApi} methods on BluetoothA2dp, normally gated behind
 * BLUETOOTH_PRIVILEGED, which is signature-level and unreachable for a
 * sideloaded app. There are three ways in, and this class tries them in order:
 *
 * <ol>
 *   <li><b>A CompanionDeviceManager association.</b> The user consents once, per
 *       device, in a system dialog; AOSP accepts that association in place of
 *       BLUETOOTH_PRIVILEGED for that device. The clean path — no computer, no
 *       root.</li>
 *   <li><b>WRITE_SECURE_SETTINGS, granted once over adb.</b> Then the
 *       Settings.Global keys can be written directly, which is exactly what
 *       Developer Options does.</li>
 *   <li><b>Read-only.</b> Settings.Global is world-readable, so the active codec
 *       can always be REPORTED even when it cannot be changed. That alone is
 *       worth having: the signal-chain readout exists to say what the chain
 *       actually achieves, and over Bluetooth the lossy encode is the largest
 *       thing happening to the audio.</li>
 * </ol>
 *
 * <p>Ported from the previous player (reference/media_player), which is where
 * these paths were worked out against real hardware.
 *
 * <h2>Two timings that are load-bearing</h2>
 *
 * <p>A config is applied {@value #APPLY_DELAY_MS} ms after the device connects
 * and verified {@value #VERIFY_DELAY_MS} ms after that. The stack renegotiates
 * asynchronously and silently ignores a request made too early; both numbers
 * come from the old player, where they were arrived at the hard way.
 */
public final class BluetoothCodecManager {

    private static final String TAG = "MatrixBtCodec";

    private static final long APPLY_DELAY_MS  = 2500;
    private static final long RETRY_DELAY_MS  = 2000;
    private static final long VERIFY_DELAY_MS = 1500;

    // Capability, mirroring bt_codec::Capability in gui/src/bt_codec.hh.
    static final int CAP_UNAVAILABLE = 0;
    static final int CAP_READ_ONLY   = 1;
    static final int CAP_WRITABLE    = 2;

    static { System.loadLibrary("matrix_player_android"); }

    private BluetoothCodecManager() {}

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static volatile Context sActivity;
    private static volatile BluetoothAdapter sAdapter;
    private static volatile BluetoothA2dp sProxy;
    private static volatile boolean sRegistered;

    public static void setActivity(Context activity) { sActivity = activity; }

    // --- lifecycle ----------------------------------------------------------

    private static final BluetoothProfile.ServiceListener sProfileListener =
            new BluetoothProfile.ServiceListener() {
                @Override public void onServiceConnected(int profile, BluetoothProfile proxy) {
                    if (profile == BluetoothProfile.A2DP) sProxy = (BluetoothA2dp) proxy;
                }
                @Override public void onServiceDisconnected(int profile) {
                    if (profile == BluetoothProfile.A2DP) sProxy = null;
                }
            };

    private static final BroadcastReceiver sReceiver = new BroadcastReceiver() {
        @Override
        @SuppressLint("MissingPermission")
        public void onReceive(Context ctx, Intent intent) {
            if (!BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED.equals(intent.getAction())) return;
            final int state = intent.getIntExtra(BluetoothProfile.EXTRA_STATE, -1);
            final BluetoothDevice device = intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if (device == null) return;

            if (state == BluetoothProfile.STATE_CONNECTED) {
                // Delayed, not immediate: the stack is still negotiating, and a
                // request made now is accepted and then thrown away.
                sMain.postDelayed(
                        () -> nativeOnA2dpReady(device.getAddress(), safeName(device)),
                        APPLY_DELAY_MS);
            } else if (state == BluetoothProfile.STATE_DISCONNECTED) {
                nativeOnA2dpGone(device.getAddress());
            }
        }
    };

    /** Idempotent. Safe on a device with no Bluetooth at all. */
    @SuppressLint("MissingPermission")
    public static void start() {
        if (sRegistered) return;
        Context ctx = sActivity;
        if (ctx == null) return;

        sAdapter = BluetoothAdapter.getDefaultAdapter();
        if (sAdapter == null) return;   // no radio: nothing to manage

        requestConnectPermission(ctx);

        try {
            sAdapter.getProfileProxy(ctx, sProfileListener, BluetoothProfile.A2DP);
            IntentFilter f = new IntentFilter(BluetoothA2dp.ACTION_CONNECTION_STATE_CHANGED);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                ctx.registerReceiver(sReceiver, f, Context.RECEIVER_NOT_EXPORTED);
            } else {
                ctx.registerReceiver(sReceiver, f);
            }
            sRegistered = true;
        } catch (Exception e) {
            Log.w(TAG, "could not start Bluetooth codec management", e);
        }
    }

    // --- what native asks ---------------------------------------------------

    /**
     * Which of the three paths this phone actually grants. Answered by probing,
     * never assumed: whether the hidden methods exist at all is a per-ROM
     * question, and whether they may be CALLED is a per-device one.
     */
    public static int capability() {
        if (!hiddenApiPresent()) return CAP_UNAVAILABLE;
        Context ctx = sActivity;
        if (ctx == null) return CAP_READ_ONLY;
        if (hasWriteSecureSettings(ctx)) return CAP_WRITABLE;
        // A CDM association is per-device, so "writable" here means "writable
        // for the device currently connected".
        String mac = connectedMac();
        if (mac != null && hasAssociation(mac)) return CAP_WRITABLE;
        return CAP_READ_ONLY;
    }

    /** MAC of the connected A2DP sink, or null. */
    @SuppressLint("MissingPermission")
    public static String connectedMac() {
        BluetoothA2dp proxy = sProxy;
        if (proxy == null) return null;
        try {
            List<BluetoothDevice> connected = proxy.getConnectedDevices();
            if (connected == null || connected.isEmpty()) return null;
            return connected.get(0).getAddress();
        } catch (Exception e) {
            return null;
        }
    }

    /** Human name of the connected A2DP sink, or "". */
    public static String connectedName() {
        BluetoothDevice d = deviceFor(connectedMac());
        return d == null ? "" : safeName(d);
    }

    /**
     * The codec actually running, as
     * {codec, sampleRateMask, bitsMask, channelMask, ldacQuality}, or an empty
     * array when it cannot be read.
     *
     * <p>Tries getCodecStatus first (exact, needs the association) and falls
     * back to Settings.Global, which anyone may read. The fallback is why the
     * signal chain can name the codec on a phone that refuses everything else.
     */
    @SuppressLint("MissingPermission")
    public static int[] activeConfig() {
        BluetoothDevice device = deviceFor(connectedMac());
        if (device != null) {
            BluetoothCodecStatus status = codecStatus(device);
            if (status != null && status.getCodecConfig() != null) {
                BluetoothCodecConfig c = status.getCodecConfig();
                long ldac = c.getCodecSpecific1();
                if (ldac >= 1000) ldac -= 1000;
                return new int[]{ c.getCodecType(), c.getSampleRate(),
                                  c.getBitsPerSample(), c.getChannelMode(), (int) ldac };
            }
        }
        return fromSecureSettings();
    }

    /**
     * Which codecs THIS pair of headphones can actually take, as
     * "id\u0009name" lines — the id AOSP uses in BluetoothCodecConfig, and the
     * name the stack itself prints for it.
     *
     * <p>Read from {@code getCodecsSelectableCapabilities()}, which is the
     * stack's own answer to "what could this device be switched to". It
     * replaced a hardcoded five-row list of SBC/AAC/aptX/aptX HD/LDAC, which
     * was wrong in both directions on real hardware: it offered LDAC and aptX
     * to headphones that only speak SBC and AAC, and it could never mention
     * LC3, Opus, aptX Adaptive or any vendor codec, because they were not in
     * the enum. Asking the device removes both failures at once and needs no
     * new entry here when Android gains a codec.
     *
     * <p>The NAME comes from the stack too, on API 33+, for the same reason —
     * a local table could only ever name the codecs it already knew about.
     *
     * <p>An empty string means the question could not be answered (no
     * association, no hidden API, nothing connected). That is NOT the same as
     * "no codecs", and the caller must not draw it as a list of none.
     */
    @SuppressLint({"MissingPermission", "NewApi"})
    public static String selectableCodecs() {
        BluetoothDevice device = deviceFor(connectedMac());
        if (device == null) return "";
        BluetoothCodecStatus status = codecStatus(device);
        if (status == null) return "";

        List<BluetoothCodecConfig> caps;
        try {
            caps = status.getCodecsSelectableCapabilities();
        } catch (Throwable t) {
            return "";
        }
        if (caps == null || caps.isEmpty()) return "";

        StringBuilder sb = new StringBuilder();
        for (BluetoothCodecConfig c : caps) {
            if (c == null) continue;
            final int type = c.getCodecType();
            final String name = codecDisplayName(c, type);
            if (sb.length() > 0) sb.append('\n');
            sb.append(type).append('\t').append(name);
        }
        return sb.toString();
    }

    /**
     * The stack's own name for a codec, by reflection, with a fallback.
     *
     * <p>{@code BluetoothCodecConfig.getCodecName()} exists on modern AOSP but
     * is not in the public SDK this app compiles against, so it is reached the
     * same way {@code setCodecConfigPreference} is. When it is missing the
     * fallback names only the five ids that were frozen into AOSP long ago and
     * prints the raw id for anything newer — which is still better than
     * omitting a codec the device really does support, and is the case that
     * made the old hardcoded list wrong.
     */
    private static String codecDisplayName(BluetoothCodecConfig c, int type) {
        try {
            Method m = BluetoothCodecConfig.class.getMethod("getCodecName");
            Object v = m.invoke(c);
            if (v instanceof String && !((String) v).isEmpty()) return (String) v;
        } catch (Throwable ignored) { /* fall through */ }
        switch (type) {
        case 0:  return "SBC";
        case 1:  return "AAC";
        case 2:  return "aptX";
        case 3:  return "aptX HD";
        case 4:  return "LDAC";
        default: return "Codec " + type;
        }
    }

    /**
     * Ask the stack for this configuration. Returns true only when the call was
     * accepted — NOT that the codec changed, which is asked separately a moment
     * later, because the stack can accept a request and then negotiate something
     * else with the headphones.
     */
    @SuppressLint("MissingPermission")
    public static boolean apply(String mac, int codec, int sampleRate, int bits,
                                int channelMode, int ldacQuality) {
        BluetoothDevice device = deviceFor(mac);
        BluetoothA2dp proxy = sProxy;
        if (device == null || proxy == null) return false;

        BluetoothCodecConfig cfg = buildConfig(codec, sampleRate, bits, channelMode, ldacQuality);
        if (cfg == null) return false;

        boolean sent = invokeSetCodecConfigPreference(device, cfg);
        if (!sent) {
            // One retry: the proxy can be a moment behind the connection.
            sMain.postDelayed(() -> {
                if (invokeSetCodecConfigPreference(device, cfg)) scheduleVerify(mac, codec);
            }, RETRY_DELAY_MS);
            return false;
        }
        scheduleVerify(mac, codec);
        return true;
    }

    private static void scheduleVerify(String mac, int wantedCodec) {
        sMain.postDelayed(() -> {
            int[] now = activeConfig();
            final boolean ok = now.length > 0 && now[0] == wantedCodec;
            nativeOnCodecVerified(mac, ok, now.length > 0 ? now[0] : -1);
        }, VERIFY_DELAY_MS);
    }

    /** Paired devices, flattened as MAC and name pairs for the settings list. */
    @SuppressLint("MissingPermission")
    public static String[] pairedDevices() {
        BluetoothAdapter adapter = sAdapter;
        if (adapter == null) return new String[0];
        try {
            List<String> out = new ArrayList<>();
            for (BluetoothDevice d : adapter.getBondedDevices()) {
                out.add(d.getAddress());
                out.add(safeName(d));
            }
            return out.toArray(new String[0]);
        } catch (Exception e) {
            return new String[0];
        }
    }

    /** The line to show the listener when adb is the only remaining path. */
    public static String adbGrantCommand() {
        Context ctx = sActivity;
        String pkg = ctx != null ? ctx.getPackageName() : "io.nava.matrixplayer";
        return "adb shell pm grant " + pkg + " android.permission.WRITE_SECURE_SETTINGS";
    }

    // --- CompanionDeviceManager --------------------------------------------

    @SuppressLint("MissingPermission")
    public static boolean hasAssociation(String mac) {
        Context ctx = sActivity;
        if (ctx == null || mac == null) return false;
        CompanionDeviceManager cdm = ctx.getSystemService(CompanionDeviceManager.class);
        if (cdm == null) return false;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                for (AssociationInfo info : cdm.getMyAssociations()) {
                    Object addr = info.getDeviceMacAddress();
                    if (addr != null && mac.equalsIgnoreCase(addr.toString())) return true;
                }
            } else {
                @SuppressWarnings("deprecation")
                List<String> assoc = cdm.getAssociations();
                for (String a : assoc) if (mac.equalsIgnoreCase(a)) return true;
            }
        } catch (Exception e) {
            Log.w(TAG, "could not read companion associations", e);
        }
        return false;
    }

    /**
     * Ask the user to associate this device, which is what makes
     * setCodecConfigPreference legal for it. Shows a system dialog; the result
     * is not read back (the app's activity is the platform's own NativeActivity
     * and has nowhere to override onActivityResult) — hasAssociation() is
     * re-checked afterwards instead.
     */
    @SuppressLint("MissingPermission")
    public static void requestAssociation(String mac) {
        Context ctx = sActivity;
        if (!(ctx instanceof Activity) || mac == null) return;
        final Activity activity = (Activity) ctx;
        CompanionDeviceManager cdm = ctx.getSystemService(CompanionDeviceManager.class);
        if (cdm == null) return;

        AssociationRequest request = new AssociationRequest.Builder()
                .addDeviceFilter(new BluetoothDeviceFilter.Builder().setAddress(mac).build())
                .setSingleDevice(true)
                .build();
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                Executor exec = activity.getMainExecutor();
                cdm.associate(request, exec, new CompanionDeviceManager.Callback() {
                    @Override public void onAssociationPending(IntentSender sender) {
                        launch(activity, sender);
                    }
                    @Override public void onAssociationCreated(AssociationInfo info) {
                        nativeOnAssociationChanged(mac, true);
                    }
                    @Override public void onFailure(CharSequence error) {
                        Log.w(TAG, "association failed: " + error);
                        nativeOnAssociationChanged(mac, false);
                    }
                });
            } else {
                @SuppressWarnings("deprecation")
                CompanionDeviceManager.Callback cb = new CompanionDeviceManager.Callback() {
                    @Override public void onDeviceFound(IntentSender sender) { launch(activity, sender); }
                    @Override public void onFailure(CharSequence error) {
                        Log.w(TAG, "association failed: " + error);
                        nativeOnAssociationChanged(mac, false);
                    }
                };
                cdm.associate(request, cb, sMain);
            }
        } catch (Exception e) {
            Log.w(TAG, "could not start association", e);
        }
    }

    private static void launch(Activity activity, IntentSender sender) {
        try {
            activity.startIntentSenderForResult(sender, 0x4254, null, 0, 0, 0);
        } catch (IntentSender.SendIntentException e) {
            Log.w(TAG, "could not show the association dialog", e);
        }
    }

    // --- the hidden API -----------------------------------------------------

    private static boolean hiddenApiPresent() {
        try {
            BluetoothA2dp.class.getMethod("setCodecConfigPreference",
                    BluetoothDevice.class, BluetoothCodecConfig.class);
            BluetoothA2dp.class.getMethod("getCodecStatus", BluetoothDevice.class);
            return true;
        } catch (NoSuchMethodException e) {
            return false;
        }
    }

    private static boolean invokeSetCodecConfigPreference(BluetoothDevice device,
                                                          BluetoothCodecConfig config) {
        BluetoothA2dp proxy = sProxy;
        if (proxy == null) return false;
        try {
            Method m = BluetoothA2dp.class.getMethod("setCodecConfigPreference",
                    BluetoothDevice.class, BluetoothCodecConfig.class);
            m.invoke(proxy, device, config);
            return true;
        } catch (Exception e) {
            if (e.getCause() instanceof SecurityException) {
                Log.w(TAG, "setCodecConfigPreference refused: no association and no "
                         + "WRITE_SECURE_SETTINGS");
            } else {
                Log.w(TAG, "setCodecConfigPreference failed", e);
            }
            return false;
        }
    }

    private static BluetoothCodecStatus codecStatus(BluetoothDevice device) {
        BluetoothA2dp proxy = sProxy;
        if (proxy == null) return null;
        try {
            Method m = BluetoothA2dp.class.getMethod("getCodecStatus", BluetoothDevice.class);
            return (BluetoothCodecStatus) m.invoke(proxy, device);
        } catch (Exception e) {
            return null;   // no association: the Settings.Global fallback answers
        }
    }

    @SuppressLint("NewApi")
    private static BluetoothCodecConfig buildConfig(int codec, int sampleRate, int bits,
                                                    int channelMode, int ldacQuality) {
        // LDAC's quality rides in codecSpecific1, offset by 1000 — an AOSP
        // convention, not an invention here.
        final long specific1 = (codec == 4) ? 1000 + ldacQuality : 0;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            try {
                return new BluetoothCodecConfig.Builder()
                        .setCodecType(codec)
                        .setSampleRate(sampleRate)
                        .setBitsPerSample(bits)
                        .setChannelMode(channelMode)
                        .setCodecSpecific1(specific1)
                        .setCodecPriority(1000000)   // CODEC_PRIORITY_HIGHEST
                        .build();
            } catch (Exception e) {
                Log.w(TAG, "codec config builder failed", e);
            }
        }
        try {
            @SuppressWarnings("JavaReflectionMemberAccess")
            Constructor<BluetoothCodecConfig> ctor = BluetoothCodecConfig.class.getConstructor(
                    int.class, int.class, int.class, int.class, int.class,
                    long.class, long.class, long.class, long.class);
            return ctor.newInstance(codec, 1000000, sampleRate, bits, channelMode,
                                    specific1, 0L, 0L, 0L);
        } catch (Exception e) {
            Log.w(TAG, "codec config reflection constructor failed", e);
            return null;
        }
    }

    /**
     * What the Bluetooth stack wrote about itself. World-readable, so this is
     * the one path that always works — it is how the codec gets NAMED on a
     * phone that refuses to let it be changed.
     */
    private static int[] fromSecureSettings() {
        Context ctx = sActivity;
        if (ctx == null) return new int[0];
        try {
            int codec = Settings.Global.getInt(ctx.getContentResolver(),
                    "bluetooth_a2dp_codec", -1);
            if (codec < 0) return new int[0];
            int rate = Settings.Global.getInt(ctx.getContentResolver(),
                    "bluetooth_a2dp_sample_rate", 0x1);
            int bits = Settings.Global.getInt(ctx.getContentResolver(),
                    "bluetooth_a2dp_bits_per_sample", 0x1);
            int chan = Settings.Global.getInt(ctx.getContentResolver(),
                    "bluetooth_a2dp_channel_mode", 0x2);
            long ldac = Settings.Global.getLong(ctx.getContentResolver(),
                    "bluetooth_a2dp_ldac_playback_quality", 1000);
            if (ldac >= 1000) ldac -= 1000;
            return new int[]{ codec, rate, bits, chan, (int) ldac };
        } catch (Exception e) {
            return new int[0];
        }
    }

    static boolean hasWriteSecureSettings(Context ctx) {
        return ctx.checkSelfPermission("android.permission.WRITE_SECURE_SETTINGS")
                == PackageManager.PERMISSION_GRANTED;
    }

    // --- odds and ends ------------------------------------------------------

    @SuppressLint("MissingPermission")
    private static BluetoothDevice deviceFor(String mac) {
        BluetoothAdapter adapter = sAdapter;
        if (adapter == null || mac == null) return null;
        try {
            return adapter.getRemoteDevice(mac);
        } catch (IllegalArgumentException e) {
            return null;   // not a MAC
        }
    }

    /** getName() needs BLUETOOTH_CONNECT and throws without it. */
    @SuppressLint("MissingPermission")
    private static String safeName(BluetoothDevice d) {
        try {
            String n = d.getName();
            return n != null ? n : d.getAddress();
        } catch (SecurityException e) {
            return d.getAddress();
        }
    }

    private static void requestConnectPermission(Context ctx) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return;
        if (!(ctx instanceof Activity)) return;
        if (ctx.checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)
                == PackageManager.PERMISSION_GRANTED) return;
        try {
            ((Activity) ctx).requestPermissions(
                    new String[]{ Manifest.permission.BLUETOOTH_CONNECT }, 0x4254);
        } catch (Exception e) {
            Log.w(TAG, "could not ask for BLUETOOTH_CONNECT", e);
        }
    }

    static native void nativeOnA2dpReady(String mac, String name);
    static native void nativeOnA2dpGone(String mac);
    static native void nativeOnCodecVerified(String mac, boolean ok, int actualCodec);
    static native void nativeOnAssociationChanged(String mac, boolean associated);
}
