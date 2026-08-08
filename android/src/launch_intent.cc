#include "launch_intent.hh"

#include <jni.h>

#include "jni_util.hh"  // framework/vk_canvas/platform/android/jni_util.hh

using vce::platform::jni::env_for;
using vce::platform::jni::check_exc;

std::string read_scan_root_extra(android_app* app) {
    JNIEnv* env = env_for(app);
    if (!env) return kDefaultScanRoot;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    // activity.getIntent()
    jmethodID get_intent = env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;");
    if (check_exc(env, "GetMethodID(getIntent)") || !get_intent) {
        env->DeleteLocalRef(act_cls);
        return kDefaultScanRoot;
    }
    jobject intent = env->CallObjectMethod(activity, get_intent);
    if (check_exc(env, "getIntent") || !intent) {
        env->DeleteLocalRef(act_cls);
        return kDefaultScanRoot;
    }

    // intent.getStringExtra("scan_root")
    jclass    intent_cls = env->GetObjectClass(intent);
    jmethodID get_extra  = env->GetMethodID(intent_cls, "getStringExtra",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
    if (check_exc(env, "GetMethodID(getStringExtra)") || !get_extra) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(intent);
        env->DeleteLocalRef(act_cls);
        return kDefaultScanRoot;
    }
    jstring key   = env->NewStringUTF("scan_root");
    jstring value = static_cast<jstring>(env->CallObjectMethod(intent, get_extra, key));
    env->DeleteLocalRef(key);
    bool exc = check_exc(env, "getStringExtra");

    std::string result = kDefaultScanRoot;
    if (!exc && value) {
        const char* chars = env->GetStringUTFChars(value, nullptr);
        if (chars && chars[0] != '\0') result = chars;
        env->ReleaseStringUTFChars(value, chars);
        env->DeleteLocalRef(value);
    }

    env->DeleteLocalRef(intent_cls);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(act_cls);
    return result;
}
