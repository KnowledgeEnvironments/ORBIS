// How much of the window the on-screen keyboard covers, in the same units as SDL_GetWindowSize.
// SDL has no API for it: left alone, iOS slides the whole view up and Android draws the keyboard
// over the bottom of the window. The phone frame subtracts this from the work area, so the panes
// shrink and the input line sits on top of the keyboard. No-ops off the phones.
#pragma once

#include "platform/Platform.hpp"

#include <SDL3/SDL.h>

namespace ke::platform {

#if LISTENER_PHONE
void watchKeyboard(SDL_Window* window); // once, after the window exists
int keyboardOverlap();
#else
inline void watchKeyboard(SDL_Window*) {}
inline int keyboardOverlap() { return 0; }
#endif

// A orbis:// link that launched the app reached the OS before SDL had an event queue: this hands
// it to the queue, as SDL_EVENT_DROP_FILE like any later link. Android only; iOS's SDL holds it.
#if defined(SDL_PLATFORM_ANDROID)
void deliverPendingLink();
#else
inline void deliverPendingLink() {}
#endif

} // namespace ke::platform
