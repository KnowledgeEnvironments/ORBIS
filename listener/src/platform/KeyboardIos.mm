#include "platform/Keyboard.hpp"

#include "util/Log.hpp"

#import <UIKit/UIKit.h>

#include <atomic>

namespace ke::platform {
namespace {

std::atomic<int> g_overlap{0};
id g_observer = nil;

} // namespace

int keyboardOverlap() { return g_overlap.load(); }

void watchKeyboard(SDL_Window* window) {
    if (g_observer != nil) {
        return;
    }
    UIWindow* ui = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                              SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (ui == nil) {
        log::warn("keyboard: no UIWindow, the view will slide instead of resizing");
        return;
    }
    __weak UIWindow* weakUi = ui;
    g_observer = [[NSNotificationCenter defaultCenter]
        addObserverForName:UIKeyboardWillChangeFrameNotification
                    object:nil
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                    UIWindow* w = weakUi;
                    if (w == nil) {
                        return;
                    }
                    const CGRect end = [note.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
                    const CGRect local = [w convertRect:end fromCoordinateSpace:w.screen.coordinateSpace];
                    const CGRect bounds = w.bounds;
                    const CGRect hit = CGRectIntersection(bounds, local);
                    // a floating keyboard does not reach the bottom edge and covers nothing we can give up
                    const bool docked = CGRectGetMaxY(local) >= CGRectGetMaxY(bounds) - 1.0;
                    const int overlap = (docked && !CGRectIsNull(hit)) ? static_cast<int>(hit.size.height) : 0;
                    if (overlap != g_overlap.exchange(overlap)) {
                        log::info("keyboard: covers {} of {} points", overlap, static_cast<int>(bounds.size.height));
                        SDL_Event wake{};
                        wake.type = SDL_EVENT_USER;
                        SDL_PushEvent(&wake); // the loop may be idle: this draws the new layout
                    }
                }];
}

} // namespace ke::platform
