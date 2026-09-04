// The Android half of bt_codec.hh, over io.nava.matrixplayer.BluetoothCodecManager.
//
// No policy lives here. This file resolves the Java class the same way
// aoas_output.cc and media_session_android.cc do, converts types, and hands
// callbacks back to whoever registered one. What a saved configuration IS, and
// when to apply it, is PlayerWindow's.
//
// Compiled only by android/CMakeLists.txt.
#include "bt_codec_android.hh"

#include <cstdlib>

#include "bt_codec.hh"

#include <android/log.h>
#include <jni.h>

#include <mutex>

#include "jni_util.hh"   // framework/vk_canvas/platform/android/jni_util.hh

#define LOG_TAG "MatrixBtCodec"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

android_app* g_app = nullptr;
jclass       g_cls = nullptr;
jmethodID    mStart = nullptr, mCapability = nullptr, mConnectedMac = nullptr,
             mConnectedName = nullptr, mActiveConfig = nullptr, mApply = nullptr,
             mPaired = nullptr, mHasAssoc = nullptr, mRequestAssoc = nullptr,
             mAdbCommand = nullptr, mSelectable = nullptr;

// Set on the app thread, called from Android's main thread.
std::mutex g_mu;
std::function<void(const bt_codec::Device&)>                        g_onConnected;
std::function<void(const std::string&, bool, int)>                  g_onApplied;
std::function<void(const std::string&)>                             g_onGone;

std::string fromJString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return out;
}

bool ensureClass() {
    if (g_cls) return true;
    if (!g_app) return false;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return false;

    // The activity's CLASSLOADER, never FindClass(): a native thread's default
    // loader is the system one and cannot see this app's classes.
    jobject act    = g_app->activity->clazz;
    jclass  actCls = env->GetObjectClass(act);
    jmethodID getLoader =
        env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader = getLoader ? env->CallObjectMethod(act, getLoader) : nullptr;
    if (vce::platform::jni::check_exc(env, "btcodec getClassLoader") || !loader) return false;

    jclass loaderCls = env->GetObjectClass(loader);
    jmethodID loadClass = env->GetMethodID(
        loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = env->NewStringUTF("io.nava.matrixplayer.BluetoothCodecManager");
    jclass local = (loadClass && name)
        ? (jclass)env->CallObjectMethod(loader, loadClass, name) : nullptr;
    if (vce::platform::jni::check_exc(env, "btcodec loadClass") || !local) return false;

    g_cls = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);

    jmethodID setAct = env->GetStaticMethodID(
        g_cls, "setActivity", "(Landroid/content/Context;)V");
    if (setAct) {
        env->CallStaticVoidMethod(g_cls, setAct, act);
        vce::platform::jni::check_exc(env, "btcodec setActivity");
    }

    bool ok = true;
#define BT_METHOD(field, jname, sig)                                     \
    do {                                                                 \
        field = env->GetStaticMethodID(g_cls, jname, sig);               \
        if (!field) { LOGE("BluetoothCodecManager.%s missing", jname); ok = false; } \
    } while (0)
    BT_METHOD(mStart,         "start",           "()V");
    BT_METHOD(mCapability,    "capability",      "()I");
    BT_METHOD(mConnectedMac,  "connectedMac",    "()Ljava/lang/String;");
    BT_METHOD(mConnectedName, "connectedName",   "()Ljava/lang/String;");
    BT_METHOD(mActiveConfig,  "activeConfig",    "()[I");
    BT_METHOD(mApply,         "apply",           "(Ljava/lang/String;IIIII)Z");
    BT_METHOD(mPaired,        "pairedDevices",   "()[Ljava/lang/String;");
    BT_METHOD(mHasAssoc,      "hasAssociation",  "(Ljava/lang/String;)Z");
    BT_METHOD(mRequestAssoc,  "requestAssociation", "(Ljava/lang/String;)V");
    BT_METHOD(mAdbCommand,    "adbGrantCommand", "()Ljava/lang/String;");
    BT_METHOD(mSelectable,    "selectableCodecs", "()Ljava/lang/String;");
#undef BT_METHOD
    if (!ok) {
        env->DeleteGlobalRef(g_cls);
        g_cls = nullptr;
        return false;
    }
    return true;
}

}  // namespace

void matrixBtCodecSetApp(android_app* app) { g_app = app; }

