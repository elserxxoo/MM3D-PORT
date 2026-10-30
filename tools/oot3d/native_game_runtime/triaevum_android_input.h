#pragma once

// Android device input bridge (AYN Thor): written by the JNI layer
// (ports/android/runtime/android_host.cpp), read by the native input poll.
// Kept free of game-specific types so it can be shared by any title.

#if defined(__ANDROID__)
#include <atomic>
#include <cstdint>

namespace Oot3dNativeGame {

struct AndroidDeviceInput {
  // Touch on the secondary display (3DS lower screen), in secondary-view pixels.
  std::atomic<bool> secondaryTouchPressed{false};
  std::atomic<float> secondaryTouchX{0.0F};
  std::atomic<float> secondaryTouchY{0.0F};
  std::atomic<uint32_t> secondaryTouchW{0U};
  std::atomic<uint32_t> secondaryTouchH{0U};
  // Built-in motion sensors, already rotated into the screen frame by the Java
  // side: X right, Y up, Z toward the player. gyro* rad/s, accel* m/s^2.
  std::atomic<bool> motionEnabled{true};
  std::atomic<bool> motionValid{false};
  std::atomic<float> gyroX{0.0F}, gyroY{0.0F}, gyroZ{0.0F};
  std::atomic<float> accelX{0.0F}, accelY{0.0F}, accelZ{0.0F};
};

inline AndroidDeviceInput &GetAndroidDeviceInput() {
  static AndroidDeviceInput state;
  return state;
}

}  // namespace Oot3dNativeGame
#endif
