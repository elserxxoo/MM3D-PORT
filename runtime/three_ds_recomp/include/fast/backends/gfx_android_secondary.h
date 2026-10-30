#pragma once

// Secondary-display window store for Android dual-screen support.
// The Java side (SecondaryDisplayPresenter) hands the Surface of a Presentation
// shown on a secondary display to native code; the Vulkan backend presents the
// 3DS bottom screen to it.

#if defined(__ANDROID__)

#include <android/native_window.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace Fast::AndroidSecondary {

struct State {
    std::mutex Mutex;
    ANativeWindow* Window = nullptr;
    std::uint32_t Generation = 0;
    std::vector<ANativeWindow*> Retired;
    std::atomic<bool> HasWindow{false};
};

inline State& Get() {
    static State state;
    return state;
}

// Takes ownership of one reference on `window` (may be nullptr to clear).
inline void SetWindow(ANativeWindow* window) {
    State& state = Get();
    std::lock_guard lock(state.Mutex);
    if (state.Window != nullptr && state.Window != window) {
        state.Retired.push_back(state.Window);
    } else if (state.Window == window && window != nullptr) {
        // Same pointer handed over again: drop the duplicate reference.
        ANativeWindow_release(window);
        ++state.Generation;
        return;
    }
    state.Window = window;
    ++state.Generation;
    state.HasWindow.store(window != nullptr);
}

inline ANativeWindow* GetWindow(std::uint32_t* generation) {
    State& state = Get();
    std::lock_guard lock(state.Mutex);
    if (generation != nullptr) {
        *generation = state.Generation;
    }
    return state.Window;
}

// Called by the render thread once it no longer uses retired windows.
inline void ReleaseRetired() {
    State& state = Get();
    std::lock_guard lock(state.Mutex);
    for (ANativeWindow* window : state.Retired) {
        ANativeWindow_release(window);
    }
    state.Retired.clear();
}

}  // namespace Fast::AndroidSecondary

#endif  // __ANDROID__
