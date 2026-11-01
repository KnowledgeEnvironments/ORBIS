#include "platform/Keyboard.hpp"

#include "util/Log.hpp"

#include <SDL3/SDL_system.h>
#include <atomic>
#include <jni.h>

namespace ke::platform {

static std::atomic<int> g_overlap{0}; // named from the JNI entry below

void watchKeyboard(SDL_Window*) {} // OrbisSurface pushes the height; nothing to subscribe to

int keyboardOverlap() {
    return g_overlap.load();
}

void deliverPendingLink() {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        log::warn("android: no activity to ask for a pending link");
        return;
    }
    // The object's class, not FindClass: this thread's class loader cannot see the app's classes.
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID method = env->GetMethodID(cls, "deliverPendingLink", "()V")) {
        env->CallVoidMethod(activity, method);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        log::warn("android: pending link delivery failed");
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

} // namespace ke::platform

// The surface's pixels are SDL's window units on Android (pixel density 1, the density is in the
// display scale), so the height needs no conversion. Called on the UI thread.
extern "C" JNIEXPORT void JNICALL Java_dev_orbis_listener_OrbisActivity_nativeKeyboardHeight(JNIEnv*, jclass,
                                                                                             jint pixels) {
    if (pixels != ke::platform::g_overlap.exchange(pixels)) {
        ke::log::info("keyboard: covers {} px", static_cast<int>(pixels));
        SDL_Event wake{};
        wake.type = SDL_EVENT_USER;
        SDL_PushEvent(&wake); // the loop may be idle: this draws the new layout
    }
}