namespace bt_codec {

void start() {
    if (!ensureClass()) { LOGW("no BluetoothCodecManager; codec control unavailable"); return; }
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return;
    env->CallStaticVoidMethod(g_cls, mStart);
    vce::platform::jni::check_exc(env, "btcodec start");
}

Capability capability() {
    if (!ensureClass()) return Capability::Unavailable;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return Capability::Unavailable;
    const jint c = env->CallStaticIntMethod(g_cls, mCapability);
    if (vce::platform::jni::check_exc(env, "btcodec capability")) return Capability::Unavailable;
    switch (c) {
    case 2:  return Capability::Writable;
    case 1:  return Capability::ReadOnly;
    default: return Capability::Unavailable;
    }
}

Device connectedDevice() {
    Device d;
    if (!ensureClass()) return d;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return d;
    jstring mac = (jstring)env->CallStaticObjectMethod(g_cls, mConnectedMac);
    if (vce::platform::jni::check_exc(env, "btcodec connectedMac")) return d;
    d.mac = fromJString(env, mac);
    if (mac) env->DeleteLocalRef(mac);
    if (d.mac.empty()) return d;
    jstring nm = (jstring)env->CallStaticObjectMethod(g_cls, mConnectedName);
    if (!vce::platform::jni::check_exc(env, "btcodec connectedName")) {
        d.name = fromJString(env, nm);
        if (nm) env->DeleteLocalRef(nm);
    }
    if (d.name.empty()) d.name = d.mac;
    return d;
}

Config activeConfig() {
    Config c;
    if (!ensureClass()) return c;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return c;
    jintArray arr = (jintArray)env->CallStaticObjectMethod(g_cls, mActiveConfig);
    if (vce::platform::jni::check_exc(env, "btcodec activeConfig") || !arr) return c;
    if (env->GetArrayLength(arr) >= 5) {
        jint v[5] = {0, 0, 0, 0, 0};
        env->GetIntArrayRegion(arr, 0, 5, v);
        c.codec       = v[0];
        c.sampleRate  = v[1];
        c.bits        = v[2];
        c.channelMode = v[3];
        c.ldacQuality = v[4];
    }
    env->DeleteLocalRef(arr);
    return c;
}

// "id\tname" per line, straight from the stack. Parsed rather than mapped: the
// point of asking is that this side does not have the table.
std::vector<CodecOption> selectableCodecs() {
    std::vector<CodecOption> out;
    if (!ensureClass()) return out;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return out;

    jstring js = (jstring)env->CallStaticObjectMethod(g_cls, mSelectable);
    if (vce::platform::jni::check_exc(env, "btcodec selectableCodecs") || !js) return out;
    const std::string blob = fromJString(env, js);
    env->DeleteLocalRef(js);

    size_t pos = 0;
    while (pos < blob.size()) {
        size_t nl = blob.find('\n', pos);
        if (nl == std::string::npos) nl = blob.size();
        const std::string line = blob.substr(pos, nl - pos);
        pos = nl + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        CodecOption o;
        o.id   = std::atoi(line.substr(0, tab).c_str());
        o.name = line.substr(tab + 1);
        if (!o.name.empty()) out.push_back(std::move(o));
    }
    return out;
}

bool apply(const std::string& mac, const Config& c) {
    if (!ensureClass() || mac.empty() || !c.valid()) return false;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return false;
    jstring jmac = env->NewStringUTF(mac.c_str());   // a MAC is pure ASCII
    const jboolean ok = env->CallStaticBooleanMethod(
        g_cls, mApply, jmac, (jint)c.codec, (jint)c.sampleRate, (jint)c.bits,
        (jint)c.channelMode, (jint)c.ldacQuality);
    env->DeleteLocalRef(jmac);
    if (vce::platform::jni::check_exc(env, "btcodec apply")) return false;
    LOGI("requested %s on %s -> %s", summary(c).c_str(), mac.c_str(),
         ok ? "sent" : "refused");
    return ok == JNI_TRUE;
}

std::vector<Device> pairedDevices() {
    std::vector<Device> out;
    if (!ensureClass()) return out;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return out;
    jobjectArray arr = (jobjectArray)env->CallStaticObjectMethod(g_cls, mPaired);
    if (vce::platform::jni::check_exc(env, "btcodec pairedDevices") || !arr) return out;
    // Flattened as {mac, name, mac, name, ...}: one array beats a parallel Java
    // type that would have to be kept in step with this struct.
    const jsize n = env->GetArrayLength(arr);
    for (jsize i = 0; i + 1 < n; i += 2) {
        jstring jm = (jstring)env->GetObjectArrayElement(arr, i);
        jstring jn = (jstring)env->GetObjectArrayElement(arr, i + 1);
        Device d;
        d.mac  = fromJString(env, jm);
        d.name = fromJString(env, jn);
        if (d.name.empty()) d.name = d.mac;
        if (!d.mac.empty()) out.push_back(std::move(d));
        if (jm) env->DeleteLocalRef(jm);
        if (jn) env->DeleteLocalRef(jn);
    }
    env->DeleteLocalRef(arr);
    return out;
}

bool hasAssociation(const std::string& mac) {
    if (!ensureClass() || mac.empty()) return false;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return false;
    jstring jmac = env->NewStringUTF(mac.c_str());
    const jboolean ok = env->CallStaticBooleanMethod(g_cls, mHasAssoc, jmac);
    env->DeleteLocalRef(jmac);
    if (vce::platform::jni::check_exc(env, "btcodec hasAssociation")) return false;
    return ok == JNI_TRUE;
}

void requestAssociation(const std::string& mac) {
    if (!ensureClass() || mac.empty()) return;
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return;
    jstring jmac = env->NewStringUTF(mac.c_str());
    env->CallStaticVoidMethod(g_cls, mRequestAssoc, jmac);
    env->DeleteLocalRef(jmac);
    vce::platform::jni::check_exc(env, "btcodec requestAssociation");
}

std::string adbGrantCommand() {
    if (!ensureClass()) return {};
    JNIEnv* env = vce::platform::jni::env_for(g_app);
    if (!env) return {};
    jstring s = (jstring)env->CallStaticObjectMethod(g_cls, mAdbCommand);
    if (vce::platform::jni::check_exc(env, "btcodec adbGrantCommand")) return {};
    std::string out = fromJString(env, s);
    if (s) env->DeleteLocalRef(s);
    return out;
}

void setDeviceConnectedHandler(std::function<void(const Device&)> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_onConnected = std::move(fn);
}
void setAppliedHandler(std::function<void(const std::string&, bool, int)> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_onApplied = std::move(fn);
}
void setDeviceGoneHandler(std::function<void(const std::string&)> fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_onGone = std::move(fn);
}

}  // namespace bt_codec

