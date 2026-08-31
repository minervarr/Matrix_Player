package io.nava.matrixplayer;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

import io.nava.aoas.IAoas;
import io.nava.aoas.IAoasClient;

/**
 * Matrix Player's client half of AOAS (the Android One Audio Server).
 *
 * AOAS owns the USB permission and the live isochronous stream to the DAC;
 * this class binds it, becomes the owner, and hands the ring-buffer file
 * descriptor to native code, which mmaps it and writes wire-format PCM. Every
 * decision about audio — packing, buffering, error text, when to acquire and
 * release — is made in gui/src/os/aoas_output.cc; each method here is a
 * straight line, and if logic starts accumulating in this file it is in the
 * wrong file. (The mirror image of AOAS's own split, where AoasBinder.java is
 * a shell over the C++ server: Java exists because the AIDL contract is
 * generated with the Java backend, nothing more.)
 *
 * Binding requires the signature-level permission io.nava.aoas.BIND_AOAS, so
 * this only works when Matrix Player and AOAS are signed with the same key
 * (two debug builds on one machine share the debug keystore and do bind).
 */
public final class AoasClient {

    static {
        // The native library is already mapped (NativeActivity dlopen()ed it
        // from C), but dlopen from native never registers the library with
        // the JVM for symbol lookup — without this line every native method
        // below throws UnsatisfiedLinkError. Same rule as app_shell's
        // AppShellActivity subclasses.
        System.loadLibrary("matrix_player_android");
    }

    private AoasClient() {}

    // Codes this side produces itself. AOAS's ERR_* are 1..4, so these sit
    // deliberately outside that range and can never be confused with them.
    public static final int ERR_NOT_BOUND = 100;   // service not connected (yet)
    public static final int ERR_DEAD      = 101;   // binder transaction failed

    private static volatile Context sActivity;
    private static volatile IAoas sAoas;
    private static volatile boolean sConnecting;

    /** Called from C++ with the NativeActivity's object: the Context bindService needs. */
    public static void setActivity(Context activity) { sActivity = activity; }

    private static final IAoasClient.Stub sCallback = new IAoasClient.Stub() {
        @Override
        public void onOwnershipLost(int reason) {
            nativeOnOwnershipLost(reason);
        }
    };

    private static final ServiceConnection sConn = new ServiceConnection() {
        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            sAoas = IAoas.Stub.asInterface(service);
            sConnecting = false;
            nativeOnServiceConnected();
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            // AOAS's process died. The framework auto-rebinds; until it does,
            // every proxy call here fails and the owner callback never fires —
            // the server cannot tell us anymore. Native treats this as a
            // REASON_SERVER_STOPPING loss when it believed it owned the device.
            sAoas = null;
            nativeOnServiceDisconnected();
        }
    };

    /** Idempotent. Returns false when AOAS is not installed or refused the binding. */
    public static boolean bind() {
        Context ctx = sActivity;
        if (ctx == null) return false;
        if (sAoas != null || sConnecting) return true;
        Intent intent = new Intent("io.nava.aoas.IAoas").setPackage("io.nava.aoas");
        try {
            sConnecting = true;
            if (!ctx.bindService(intent, sConn, Context.BIND_AUTO_CREATE)) {
                sConnecting = false;
                return false;
            }
            return true;
        } catch (SecurityException e) {
            // Signed with a different key than AOAS — the signature permission
            // was defined by an APK we are not.
            sConnecting = false;
            return false;
        }
    }

    public static boolean isBound() { return sAoas != null; }

    /**
     * Become the owner and get the ring. Returns IAoas.OK / ERR_*, or
     * ERR_NOT_BOUND / ERR_DEAD. On OK the fd has ALREADY been handed to native
     * (nativeOnRingFd mmaps it synchronously on this thread) before this
     * returns, so closing the descriptor afterwards cannot race the mapping —
     * an mmap keeps its own reference to the region.
     *
     * @param ringMillis 0 lets the server apply its default.
     */
    public static int acquire(int sampleRate, int channels, int bitDepth, int ringMillis) {
        IAoas aoas = sAoas;
        if (aoas == null) return ERR_NOT_BOUND;
        ParcelFileDescriptor[] out = new ParcelFileDescriptor[1];
        try {
            int res = aoas.acquire(sCallback, sampleRate, channels, bitDepth, ringMillis, out);
            if (res == IAoas.OK && out[0] != null) {
                // detachFd hands the descriptor to native, which closes it
                // after mmap; this PFD is a no-op from here on.
                nativeOnRingFd(out[0].detachFd());
            }
            return res;
        } catch (RemoteException e) {
            return ERR_DEAD;
        }
    }

    public static void release() {
        IAoas aoas = sAoas;
        if (aoas == null) return;
        try { aoas.release(); } catch (RemoteException ignored) {}
    }

    /** {rate, channels, bitDepth, subslotBytes}, empty when nothing is configured. */
    public static int[] activeFormat() {
        IAoas aoas = sAoas;
        if (aoas == null) return new int[0];
        try { return aoas.activeFormat(); } catch (RemoteException e) { return new int[0]; }
    }

    public static int pendingPlaybackMs() {
        IAoas aoas = sAoas;
        if (aoas == null) return 0;
        try { return aoas.pendingPlaybackMs(); } catch (RemoteException e) { return 0; }
    }

    public static boolean deviceReady() {
        IAoas aoas = sAoas;
        if (aoas == null) return false;
        try { return aoas.isDeviceReady(); } catch (RemoteException e) { return false; }
    }

    public static String deviceInfo() {
        IAoas aoas = sAoas;
        if (aoas == null) return "";
        try { return aoas.deviceInfo(); } catch (RemoteException e) { return ""; }
    }

    static native void nativeOnRingFd(int fd);
    static native void nativeOnOwnershipLost(int reason);
    static native void nativeOnServiceConnected();
    static native void nativeOnServiceDisconnected();
}
