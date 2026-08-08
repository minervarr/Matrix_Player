#include "storage_permission.hh"

#include <jni.h>
#include <string>

#include "jni_util.hh"  // framework/vk_canvas/platform/android/jni_util.hh

using vce::platform::jni::env_for;
using vce::platform::jni::check_exc;

void request_all_files_access(android_app* app) {
    JNIEnv* env = env_for(app);
    if (!env) return;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    // activity.getPackageName()
    jmethodID get_pkg = env->GetMethodID(act_cls, "getPackageName", "()Ljava/lang/String;");
    if (check_exc(env, "GetMethodID(getPackageName)") || !get_pkg) {
        env->DeleteLocalRef(act_cls);
        return;
    }
    jstring pkg_name = static_cast<jstring>(env->CallObjectMethod(activity, get_pkg));
    if (check_exc(env, "getPackageName") || !pkg_name) {
        env->DeleteLocalRef(act_cls);
        return;
    }
    const char* pkg_chars = env->GetStringUTFChars(pkg_name, nullptr);
    std::string uri_str = std::string("package:") + pkg_chars;
    env->ReleaseStringUTFChars(pkg_name, pkg_chars);
    env->DeleteLocalRef(pkg_name);

    // Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION (static String
    // field, API 30+ — absent on older platforms, degrades to a no-op).
    jclass settings_cls = env->FindClass("android/provider/Settings");
    if (check_exc(env, "FindClass(Settings)") || !settings_cls) {
        env->DeleteLocalRef(act_cls);
        return;
    }
    jfieldID action_field = env->GetStaticFieldID(
        settings_cls, "ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION", "Ljava/lang/String;");
    if (check_exc(env, "GetStaticFieldID(ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION)") ||
        !action_field) {
        env->DeleteLocalRef(settings_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jstring action = static_cast<jstring>(env->GetStaticObjectField(settings_cls, action_field));
    env->DeleteLocalRef(settings_cls);
    if (!action) {
        env->DeleteLocalRef(act_cls);
        return;
    }

    // new Intent(String action)
    jclass intent_cls = env->FindClass("android/content/Intent");
    if (check_exc(env, "FindClass(Intent)") || !intent_cls) {
        env->DeleteLocalRef(action);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jmethodID intent_ctor = env->GetMethodID(intent_cls, "<init>", "(Ljava/lang/String;)V");
    if (check_exc(env, "GetMethodID(Intent<init>)") || !intent_ctor) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(action);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jobject intent = env->NewObject(intent_cls, intent_ctor, action);
    env->DeleteLocalRef(action);
    if (check_exc(env, "new Intent") || !intent) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }

    // Uri.parse("package:" + packageName), intent.setData(uri)
    jclass uri_cls = env->FindClass("android/net/Uri");
    if (!check_exc(env, "FindClass(Uri)") && uri_cls) {
        jmethodID uri_parse = env->GetStaticMethodID(uri_cls, "parse",
                                                      "(Ljava/lang/String;)Landroid/net/Uri;");
        if (!check_exc(env, "GetStaticMethodID(Uri.parse)") && uri_parse) {
            jstring uri_jstr = env->NewStringUTF(uri_str.c_str());
            jobject uri = env->CallStaticObjectMethod(uri_cls, uri_parse, uri_jstr);
            env->DeleteLocalRef(uri_jstr);
            if (!check_exc(env, "Uri.parse") && uri) {
                jmethodID set_data = env->GetMethodID(
                    intent_cls, "setData", "(Landroid/net/Uri;)Landroid/content/Intent;");
                if (!check_exc(env, "GetMethodID(setData)") && set_data) {
                    jobject ret = env->CallObjectMethod(intent, set_data, uri);
                    check_exc(env, "setData");
                    if (ret) env->DeleteLocalRef(ret);
                }
                env->DeleteLocalRef(uri);
            }
        }
        env->DeleteLocalRef(uri_cls);
    }

    // activity.startActivity(intent)
    jmethodID start_activity = env->GetMethodID(act_cls, "startActivity",
                                                "(Landroid/content/Intent;)V");
    if (!check_exc(env, "GetMethodID(startActivity)") && start_activity) {
        env->CallVoidMethod(activity, start_activity, intent);
        check_exc(env, "startActivity");
    }

    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(intent_cls);
    env->DeleteLocalRef(act_cls);
}

bool has_all_files_access(android_app* app) {
    JNIEnv* env = env_for(app);
    if (!env) return false;

    jclass env_cls = env->FindClass("android/os/Environment");
    if (check_exc(env, "FindClass(Environment)") || !env_cls) return false;

    jmethodID is_manager = env->GetStaticMethodID(env_cls, "isExternalStorageManager", "()Z");
    if (check_exc(env, "GetStaticMethodID(isExternalStorageManager)") || !is_manager) {
        // API < 30 — the method doesn't exist; nothing to report.
        env->DeleteLocalRef(env_cls);
        return false;
    }
    jboolean result = env->CallStaticBooleanMethod(env_cls, is_manager);
    check_exc(env, "isExternalStorageManager");
    env->DeleteLocalRef(env_cls);
    return result == JNI_TRUE;
}

void show_folder_picker_hint(android_app* app) {
    JNIEnv* env = env_for(app);
    if (!env) return;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    jclass intent_cls = env->FindClass("android/content/Intent");
    if (check_exc(env, "FindClass(Intent)") || !intent_cls) {
        env->DeleteLocalRef(act_cls);
        return;
    }

    jfieldID action_field = env->GetStaticFieldID(intent_cls, "ACTION_OPEN_DOCUMENT_TREE",
                                                  "Ljava/lang/String;");
    if (check_exc(env, "GetStaticFieldID(ACTION_OPEN_DOCUMENT_TREE)") || !action_field) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jstring action = static_cast<jstring>(env->GetStaticObjectField(intent_cls, action_field));
    if (!action) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }

    jmethodID intent_ctor = env->GetMethodID(intent_cls, "<init>", "(Ljava/lang/String;)V");
    if (check_exc(env, "GetMethodID(Intent<init>)") || !intent_ctor) {
        env->DeleteLocalRef(action);
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }
    jobject intent = env->NewObject(intent_cls, intent_ctor, action);
    env->DeleteLocalRef(action);
    if (check_exc(env, "new Intent") || !intent) {
        env->DeleteLocalRef(intent_cls);
        env->DeleteLocalRef(act_cls);
        return;
    }

    jmethodID start_activity = env->GetMethodID(act_cls, "startActivity",
                                                "(Landroid/content/Intent;)V");
    if (!check_exc(env, "GetMethodID(startActivity)") && start_activity) {
        env->CallVoidMethod(activity, start_activity, intent);
        check_exc(env, "startActivity");
    }

    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(intent_cls);
    env->DeleteLocalRef(act_cls);
}
