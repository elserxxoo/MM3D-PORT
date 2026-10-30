#include "android_host.h"

#include <SDL.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "fast/backends/gfx_android_secondary.h"
#include "triaevum_android_input.h"

void InitializeAndroidGameHost() {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    const char* root = SDL_AndroidGetExternalStoragePath();
    if (!root || !*root) {
        throw std::runtime_error("Android application data directory is unavailable");
    }
    std::filesystem::current_path(root);
    std::filesystem::create_directories("logs");
    // Android does not preserve a console stream; retain diagnostics beside user data.
    if (!std::freopen("logs/native-stdout.log", "w", stdout) ||
        !std::freopen("logs/native-stderr.log", "w", stderr)) {
        throw std::runtime_error("Cannot open Android runtime logs");
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SDL_Log("TriAevum game data: %s", root);
}

// ---- Dual-screen and motion bridge (AYN Thor) -------------------------------
// Java (SecondaryDisplayPresenter, MotionSensorBridge) -> native input state.
// SDL keeps owning the main surface and lifecycle; nothing here touches it.
namespace {
using Oot3dNativeGame::GetAndroidDeviceInput;
constexpr const char *kTag = "TriAevum";
}  // namespace

extern "C" {

JNIEXPORT void JNICALL
Java_org_triaevum_android_SecondaryDisplayPresenter_nativeSecondarySurfaceChanged(
    JNIEnv *env, jclass /*clazz*/, jobject surface, jint width, jint height) {
  if (surface == nullptr) {
    return;
  }
  ANativeWindow *win = ANativeWindow_fromSurface(env, surface);
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "nativeSecondarySurfaceChanged: ANativeWindow=%p, %dx%d", win, width,
                      height);
  Fast::AndroidSecondary::SetWindow(win);  // takes the reference
}

JNIEXPORT void JNICALL
Java_org_triaevum_android_SecondaryDisplayPresenter_nativeSecondarySurfaceDestroyed(
    JNIEnv * /*env*/, jclass /*clazz*/) {
  __android_log_print(ANDROID_LOG_INFO, kTag, "nativeSecondarySurfaceDestroyed");
  GetAndroidDeviceInput().secondaryTouchPressed.store(false);
  Fast::AndroidSecondary::SetWindow(nullptr);
}

JNIEXPORT void JNICALL
Java_org_triaevum_android_SecondaryDisplayPresenter_nativeSecondaryTouch(
    JNIEnv * /*env*/, jclass /*clazz*/, jfloat x, jfloat y, jint width, jint height,
    jboolean pressed) {
  auto &state = GetAndroidDeviceInput();
  if (width <= 0 || height <= 0) {
    state.secondaryTouchPressed.store(false, std::memory_order_relaxed);
    return;
  }
  state.secondaryTouchW.store(static_cast<uint32_t>(width), std::memory_order_relaxed);
  state.secondaryTouchH.store(static_cast<uint32_t>(height), std::memory_order_relaxed);
  state.secondaryTouchX.store(x, std::memory_order_relaxed);
  state.secondaryTouchY.store(y, std::memory_order_relaxed);
  state.secondaryTouchPressed.store(pressed == JNI_TRUE, std::memory_order_release);
}

JNIEXPORT void JNICALL
Java_org_triaevum_android_MotionSensorBridge_nativeSetMotionEnabled(
    JNIEnv * /*env*/, jclass /*clazz*/, jboolean enabled) {
  auto &state = GetAndroidDeviceInput();
  state.motionEnabled.store(enabled == JNI_TRUE, std::memory_order_relaxed);
  if (enabled != JNI_TRUE) {
    state.motionValid.store(false, std::memory_order_relaxed);
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "motion input %s",
                      enabled == JNI_TRUE ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_org_triaevum_android_MotionSensorBridge_nativeMotionSample(
    JNIEnv * /*env*/, jclass /*clazz*/, jfloat gx, jfloat gy, jfloat gz, jfloat ax, jfloat ay,
    jfloat az) {
  auto &state = GetAndroidDeviceInput();
  state.gyroX.store(gx, std::memory_order_relaxed);
  state.gyroY.store(gy, std::memory_order_relaxed);
  state.gyroZ.store(gz, std::memory_order_relaxed);
  state.accelX.store(ax, std::memory_order_relaxed);
  state.accelY.store(ay, std::memory_order_relaxed);
  state.accelZ.store(az, std::memory_order_relaxed);
  state.motionValid.store(true, std::memory_order_release);
}

JNIEXPORT void JNICALL
Java_org_triaevum_android_MotionSensorBridge_nativeMotionStopped(JNIEnv * /*env*/,
                                                                 jclass /*clazz*/) {
  GetAndroidDeviceInput().motionValid.store(false, std::memory_order_relaxed);
}

}  // extern "C"
