package io.nava.aoas;

import io.nava.aoas.IAoasClient;

/**
 * The Android One Audio Server: one process owns the USB permission and the
 * live isochronous stream to the DAC, forever; client apps take turns feeding
 * it PCM through shared memory.
 *
 * Exactly one client owns the device at a time. AOAS is not a mixer and never
 * will be (CLAUDE.md, rule 3).
 *
 * Binding requires the signature-level permission io.nava.aoas.BIND_AOAS, so
 * only apps signed with the same key can reach this interface.
 *
 * AOAS copies the bytes a client writes straight to the USB endpoint. It applies
 * no gain, no resampling, no dithering, no mixing -- the ring copy is a memcpy
 * and nothing else touches the samples (CLAUDE.md, rule 2).
 */
interface IAoas {
    // --- acquire() results ---------------------------------------------------
    // acquire() returns one of these and delivers the descriptor through an
    // out-parameter, rather than returning the descriptor and throwing on
    // failure. Throwing would have been tidier, but the exception type AIDL
    // uses to carry a numeric code across a Binder boundary,
    // android.os.ServiceSpecificException, is not in the public SDK -- and the
    // exceptions that ARE marshallable carry only a message, which a client
    // cannot branch on. A code the client can actually switch over beats a
    // prettier signature it has to parse strings out of.

    /** Granted. The out-parameter holds the ring. */
    const int OK = 0;

    /** Another client owns the device. Ownership is never taken by force. */
    const int ERR_BUSY = 1;
    /** No DAC is connected, or Android has not granted the USB permission yet. */
    const int ERR_NO_DEVICE = 2;
    /** The DAC cannot do this format. AOAS will not resample to fake it. */
    const int ERR_FORMAT_UNSUPPORTED = 3;
    /** The shared memory region could not be created or mapped. */
    const int ERR_SHM_FAILED = 4;

    /** Indices into the array returned by activeFormat(). */
    const int FMT_SAMPLE_RATE = 0;
    const int FMT_CHANNELS    = 1;
    const int FMT_BIT_DEPTH   = 2;
    /** Bytes per sample ON THE WIRE, which is not always bitDepth / 8: a DAC
     *  commonly carries 24-bit samples in 4-byte subslots. Clients must lay
     *  out their PCM by this, not by bitDepth. */
    const int FMT_SUBSLOT_BYTES = 3;
    const int FMT_COUNT = 4;

    /**
     * Become the active owner and get the ring buffer to write into.
     *
     * On success the returned descriptor maps to a region laid out by
     * native/shm_ring.hh: a header with atomic read/write indices, then the
     * PCM. Map it, attach a ShmRing to it, and write frames.
     *
     * About the format: AOAS runs the DAC at the current owner's native format
     * and never converts. If the stream is already running at exactly this
     * format -- the common case, and the whole reason this server exists -- the
     * handover is silent, because the isochronous stream is not touched. If it
     * differs, the DAC has to be reconfigured and its clock will re-lock, which
     * is usually audible. That cost is accepted rather than hidden, because the
     * alternative is resampling, and resampling alters every single sample
     * instead of one moment between owners.
     *
     * Ownership also ends if this client's process dies: AOAS holds a death
     * recipient on `client` and frees the device automatically, so a crash
     * never strands the DAC.
     *
     * @param client       callback for losing the device; must not be null.
     * @param sampleRate   Hz, e.g. 44100, 96000.
     * @param channels     e.g. 2.
     * @param bitDepth     significant bits per sample, e.g. 24.
     * @param ringMillis   how much audio the ring should hold. Bigger survives
     *                     scheduling hiccups; smaller lowers latency.
     * @param ringOut      a one-element array the server fills with the shared
     *                     memory descriptor on success, and leaves untouched
     *                     otherwise. Allocate it as
     *                     `new ParcelFileDescriptor[1]`. Delivered together
     *                     with the result so that "granted, but with nowhere to
     *                     write" is not a state that can exist.
     * @return OK, or one of the ERR_* codes.
     */
    int acquire(IAoasClient client, int sampleRate, int channels, int bitDepth,
                int ringMillis, out ParcelFileDescriptor[] ringOut);

    /**
     * Give the device up cleanly. Handover is cooperative: the outgoing owner
     * releases, AOAS confirms by returning, and only then can the next client
     * acquire (CLAUDE.md, rule 4).
     *
     * Drain first. Bytes still in the ring when this returns are discarded --
     * check pendingPlaybackMs() and wait it out if the tail matters.
     * Calling this without owning the device does nothing.
     */
    void release();

    /**
     * The format the DAC is actually running, as
     * {sampleRate, channels, bitDepth, subslotBytes} -- see the FMT_* indices.
     * Empty when no stream is configured. Read it after acquire(): the driver
     * may settle on a wider subslot than requested, and the ring layout follows
     * what the wire actually carries.
     */
    int[] activeFormat();

    /** Milliseconds of real audio still between the ring and the DAC. Lets an
     *  owner drain its tail before release() instead of cutting it off. */
    int pendingPlaybackMs();

    /** True when a DAC is connected, permitted, and the stream is open. */
    boolean isDeviceReady();

    /** Human-readable DAC description for a client's UI. Empty if none. */
    String deviceInfo();

    /**
     * UID of the current owner, or -1 when the device is free. For a client
     * that wants to explain why acquire() came back ERR_BUSY.
     *
     * A UID rather than a package name because turning one into the other
     * needs PackageManager, which is Java-only, and this interface is
     * implemented in C++. AOAS's own notification does that translation on the
     * Java side, where it is free.
     */
    int currentOwnerUid();
}
