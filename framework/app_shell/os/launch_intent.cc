#include "launch_intent.hh"

#include <jni.h>

#include "jni_util.hh"  // framework/vk_canvas/platform/android/jni_util.hh

using vce::platform::jni::env_for;
using vce::platform::jni::check_exc;

std::string read_string_extra(android_app* app, const char* key,
                              const std::string& fallback) {
    JNIEnv* env = env_for(app);
    if (!env) return fallback;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    // activity.getIntent()
    jmethodID get_intent = env->GetMethodID(act_cls, "getIntent", "()Landroid/content/Intent;");
    if (check_exc(env, "GetMethodID(getIntent)") || !get_intent) {
        env->DeleteLocalRef(act_cls);
        return fallback;
    }
    jobject intent = env->CallObjectMethod(activity, get_intent);
    if (check_exc(env, "getIntent") || !intent) {
        env->DeleteLocalRef(act_cls);
        return fallback;
    }

    // intent.getStringExtra(key)
    jclass    intent_cls = env->GetObjectClass(intent);
    jmethodID get_extra  = env->GetMethodID(intent_cls, "getStringExtra",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
    if (check_exc(env, "GetMethodID(getStringExtra)") || !get_extra) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(intent);
        env->DeleteLocalRef(act_cls);
        return fallback;
    }
    jstring jkey  = env->NewStringUTF(key);
    jstring value = static_cast<jstring>(env->CallObjectMethod(intent, get_extra, jkey));
    env->DeleteLocalRef(jkey);
    bool exc = check_exc(env, "getStringExtra");

    std::string result = fallback;
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
