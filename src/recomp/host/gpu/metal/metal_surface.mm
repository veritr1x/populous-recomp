// metal_surface.mm - the CAMetalLayer side of the swapchain: refresh rate and
// the layer a test uses when it has no window.
#include "metal_device.h"

#import <CoreGraphics/CoreGraphics.h>

namespace gpu {

double MetalDevice::refresh_period(Swapchain s) {
    {
        std::lock_guard lock(mutex_);
        if (swapchains_.find(s.id) == swapchains_.end())
            return 1.0 / 60;
    }
    // The layer does not know its display; the main display's rate is what the
    // AppKit host's CVDisplayLink reported for a single-display machine. Apple
    // laptops report 0 for an adaptive panel, which reads as 60.
    double hz = 0;
    if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGMainDisplayID())) {
        hz = CGDisplayModeGetRefreshRate(mode);
        CGDisplayModeRelease(mode);
    }
    return hz > 1.0 ? 1.0 / hz : 1.0 / 60;
}

void *metal_test_native_surface(int w, int h) {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.drawableSize = CGSizeMake(w, h);
    return (__bridge_retained void *)layer;
}

} // namespace gpu
