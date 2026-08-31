# AOAS client backend — Matrix Player as the first client of the Android One Audio Server

Date: 2026-08-28
Status: implemented
Sibling project: `AndroidOneAudioServer_AOAS` (its `CLAUDE.md` and `docs/design.md`
are the authority on the server; this doc covers only the client side).

## What this is

AOAS is a standalone Android service that owns the USB permission grant and the
live isochronous stream to one DAC, forever. Exactly one client app at a time
feeds it raw PCM through a shared-memory ring; AOAS copies those bytes to the
USB endpoint untouched. Matrix Player gains a fifth `AudioBackend` —
`AudioBackend::Aoas` — that makes the phone a client of that server instead of
racing it for the DAC. The other three backends (USB direct, AAudio, and
nothing on the desktop) are unchanged; `Aoas` is Android-only and compiled only
by `android/CMakeLists.txt`.

## Why a Java file exists at all

`IAoas` is an AIDL contract generated with the **Java** backend (AOAS's
CLAUDE.md, open question 1: the NDK ships libbinder_ndk's C headers only; the
C++ headers the generated code needs exist only in the platform build, and
hand-crafting Binder parcels is hand-rolling the wire format — the thing the
Java backend exists to prevent). So the client needs one Java class.

`android/app/src/main/java/io/nava/matrixplayer/AoasClient.java` is that class,
and it holds **no audio decisions**: it binds the service, caches the `IAoas`
proxy, calls `acquire()`/`release()`/`activeFormat()`/`pendingPlaybackMs()`/
`deviceInfo()` when C++ asks, delivers the ring fd to native synchronously
inside `acquire()`, and forwards `onOwnershipLost` down. Every method is a
straight line; if logic starts accumulating there, it is in the wrong file.
This mirrors AOAS's own split (`AoasBinder.java` is a shell over `AoasServer`),
in the opposite direction.

The class references native via four `native` methods, and therefore needs
`System.loadLibrary("matrix_player_android")` in a static block even though the
library is already mapped: NativeActivity `dlopen()`s it from native code,
which never registers it with the JVM for symbol lookup (the same rule
app_shell's CLAUDE.md records for `AppShellActivity` subclasses).

Native finds the class **through the activity's classloader**, never
`FindClass()`: the glue thread's default loader is the system one, which cannot
see app classes. `matrixAoasSetApp(android_app*)` (called from
`android/src/main.cc` before `create()`) hands over the `android_app*`; the
first JNI use walks `activity.getClassLoader().loadClass(...)` and caches a
global ref plus the method ids.

## The lifecycle, and the two server rules that shape it

1. **`acquire()` refuses while anyone owns the device — including the current
   owner.** There is no re-acquire. So *every* format change and every
   `flush()` goes release → acquire. The AIDL doc's silent-handover promise
   covers this: an identical format leaves the isochronous stream untouched,
   and a different one re-locks the clock, which the design accepts rather
   than hides (AOAS never resamples).

2. **The producer may not move `readPos`.** `ShmRing` is symmetric code but the
   indices have owners: the client owns `writePos`, AOAS owns `readPos`. A
   client-side `clear()` would corrupt the consumer's index, and there is no
   legal way to discard buffered audio from the producer side. Therefore:
   - `stop()` = `release()` — AOAS discards the ring and pads silence, so Stop
     is immediate while the stream itself stays open (the whole point of AOAS).
   - `flush()` = `release()` + `acquire(same format)` — the silent self-handover
     used as a flush. This is what `PlayerWindow` calls at a manual Next, where
     the USB path clears its own ring: without it the previous track's tail
     would drain into the new track's start.

`configure()` acquires (so `ERR_FORMAT_UNSUPPORTED` surfaces at configure, with
`lastError()` carrying it into `audioNotice_`); `start()` re-acquires only if
`stop()` released meanwhile; `close()` releases but leaves the service bound —
rebinding costs seconds, and a started AOAS keeps running on its own anyway.

## Bit-exactness

The ring carries **wire bytes**: `frameBytes = channels × subslotBytes`, where
`activeFormat()`'s subslot may be wider than requested (24-bit in a 4-byte
subslot). The client packs with the same primitives `UsbAudioDriver` uses —
`ae::usbpack::packInt32Dyn`/`storeLEDyn` and `ae::wireScaleNative`, dithering
the float path at 16-bit exactly as the driver does (`DitherLCG`, no noise
shaping). The startup fade is omitted: the driver fades per `configure()`
because its stream restarts; an AOAS relay does not, and a ramp on every track
would be an alteration. The 4-byte subslot fast path writes the caller's int32
buffer into the ring unconverted, which is the bit-perfect claim the
signal-chain page makes. `strictBitperfect` is accepted: the relay is
passthrough by construction, and a format the DAC cannot do comes back as
`ERR_FORMAT_UNSUPPORTED` from the server instead of being quietly converted.

`probeRates()` returns empty — `IAoas` exposes no rate list. `pickOutputRate`'s
fallback is 48 kHz, which is a guess, and the second `configure()` then either
works or reports the server's refusal honestly.

## What the client may not trust

The ring geometry arrives over Binder (`ParcelFileDescriptor`) and the header
was written by another process. The client `fstat`s the fd for its own mapping
length and lets `ShmRing::attach` verify magic/version — the same discipline as
the server's note 3 in `shm_ring.hh`, mirrored. `ShmRing::clear()` is never
called from this side (see rule 2 above).

## Contract files are verbatim copies

- `third_party/aoas/shm_ring.hh` — copied from AOAS `native/shm_ring.hh`.
- `android/app/src/main/aidl/io/nava/aoas/*.aidl` — copied from AOAS `aidl/`,
  which AOAS places at its repo root precisely so clients can compile against
  it. Both must stay byte-identical to the originals; `attach()`'s magic and
  version check catches a drifted copy only at runtime, so keep them in sync by
  hand when AOAS's contract changes.

## Signing

`io.nava.aoas.BIND_AOAS` is signature-level: Matrix Player can bind AOAS only
when both APKs carry the same signing certificate. Two debug builds on one
machine share `~/.android/debug.keystore`, so debug + debug binds; a release
build signed with `bruno.jks` will not bind a debug-signed AOAS. The manifest
also declares `<queries>` for `io.nava.aoas` — without it, Android 11+ package
visibility makes `bindService` fail as if the service did not exist.

## Device identity

`getActiveDeviceKey()` returns `"aoas"` for this backend. The DAC behind the
relay is not addressable by VID:PID through `IAoas` (only `deviceInfo()`, a
human-readable string), and AutoEQ assignments are keyed by *output* — the
same one-slot model `"alsa"` and `"jack"` use. If AOAS ever exposes
descriptors through the contract, the key should become VID:PID, because the
EQ profile follows the DAC's drivers, not the relay.
