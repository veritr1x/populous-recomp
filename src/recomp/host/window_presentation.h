#pragma once
#import <AppKit/NSApplication.h>
#include <algorithm>
#include <cmath>

// Keep the associated, accelerated pointer away from desktop hot edges. Map
// this slightly inset travel area back onto every game pixel, including the
// outermost row/column used by edge scrolling. Reach the last pixel one point
// before the far clip boundary, independent of fractional boundary rounding.
inline NSRect host_pointer_confinement_rect(NSRect bounds) {
    if (bounds.size.width <= 10 || bounds.size.height <= 10)
        return NSZeroRect;
    return NSInsetRect(bounds, 4, 4);
}
inline int host_confined_pointer_pixel(double point, double origin, double extent, int pixels,
                                       bool flip = false) {
    if (extent <= 1 || pixels <= 1)
        return 0;
    double t = std::clamp((point - origin) / (extent - 1), 0.0, 1.0);
    if (flip)
        t = 1.0 - t;
    return int(std::lround(t * (pixels - 1)));
}

// Retain AppKit's normal fullscreen options so releasing game input restores
// the user's usual menu/toolbar behavior. Hide and auto-hide are mutually
// exclusive; AutoHideToolbar also requires AutoHideMenuBar.
inline NSApplicationPresentationOptions
host_fullscreen_presentation(NSApplicationPresentationOptions normal, bool playing) {
    if (!playing)
        return normal;
    return (normal &
            ~(NSApplicationPresentationAutoHideDock | NSApplicationPresentationAutoHideMenuBar |
              NSApplicationPresentationAutoHideToolbar)) |
           NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
}
