#include "aoas_output.hh"

#include <jni.h>
#include <android/log.h>
#include <android/sharedmem.h>

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "jni_util.hh"               // vce::platform::jni::env_for/check_exc
#include "usb_pack.h"                // ae::usbpack — the SAME packing the USB driver uses
#include "core/dsp/round.h"          // ae::roundHalfEven

#define LOG_TAG "matrix_player"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── The Java seam ────────────────────────────────────────────────────────────
//
// Java exists because the IAoas contract is an AIDL interface generated with
// the Java backend (AOAS's CLAUDE.md, open question 1). Everything with an
// opinion about audio is C++; each Java method here is called on its first
// line and does nothing but what Binder requires. The class is found through
// the activity's classloader — FindClass() on a native thread consults the
// SYSTEM loader, which cannot see app classes.

namespace {

android_app* g_app = nullptr;

jclass    g_cls = nullptr;   // io.nava.matrixplayer.AoasClient, global ref
jmethodID mBind, mIsBound, mAcquire, mRelease, mFlush, mActiveFormat, mPendingMs,
          mDeviceInfo;

// --- down-call state (Java writes, C++ reads) --------------------------------

std::mutex              g_fdMu;
std::condition_variable g_fdCv;
int                     g_fd = -1;          // the ring fd, handed over inside acquire()

std::atomic<bool> g_lost{false};            // onOwnershipLost arrived
std::atomic<int>  g_lostReason{0};          // IAoasClient.REASON_*
std::atomic<bool> g_serverGone{false};      // onServiceDisconnected while it mattered

// AOAS's IAoas result codes (IAoas.OK == 0), described in the driver's own
// terms. A code the screen can quote beats a bare number; the two >99 codes
// are AoasClient's own (ERR_NOT_BOUND / ERR_DEAD).
std::string describeResult(int res) {
    switch (res) {
    case 1:   return "another app owns the DAC \xE2\x80\x94 AOAS never takes it by force";
    case 2:   return "no DAC is connected, or Android has not granted the USB permission";
    case 3:   return "the DAC cannot run this format, and the relay never converts";
    case 4:   return "the shared ring could not be created";
    case 100: return "AOAS service is not connected";
    case 101: return "AOAS service died during the request";
    default:  return "AOAS refused the request (code " + std::to_string(res) + ")";
    }
}

std::string lostReasonText(int reason) {
    switch (reason) {
    case 1:  return "the DAC was handed over by AOAS (user disconnect)";
    case 2:  return "the DAC was unplugged";
    case 3:  return "the AOAS server stopped";
    default: return "AOAS took the DAC away (reason " + std::to_string(reason) + ")";
    }
}

jstring toJString(JNIEnv* env, const std::string& utf8) {
    return env->NewStringUTF(utf8.c_str());
}

std::string fromJString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

bool ensureClient() {
    if (g_cls) return true;
    if (!g_app) return false;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return false;

    jobject act    = g_app->activity->clazz;
    jclass  actCls = env->GetObjectClass(act);
    jmethodID getLoader =
        env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader =
        getLoader ? env->CallObjectMethod(act, getLoader) : nullptr;
    if (vce::platform::jni::check_exc(env, "aoas getClassLoader") || !loader)
        return false;

    jclass loaderCls = env->GetObjectClass(loader);
    jmethodID loadClass = env->GetMethodID(
        loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("io.nava.matrixplayer.AoasClient");
    jclass local = (loadClass && name)
        ? (jclass)env->CallObjectMethod(loader, loadClass, name) : nullptr;
    if (vce::platform::jni::check_exc(env, "aoas loadClass") || !local) return false;

    g_cls = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    // bindService needs a real Context, and only Java has one for itself.
    jmethodID setAct = env->GetStaticMethodID(
        g_cls, "setActivity", "(Landroid/content/Context;)V");
    if (setAct) {
        env->CallStaticVoidMethod(g_cls, setAct, act);
        vce::platform::jni::check_exc(env, "aoas setActivity");
    }

    bool ok = true;
#define AOAS_METHOD(field, name, sig)                          \
    do {                                                       \
        field = env->GetStaticMethodID(g_cls, name, sig);      \
        if (!field) { LOGE("AoasClient.%s missing", name); ok = false; } \
    } while (0)
    AOAS_METHOD(mBind,         "bind",               "()Z");
    AOAS_METHOD(mIsBound,      "isBound",            "()Z");
    AOAS_METHOD(mAcquire,      "acquire",            "(IIII)I");
    AOAS_METHOD(mRelease,      "release",            "()V");
    AOAS_METHOD(mFlush,        "flush",              "()Z");
    AOAS_METHOD(mActiveFormat, "activeFormat",       "()[I");
    AOAS_METHOD(mPendingMs,    "pendingPlaybackMs",  "()I");
    AOAS_METHOD(mDeviceInfo,   "deviceInfo",         "()Ljava/lang/String;");
#undef AOAS_METHOD
    if (!ok) {
        // One global ref leaked, once, in a process that failed to start the
        // backend at all. Resetting g_cls keeps a half-initialised seam from
        // being used.
        g_cls = nullptr;
        return false;
    }
    return true;
}

// --- down-calls (implemented in this file, declared in AoasClient.java) ------

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_AoasClient_nativeOnRingFd(JNIEnv*, jclass, jint fd) {
    std::lock_guard<std::mutex> lk(g_fdMu);
    g_fd = fd;
    g_fdCv.notify_all();
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_AoasClient_nativeOnOwnershipLost(JNIEnv*, jclass,
                                                           jint reason) {
    LOGW("AOAS: ownership lost (reason %d)", reason);
    g_lostReason.store(reason, std::memory_order_release);
    g_lost.store(true, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_AoasClient_nativeOnServiceConnected(JNIEnv*, jclass) {
    g_serverGone.store(false, std::memory_order_release);
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_AoasClient_nativeOnServiceDisconnected(JNIEnv*,
                                                                 jclass) {
    // AOAS's process died; it cannot call onOwnershipLost anymore, so this is
    // the loss notification while we believed we owned the device. hasFaulted()
    // turns it into a stop as soon as the timer asks.
    LOGE("AOAS: service disconnected");
    g_serverGone.store(true, std::memory_order_release);
}

void matrixAoasSetApp(android_app* app) { g_app = app; }

// ── AoasOutput ───────────────────────────────────────────────────────────────

AoasOutput::~AoasOutput() {
    releaseOwned();
    releaseMapping();
}

bool AoasOutput::configure(int rate, int channels, int bitDepth,
                           bool strictBitperfect) {
    // Passthrough by construction — there is nothing to refuse here. A format
    // the DAC cannot do comes back as ERR_FORMAT_UNSUPPORTED from the server
    // instead of being quietly converted; that IS the strict answer.
    (void)strictBitperfect;
    reqRate_ = rate; reqCh_ = channels; reqBits_ = bitDepth;

    if (!ensureClient()) {
        lastError_ = "AOAS: no Android activity to bind from";
        return false;
    }
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) { lastError_ = "AOAS: no JNIEnv"; return false; }

    jboolean bound = env->CallStaticBooleanMethod(g_cls, mBind);
    if (vce::platform::jni::check_exc(env, "aoas bind") || !bound) {
        lastError_ =
            "AOAS service not reachable \xE2\x80\x94 is it installed, and signed "
            "with the same key as this app?";
        return false;
    }
    // First use only: the connection is asynchronous, one wait, bounded.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!env->CallStaticBooleanMethod(g_cls, mIsBound)) {
        if (vce::platform::jni::check_exc(env, "aoas isBound")) break;
        if (std::chrono::steady_clock::now() > deadline) {
            lastError_ = "AOAS service did not connect within 3 s";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return acquireOwned(rate, channels, bitDepth);
}

bool AoasOutput::acquireOwned(int rate, int channels, int bits) {
    // The server refuses a second acquire outright — even from the current
    // owner — so EVERY (re)acquire starts from release. The AIDL doc's
    // silent-handover promise is what makes that affordable: an identical
    // format leaves the isochronous stream untouched; a different one re-locks
    // the clock, which the design accepts rather than hides.
    releaseOwned();
    if (!ensureClient()) {
        if (lastError_.empty()) lastError_ = "AOAS: no Android activity to bind from";
        return false;
    }
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) { lastError_ = "AOAS: no JNIEnv"; return false; }

    // A loss recorded against a PREVIOUS ownership must not fault this one.
    g_lost.store(false, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(g_fdMu);
        g_fd = -1;
    }
    jint res = env->CallStaticIntMethod(g_cls, mAcquire, (jint)rate,
                                        (jint)channels, (jint)bits, (jint)0);
    if (vce::platform::jni::check_exc(env, "aoas acquire")) {
        lastError_ = "AOAS: the acquire call failed";
        return false;
    }
    if (res != 0) {
        lastError_ = describeResult(res);
        LOGW("AOAS: acquire(%d Hz, %d ch, %d bit) -> %d (%s)",
             rate, channels, bits, res, lastError_.c_str());
        return false;
    }

    // acquire() in Java handed the fd over synchronously, on this thread,
    // inside the call above; the wait is for the contract, not the clock.
    int fd = -1;
    {
        std::unique_lock<std::mutex> lk(g_fdMu);
        g_fdCv.wait_for(lk, std::chrono::seconds(1), [] { return g_fd >= 0; });
        fd = g_fd;
    }
    // How big is the region? NOT fstat: an ASharedMemory fd reports st_size 0
    // on some kernels (measured on the moto g06) — the size lives in the
    // ASHMEM ioctl, which ASharedMemory_getSize reads. That is the whole
    // reason the API exists.
    const off_t region = (off_t)ASharedMemory_getSize(fd);
    if (fd < 0 || region <= (off_t)sizeof(aoas::ShmRingHeader)) {
        LOGW("AOAS: post-acquire failure: fd=%d region=%ld", fd, (long)region);
        if (fd >= 0) ::close(fd);
        releaseOwned();   // the server still owns — free it, release() is idempotent
        lastError_ = "AOAS granted the device but delivered no usable ring";
        return false;
    }
    void* base = ::mmap(nullptr, (size_t)region, PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd, 0);
    ::close(fd);   // the mapping holds its own reference to the region
    if (base == MAP_FAILED) {
        LOGW("AOAS: mmap of the %ld-byte ring failed (errno %d)",
             (long)region, errno);
        releaseOwned();
        lastError_ = "mmap of the AOAS ring failed";
        return false;
    }
    base_        = base;
    regionBytes_ = (size_t)region;
    ring_ = aoas::ShmRing::attach(base_, regionBytes_);
    if (!ring_.valid()) {
        LOGW("AOAS: ring header did not check out (magic=%08x version=%u "
             "capacity=%u frameBytes=%u)",
             ((aoas::ShmRingHeader*)base_)->magic,
             ((aoas::ShmRingHeader*)base_)->version,
             ((aoas::ShmRingHeader*)base_)->capacity,
             ((aoas::ShmRingHeader*)base_)->frameBytes);
        releaseOwned();
        lastError_ = "AOAS ring header did not check out (contract version drift?)";
        return false;
    }

    // What the DAC is ACTUALLY running. Rate/channels/bits are exact — the
    // server never converts — but the subslot may be wider than requested
    // (24-bit in a 4-byte slot), and the wire layout follows what the wire
    // carries, exactly as it does inside the USB driver.
    jintArray fmtArr = (jintArray)env->CallStaticObjectMethod(g_cls, mActiveFormat);
    jint fmt[4] = {0, 0, 0, 0};
    if (vce::platform::jni::check_exc(env, "aoas activeFormat")) {
        releaseOwned();
        lastError_ = "AOAS: the activeFormat call failed";
        return false;
    }
    if (fmtArr) {
        if (env->GetArrayLength(fmtArr) >= 4)
            env->GetIntArrayRegion(fmtArr, 0, 4, fmt);
        env->DeleteLocalRef(fmtArr);
    }
    if (fmt[0] != rate || fmt[1] != channels || fmt[2] != bits || fmt[3] <= 0) {
        LOGW("AOAS: format mismatch — server running %d Hz / %d ch / %d bit / "
             "%d-byte subslot, asked for %d Hz / %d ch / %d bit",
             fmt[0], fmt[1], fmt[2], fmt[3], rate, channels, bits);
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "AOAS settled on %d Hz / %d ch / %d bit / %d-byte subslot "
                 "instead of %d Hz / %d ch / %d bit",
                 fmt[0], fmt[1], fmt[2], fmt[3], rate, channels, bits);
        lastError_ = buf;
        releaseOwned();
        return false;
    }
    cfgRate_ = fmt[0]; cfgCh_ = fmt[1]; cfgBits_ = fmt[2]; subslot_ = fmt[3];
    deviceInfo_ = [&] {
        jstring s = (jstring)env->CallStaticObjectMethod(g_cls, mDeviceInfo);
        if (vce::platform::jni::check_exc(env, "aoas deviceInfo")) return std::string{};
        return fromJString(env, s);
    }();

    owned_ = true;
    LOGI("AOAS: ownership acquired \xE2\x80\x94 %d Hz / %d ch / %d bit in %d-byte "
         "subslots, %zu-byte ring, DAC: %s",
         cfgRate_, cfgCh_, cfgBits_, subslot_, ring_.capacity(),
         deviceInfo_.c_str());
    return true;
}

void AoasOutput::releaseOwned() {
    // ALWAYS tell the server, never only when owned_: this side's owned_ and
    // the server's ownerUid_ CAN disagree (a post-acquire failure, a process
    // restart while the uid still owns), and when they do, a conditional
    // release strands the DAC — every later acquire comes back ERR_BUSY
    // forever. release() is idempotent by design ("calling this without
    // owning the device does nothing"), so an unconditional release is both
    // correct and the self-heal.
    if (g_cls && g_app) {
        JNIEnv* env = vce::platform::jni::env_for(g_app);
        if (env) {
            env->CallStaticVoidMethod(g_cls, mRelease);
            vce::platform::jni::check_exc(env, "aoas release");
        }
    }
    if (owned_) {
        owned_ = false;   // first: new writes fail fast, so any in-flight write drains
        // release() discards whatever is still buffered — Stop is immediate —
        // while the isochronous stream itself stays open (the whole point of
        // AOAS). Wait out any write that already entered the ring before
        // unmapping: munmap under a concurrent memcpy is a SIGSEGV.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (ringTouches_.load(std::memory_order_acquire) > 0) {
            if (std::chrono::steady_clock::now() > deadline) {
                LOGE("AOAS: ring still in use after 50 ms; leaking one mapping "
                     "rather than unmapping under a write");
                ring_ = aoas::ShmRing{};
                base_ = nullptr;   // deliberately not munmapped
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    releaseMapping();
}

void AoasOutput::releaseMapping() {
    ring_ = aoas::ShmRing{};
    if (base_) {
        ::munmap(base_, regionBytes_);
        base_        = nullptr;
        regionBytes_ = 0;
    }
}

bool AoasOutput::start() {
    if (owned_) return true;
    return acquireOwned(reqRate_, reqCh_, reqBits_);
}

void AoasOutput::stop() {
    // Flush BEFORE releasing, and the order is the whole point: release() frees
    // the DAC but leaves the driver's three-second ring playing, so on its own
    // it makes Stop look ignored. Flushing first is what makes the silence
    // immediate; releasing after is what hands the device back to whoever wants
    // it next, instead of holding it idle.
    flush();
    releaseOwned();
}

void AoasOutput::close() {
    // Keep the service bound for the process lifetime: rebinding costs
    // seconds, and AOAS keeps running on its own anyway.
    releaseOwned();
}

void AoasOutput::flush() {
    // The producer still may not move readPos — that index belongs to AOAS —
    // so the discard has to happen server-side. It is now ONE call.
    //
    // This used to be release() → acquire(): a self-handover standing in for a
    // flush, because IAoas had no verb that meant "discard". It cost four
    // Binder round trips, a fresh ASharedMemory region, an munmap/mmap pair and
    // a format re-verify — and it did not even do the job, because the server's
    // endOwnershipLocked() never touched the DRIVER's ring, which is three
    // seconds deep. Stop and Next were late by that buffer.
    //
    // IAoas.flush() is synchronous, so when this returns the audio is really
    // gone and the caller may start writing the next track at once. The
    // mapping and the ownership are untouched, which is why no RingTouch drain
    // is needed here the way releaseOwned() needs one.
    if (!owned_) return;
    JNIEnv* env = (g_cls && g_app) ? vce::platform::jni::env_for(g_app) : nullptr;
    if (!env) return;
    const jboolean ok = env->CallStaticBooleanMethod(g_cls, mFlush);
    if (vce::platform::jni::check_exc(env, "aoas flush") || !ok) {
        // Not fatal: the tail plays on, which is what happened before this
        // call existed. Worth a line because it means Stop looked ignored.
        LOGW("AOAS: flush refused — buffered audio will still play out");
    }
}

// ── Writes ───────────────────────────────────────────────────────────────────
//
// The ring carries WIRE bytes: subslotBytes per sample, frameBytes per frame —
// the same format UsbAudioDriver packs for its own ring, packed here with the
// same primitives (usb_pack.h). No fade: the driver fades per configure()
// because its stream restarts with it; this stream does not belong to us, and
// a ramp at every track boundary would be an alteration.

namespace {
struct RingTouch {
    std::atomic<int>& c_;
    explicit RingTouch(std::atomic<int>& c) : c_(c) { c_.fetch_add(1, std::memory_order_acq_rel); }
    ~RingTouch() { c_.fetch_sub(1, std::memory_order_acq_rel); }
};
}  // namespace

bool AoasOutput::ringUsable() const {
    return owned_ && ring_.valid() && !g_lost.load(std::memory_order_acquire);
}

int AoasOutput::writeInt32(const int32_t* data, int numSamples) {
    if (numSamples <= 0) return 0;
    RingTouch touch(ringTouches_);
    if (!ringUsable()) return 0;
    const int sub           = subslot_;
    const int bytesPerFrame = sub * cfgCh_;

    // 4-byte subslot at the only gain there is (unity — no software volume
    // rides the relay): the wire format IS the caller's buffer. Same fast
    // path, same dsp_null_test-licensed reasoning, as UsbAudioDriver::writeInt32.
    if (sub == 4) {
        const size_t space   = ring_.freeSpace();
        const int    aligned = bytesPerFrame > 0
                                   ? (int)(space / bytesPerFrame) * bytesPerFrame
                                   : (int)space;
        const int toWrite = std::min(numSamples * 4, aligned);
        if (toWrite <= 0) return 0;
        return (int)ring_.write(reinterpret_cast<const uint8_t*>(data),
                                (size_t)toWrite) / 4;
    }

    const int CHUNK       = 512;
    uint8_t   convBuf[CHUNK * 4];
    int       totalConsumed = 0;
    while (totalConsumed < numSamples) {
        const size_t space        = ring_.freeSpace();
        const int    alignedBytes = bytesPerFrame > 0
                                        ? (int)(space / bytesPerFrame) * bytesPerFrame
                                        : (int)space;
        const int maxSamples = alignedBytes / sub;
        if (maxSamples <= 0) break;
        const int batch = std::min({CHUNK, numSamples - totalConsumed, maxSamples});
        // Narrower subslots truncate LSBs so the signal's MSBs survive — the
        // left-justified int32 convention audio_output.h documents, bit-exact
        // for 24-in-3 and for anything wider.
        const int outBytes = ae::usbpack::packInt32Dyn(data + totalConsumed, batch,
                                                       convBuf, sub, 1.0f);
        const int written  = (int)ring_.write(convBuf, (size_t)outBytes);
        const int samples  = written / sub;
        totalConsumed += samples;
        if (samples < batch) break;
    }
    return totalConsumed;
}

int AoasOutput::writeFloat32(const float* data, int numSamples) {
    if (numSamples <= 0) return 0;
    RingTouch touch(ringTouches_);
    if (!ringUsable()) return 0;
    const int sub           = subslot_;
    const int bytesPerFrame = sub * cfgCh_;
    int       padBits       = sub * 8 - cfgBits_;
    if (padBits < 0) padBits = 0;

    // Same quantization as UsbAudioDriver::writeFloat32: roundHalfEven (a bare
    // cast truncates toward zero, which is correlated distortion), TPDF dither
    // at 16-bit only, unsigned shift for the pad.
    const double quantScale      = ae::wireScaleNative(cfgBits_);
    const bool   ditherAndClamp16 = (cfgBits_ != 24 && cfgBits_ != 32);

    const int CHUNK       = 512;
    uint8_t   convBuf[CHUNK * 4];
    int       totalConsumed = 0;
    while (totalConsumed < numSamples) {
        const size_t space        = ring_.freeSpace();
        const int    alignedBytes = bytesPerFrame > 0
                                        ? (int)(space / bytesPerFrame) * bytesPerFrame
                                        : (int)space;
        const int maxSamples = alignedBytes / sub;
        if (maxSamples <= 0) break;
        const int batch =
            std::min({CHUNK, numSamples - totalConsumed, maxSamples});

        int outBytes = 0;
        for (int i = 0; i < batch; i++) {
            double sd = std::max(-1.0, std::min(1.0, (double)data[totalConsumed + i]));
            int32_t v;
            if (ditherAndClamp16) {
                const double scaled = sd * quantScale + dither_.nextTPDF();
                int32_t q = (int32_t)ae::roundHalfEven(scaled);
                if (q > 32767) q = 32767;
                else if (q < -32768) q = -32768;
                v = q;
            } else {
                v = (int32_t)ae::roundHalfEven(sd * quantScale);
            }
            const int32_t wire = (int32_t)((uint32_t)v << padBits);  // unsigned: no signed-shift UB
            ae::usbpack::storeLEDyn(convBuf + outBytes, wire, sub);
            outBytes += sub;
        }

        const int written = (int)ring_.write(convBuf, (size_t)outBytes);
        const int samples = written / sub;
        totalConsumed += samples;
        if (samples < batch) break;
    }
    return totalConsumed;
}

int AoasOutput::writeInt32Blocking(const int32_t* data, int numSamples,
                                   int timeoutMs) {
    int     total = 0;
    int64_t t0    = detail::monotonicMs();
    int     spins = 0;
    while (total < numSamples) {
        // A released relay is not backpressure — break immediately instead of
        // spinning out the timeout, so stop()/flush() never park the decode
        // thread behind a dead ring.
        if (!owned_) break;
        int written = writeInt32(data + total, numSamples - total);
        if (written > 0) {
            total += written;
            spins = 0;
            continue;
        }
        if ((int)(detail::monotonicMs() - t0) >= timeoutMs) break;
        if (spins < 4) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins;
    }
    return total;
}

int AoasOutput::writeFloat32Blocking(const float* data, int numSamples,
                                     int timeoutMs) {
    int     total = 0;
    int64_t t0    = detail::monotonicMs();
    int     spins = 0;
    while (total < numSamples) {
        if (!owned_) break;
        int written = writeFloat32(data + total, numSamples - total);
        if (written > 0) {
            total += written;
            spins = 0;
            continue;
        }
        if ((int)(detail::monotonicMs() - t0) >= timeoutMs) break;
        if (spins < 4) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++spins;
    }
    return total;
}

bool AoasOutput::waitForData(int minSamples, int timeoutMs) {
    const int  minBytes = minSamples * (subslot_ > 0 ? subslot_ : 2);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (ringUsable()) {
        if ((int)ring_.available() >= minBytes) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// ── Readouts ─────────────────────────────────────────────────────────────────

int AoasOutput::pendingPlaybackMs() const {
    // The server's own number: ring occupancy PLUS what is already in flight
    // at the DAC. One Binder transaction, called from onTimer at 250 ms —
    // cheap enough to be truthful rather than approximated.
    if (!owned_ || !g_cls || !g_app) return 0;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return 0;
    jint ms = env->CallStaticIntMethod(g_cls, mPendingMs);
    if (vce::platform::jni::check_exc(env, "aoas pendingPlaybackMs")) return 0;
    return ms;
}

size_t AoasOutput::ringAvailable() const {
    RingTouch touch(ringTouches_);
    return ring_.valid() ? ring_.available() : 0;
}

std::string AoasOutput::wireFormat() const {
    if (cfgBits_ <= 0 || subslot_ <= 0) return {};
    return std::to_string(cfgBits_) + "-bit in " + std::to_string(subslot_) +
           "-byte slot";
}

bool AoasOutput::hasFaulted() const {
    if (g_lost.load(std::memory_order_acquire)) return true;
    return g_serverGone.load(std::memory_order_acquire) && owned_;
}

std::string AoasOutput::currentError() const {
    if (g_lost.load(std::memory_order_acquire))
        return lostReasonText(g_lostReason.load(std::memory_order_acquire));
    if (g_serverGone.load(std::memory_order_acquire) && owned_)
        return "the AOAS service stopped";
    return lastError_;
}
