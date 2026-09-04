// The Android half of media_session.hh: the foreground service, the
// MediaSession, the audio focus and the wake lock, reached through
// io.nava.matrixplayer.MediaSessionBridge.
//
// This file holds no policy. It resolves the Java class the same way
// aoas_output.cc does, converts strings, throttles the position updates, and
// hands transport presses back to whoever registered a handler. Every decision
// about what a press MEANS is PlayerWindow's.
//
// Compiled only by android/CMakeLists.txt.
#include "media_session_android.hh"

#include "media_session.hh"

#include <android/log.h>
#include <jni.h>

#include <mutex>
#include <string>

#include "jni_util.hh"   // framework/vk_canvas/platform/android/jni_util.hh

#define LOG_TAG "MatrixPlayback"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

android_app* g_app = nullptr;
jclass       g_cls = nullptr;
jmethodID    mBegin = nullptr, mUpdate = nullptr, mEnd = nullptr;

// The handler is set on the app thread at create() and called from Android's
// main thread, so it needs a lock of its own — unlike everything else here,
// which only ever runs on the app thread.
std::mutex                                     g_handlerMu;
std::function<void(media_session::Command)>    g_handler;

// What the OS was last told. update() is called on every 250 ms position tick,
// and turning each of those into a Binder round trip would be four a second for
// a progress bar nobody can read that finely. Anything that is not the position
// still goes through immediately: a track change must never wait.
struct LastPushed {
    bool        valid = false;
    std::string title, artist, album, artPath, dspTag;
    int         positionMs = 0;
    int         durationMs = 0;
};
LastPushed g_last;
bool       g_active = false;