// ── Callbacks, all on Android's MAIN thread ─────────────────────────────────
//
// Each one copies the handler out under the lock and calls it outside, so a
// handler that takes its time cannot hold the lock against the app thread
// re-registering.

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_BluetoothCodecManager_nativeOnA2dpReady(
        JNIEnv* env, jclass, jstring mac, jstring name) {
    bt_codec::Device d;
    d.mac  = fromJString(env, mac);
    d.name = fromJString(env, name);
    if (d.name.empty()) d.name = d.mac;
    LOGI("A2DP ready: %s (%s)", d.name.c_str(), d.mac.c_str());

    std::function<void(const bt_codec::Device&)> fn;
    { std::lock_guard<std::mutex> lk(g_mu); fn = g_onConnected; }
    if (fn) fn(d);
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_BluetoothCodecManager_nativeOnA2dpGone(
        JNIEnv* env, jclass, jstring mac) {
    const std::string m = fromJString(env, mac);
    std::function<void(const std::string&)> fn;
    { std::lock_guard<std::mutex> lk(g_mu); fn = g_onGone; }
    if (fn) fn(m);
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_BluetoothCodecManager_nativeOnCodecVerified(
        JNIEnv* env, jclass, jstring mac, jboolean ok, jint actualCodec) {
    const std::string m = fromJString(env, mac);
    LOGI("codec verify on %s: %s (running %s)", m.c_str(),
         ok ? "applied" : "NOT applied", bt_codec::codecName(actualCodec).c_str());

    std::function<void(const std::string&, bool, int)> fn;
    { std::lock_guard<std::mutex> lk(g_mu); fn = g_onApplied; }
    if (fn) fn(m, ok == JNI_TRUE, (int)actualCodec);
}

extern "C" JNIEXPORT void JNICALL
Java_io_nava_matrixplayer_BluetoothCodecManager_nativeOnAssociationChanged(
        JNIEnv* env, jclass, jstring mac, jboolean associated) {
    const std::string m = fromJString(env, mac);
    LOGI("companion association for %s: %s", m.c_str(), associated ? "granted" : "refused");
    // Treated as a connection: the association is what makes apply() legal, so
    // the moment it lands the saved configuration should be tried again.
    if (!associated) return;
    bt_codec::Device d;
    d.mac  = m;
    d.name = m;
    std::function<void(const bt_codec::Device&)> fn;
    { std::lock_guard<std::mutex> lk(g_mu); fn = g_onConnected; }
    if (fn) fn(d);
}
