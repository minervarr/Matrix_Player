#include "safe_area.hh"

#include <jni.h>

#include "jni_util.hh"  // framework/vk_canvas/platform/android/jni_util.hh

// android.view.DisplayCutout has no NDK/C equivalent at all — unlike sensor
// data or the display-cutout-free content rect the glue tracks for free
// (android_app::contentRect), this value genuinely only exists on the Java
// side. So this file makes the one JNI call the OS forces: not into any
// object of ours, but into the NativeActivity's own system-provided Activity
// (app->activity->clazz) — exactly the same shape jni_util.hh/fullscreen.hh
// already use for the nav-bar height query. Zero .java files, one JNI call.

using vce::platform::jni::env_for;
using vce::platform::jni::check_exc;

SafeAreaInsets query_safe_area_insets(android_app* app) {
    SafeAreaInsets insets;

    JNIEnv* env = env_for(app);
    if (!env) return insets;

    jobject activity = app->activity->clazz;
    jclass  act_cls  = env->GetObjectClass(activity);

    // activity.getWindow()
    jmethodID get_window = env->GetMethodID(act_cls, "getWindow", "()Landroid/view/Window;");
    if (check_exc(env, "GetMethodID(getWindow)") || !get_window) {
        env->DeleteLocalRef(act_cls);
        return insets;
    }
    jobject window = env->CallObjectMethod(activity, get_window);
    if (check_exc(env, "getWindow") || !window) {
        env->DeleteLocalRef(act_cls);
        return insets;
    }

    // window.getDecorView()
    jclass    win_cls   = env->GetObjectClass(window);
    jmethodID get_decor = env->GetMethodID(win_cls, "getDecorView", "()Landroid/view/View;");
    if (check_exc(env, "GetMethodID(getDecorView)") || !get_decor) {
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }
    jobject decor = env->CallObjectMethod(window, get_decor);
    if (check_exc(env, "getDecorView") || !decor) {
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }

    // decor.getRootWindowInsets() — null before the first layout pass.
    jclass    view_cls   = env->GetObjectClass(decor);
    jmethodID get_insets = env->GetMethodID(view_cls, "getRootWindowInsets",
                                            "()Landroid/view/WindowInsets;");
    if (check_exc(env, "GetMethodID(getRootWindowInsets)") || !get_insets) {
        env->DeleteLocalRef(view_cls);
        env->DeleteLocalRef(decor);
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }
    jobject window_insets = env->CallObjectMethod(decor, get_insets);
    if (check_exc(env, "getRootWindowInsets") || !window_insets) {
        env->DeleteLocalRef(view_cls);
        env->DeleteLocalRef(decor);
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }

    // windowInsets.getDisplayCutout() — API 28+; null on an older platform,
    // and legitimately null (not an error) on a device/orientation with no
    // cutout at all.
    jclass    wi_cls    = env->GetObjectClass(window_insets);
    jmethodID get_cutout = env->GetMethodID(wi_cls, "getDisplayCutout",
                                            "()Landroid/view/DisplayCutout;");
    if (check_exc(env, "GetMethodID(getDisplayCutout)") || !get_cutout) {
        env->DeleteLocalRef(wi_cls);
        env->DeleteLocalRef(window_insets);
        env->DeleteLocalRef(view_cls);
        env->DeleteLocalRef(decor);
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }
    jobject cutout = env->CallObjectMethod(window_insets, get_cutout);
    if (check_exc(env, "getDisplayCutout") || !cutout) {
        env->DeleteLocalRef(wi_cls);
        env->DeleteLocalRef(window_insets);
        env->DeleteLocalRef(view_cls);
        env->DeleteLocalRef(decor);
        env->DeleteLocalRef(win_cls);
        env->DeleteLocalRef(window);
        env->DeleteLocalRef(act_cls);
        return insets;
    }

    // cutout.getSafeInset{Top,Left,Right,Bottom}()
    jclass cutout_cls = env->GetObjectClass(cutout);

    jmethodID get_top = env->GetMethodID(cutout_cls, "getSafeInsetTop", "()I");
    if (!check_exc(env, "GetMethodID(getSafeInsetTop)") && get_top) {
        insets.top = env->CallIntMethod(cutout, get_top);
        check_exc(env, "getSafeInsetTop");
    }
    jmethodID get_left = env->GetMethodID(cutout_cls, "getSafeInsetLeft", "()I");
    if (!check_exc(env, "GetMethodID(getSafeInsetLeft)") && get_left) {
        insets.left = env->CallIntMethod(cutout, get_left);
        check_exc(env, "getSafeInsetLeft");
    }
    jmethodID get_right = env->GetMethodID(cutout_cls, "getSafeInsetRight", "()I");
    if (!check_exc(env, "GetMethodID(getSafeInsetRight)") && get_right) {
        insets.right = env->CallIntMethod(cutout, get_right);
        check_exc(env, "getSafeInsetRight");
    }
    jmethodID get_bottom = env->GetMethodID(cutout_cls, "getSafeInsetBottom", "()I");
    if (!check_exc(env, "GetMethodID(getSafeInsetBottom)") && get_bottom) {
        insets.bottom = env->CallIntMethod(cutout, get_bottom);
        check_exc(env, "getSafeInsetBottom");
    }

    env->DeleteLocalRef(cutout_cls);
    env->DeleteLocalRef(cutout);
    env->DeleteLocalRef(wi_cls);
    env->DeleteLocalRef(window_insets);
    env->DeleteLocalRef(view_cls);
    env->DeleteLocalRef(decor);
    env->DeleteLocalRef(win_cls);
    env->DeleteLocalRef(window);
    env->DeleteLocalRef(act_cls);
    return insets;
}
