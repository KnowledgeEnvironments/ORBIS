#include "platform/Platform.hpp"

#include <SDL3/SDL.h>

#import <UIKit/UIKit.h>

// The window's own safe-area insets, in points, as UIKit lays them out now. SDL's
// SDL_GetWindowSafeArea gave the insets of the other orientation on the iPhone Duo simulator:
// an 84 pt band on the right of a portrait window, none at the top (2026-11-11).
namespace ke::platform {

bool nativeSafeArea(SDL_Window* window, Insets& out) {
    UIWindow* ui = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                              SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (ui == nil) {
        return false;
    }
    const UIEdgeInsets i = ui.safeAreaInsets;
    out.top = static_cast<float>(i.top);
    out.left = static_cast<float>(i.left);
    out.bottom = static_cast<float>(i.bottom);
    out.right = static_cast<float>(i.right);
    return true;
}

} // namespace ke::platform
