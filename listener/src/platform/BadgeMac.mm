#include "platform/Platform.hpp"

#import <AppKit/AppKit.h>

namespace ke::platform {

void setBadge(int count) {
    NSApp.dockTile.badgeLabel = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
}

} // namespace ke::platform