bool ensureBridge() {
    if (g_cls) return true;
    if (!g_app) return false;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return false;

    // Through the activity's CLASSLOADER, never FindClass(): a native thread's
    // default loader is the system one and cannot see this app's classes. Same
    // rule, and same reason, as aoas_output.cc's ensureClient().
    jobject act    = g_app->activity->clazz;
    jclass  actCls = env->GetObjectClass(act);
    jmethodID getLoader =
        env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getLoader ? env->CallObjectMethod(act, getLoader) : nullptr;
    if (vce::platform::jni::check_exc(env, "media getClassLoader") || !loader)
        return false;

    jclass loaderCls = env->GetObjectClass(loader);
    jmethodID loadClass = env->GetMethodID(
        loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("io.nava.matrixplayer.MediaSessionBridge");
    jclass local = (loadClass && name)
        ? (jclass)env->CallObjectMethod(loader, loadClass, name) : nullptr;
    if (vce::platform::jni::check_exc(env, "media loadClass") || !local) return false;

    g_cls = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    // The service needs a real Context to be started from, and only Java holds
    // one for itself.
    jmethodID setAct = env->GetStaticMethodID(
        g_cls, "setActivity", "(Landroid/content/Context;)V");
    if (setAct) {
        env->CallStaticVoidMethod(g_cls, setAct, act);
        vce::platform::jni::check_exc(env, "media setActivity");
    }

    bool ok = true;
#define MEDIA_METHOD(field, jname, sig)                                  \
    do {                                                                 \
        field = env->GetStaticMethodID(g_cls, jname, sig);               \
        if (!field) { LOGE("MediaSessionBridge.%s missing", jname); ok = false; } \
    } while (0)
    MEDIA_METHOD(mBegin,  "begin",  "()V");
    MEDIA_METHOD(mUpdate, "update",
                 "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
                 "Ljava/lang/String;Ljava/lang/String;JJ)V");
    MEDIA_METHOD(mEnd,    "end",    "()V");
#undef MEDIA_METHOD
    if (!ok) {
        // Leave g_cls null rather than half-initialised, so a later call tries
        // again instead of dereferencing a method id that was never resolved.
        env->DeleteGlobalRef(g_cls);
        g_cls = nullptr;
        return false;
    }
    return true;
}

// NewStringUTF takes Java's MODIFIED UTF-8, which is not UTF-8: a real U+0000
// or a 4-byte sequence (any emoji, and plenty of real track titles carry one)
// is malformed to it and aborts the VM in a CheckJNI build. app_shell's
// activity_bridge.hh states the same rule. Going through a byte array and
// letting Java's own decoder do the work is the safe conversion.
jstring toJString(JNIEnv* env, const std::string& s) {
    jbyteArray bytes = env->NewByteArray((jsize)s.size());
    if (!bytes) return nullptr;
    env->SetByteArrayRegion(bytes, 0, (jsize)s.size(),
                            reinterpret_cast<const jbyte*>(s.data()));

    jclass strCls = env->FindClass("java/lang/String");
    jmethodID ctor = env->GetMethodID(strCls, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");   // pure ASCII: always safe
    jstring out = (jstring)env->NewObject(strCls, ctor, bytes, charset);

    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(bytes);
    return out;
}

}  // namespace

void matrixMediaSessionSetApp(android_app* app) { g_app = app; }

namespace media_session {

void setCommandHandler(std::function<void(Command)> fn) {
    std::lock_guard<std::mutex> lk(g_handlerMu);
    g_handler = std::move(fn);
}

void begin() {
    if (g_active) return;
    if (!ensureBridge()) { LOGW("no MediaSessionBridge; playback will not survive backgrounding"); return; }
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return;
    env->CallStaticVoidMethod(g_cls, mBegin);
    vce::platform::jni::check_exc(env, "media begin");
    g_active = true;
    g_last   = LastPushed{};   // nothing has been told to the OS about this run
    LOGI("playback session begun");
}

void update(const NowPlaying& np) {
    if (!ensureBridge()) return;

    // The POSITION is deliberately not part of this test any more.
    //
    // It used to be, throttled to a second, so the lock screen's progress row
    // could follow the music. There is no progress row now: the session
    // publishes neither a duration nor a position, because publishing both is
    // what makes SystemUI draw a scrubber and this player does not want one.
    // With nothing drawing it, pushing the position was a Binder round trip a
    // second to move a number nobody reads.
    //
    // What survives is the DURATION, which the notification prints as a plain
    // label — so a track change still gets through here, because the duration
    // changes with it.
    const bool changed =
        !g_last.valid            || g_last.title      != np.title  ||
        g_last.artist  != np.artist  || g_last.album  != np.album  ||
        g_last.artPath != np.artPath || g_last.dspTag != np.dspTag ||
        g_last.durationMs != np.durationMs;
    if (!changed) return;

    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return;

    jstring jTitle  = toJString(env, np.title);
    jstring jArtist = toJString(env, np.artist);
    jstring jAlbum  = toJString(env, np.album);
    jstring jArt    = toJString(env, np.artPath);
    jstring jTag    = toJString(env, np.dspTag);

    env->CallStaticVoidMethod(g_cls, mUpdate, jTitle, jArtist, jAlbum, jArt, jTag,
                              (jlong)np.positionMs, (jlong)np.durationMs);
    vce::platform::jni::check_exc(env, "media update");

    env->DeleteLocalRef(jTag);
    env->DeleteLocalRef(jArt);
    env->DeleteLocalRef(jAlbum);
    env->DeleteLocalRef(jArtist);
    env->DeleteLocalRef(jTitle);

    g_last = LastPushed{ true, np.title, np.artist, np.album, np.artPath,
                         np.dspTag, np.positionMs, np.durationMs };
    g_active = true;
}

void end() {
    if (!g_active) return;
    g_active = false;
    g_last   = LastPushed{};
    if (!g_cls || !g_app) return;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return;
    env->CallStaticVoidMethod(g_cls, mEnd);
    vce::platform::jni::check_exc(env, "media end");
    LOGI("playback session ended");
}

}  // namespace media_session

// A transport press, arriving on Android's MAIN thread — a notification button,
// a headset, a Bluetooth remote, or a loss of audio focus. The handler is
// PlayerWindow's, and it does not act here: it posts an AppEvent so the app
// thread answers, on the same road AoasClient's ownership callback travels.
extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_MediaSessionBridge_nativeOnTransportCommand(
        JNIEnv*, jclass, jint command) {
    media_session::Command cmd;
    switch (command) {
        case 0: cmd = media_session::Command::Stop; break;
        case 1: cmd = media_session::Command::Next; break;
        case 2: cmd = media_session::Command::Prev; break;
        case 3: cmd = media_session::Command::Play; break;
        default:
            LOGW("unknown transport command %d", command);
            return;
    }
    std::function<void(media_session::Command)> fn;
    {
        std::lock_guard<std::mutex> lk(g_handlerMu);
        fn = g_handler;
    }
    if (fn) fn(cmd);
}
