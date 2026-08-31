package io.nava.aoas;

/**
 * The callback surface AOAS uses to tell a client it no longer owns the DAC.
 *
 * This exists because of a specific failure mode: without it, a client that
 * lost the device finds out only when its next ring write stops being accepted,
 * which looks exactly like "the buffer is momentarily full". It would keep
 * writing into a ring nobody drains and show a playing UI over silence. Losing
 * the device is a fact the server knows first, so the server says it.
 *
 * Every method is `oneway`: AOAS calls these from the thread that just took the
 * device away, and it must not be parked waiting for a client's UI to react --
 * least of all a client that is already misbehaving.
 *
 * (Resolves open question 2 in CLAUDE.md.)
 */
oneway interface IAoasClient {
    /** The user pressed disconnect in AOAS's own notification. Deliberate. */
    const int REASON_USER_DISCONNECTED = 1;
    /** The DAC was physically unplugged; Android revoked the USB permission. */
    const int REASON_DEVICE_DETACHED = 2;
    /** AOAS is shutting down (stopped, or killed by the OS). */
    const int REASON_SERVER_STOPPING = 3;

    /**
     * Ownership is gone. Stop writing to the ring immediately: the shared
     * mapping may be unmapped or handed to the next owner right after this
     * call returns. A player should pause rather than fail silently.
     *
     * @param reason one of the REASON_* constants above.
     */
    void onOwnershipLost(int reason);
}
