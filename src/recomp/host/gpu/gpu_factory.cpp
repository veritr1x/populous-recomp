#include "gpu_factory.h"

namespace gpu {

#ifdef __APPLE__
// Implemented in metal/metal_device.mm and metal/metal_surface.mm.
std::unique_ptr<Device> metal_create_device();
void *metal_test_native_surface(int w, int h);

std::unique_ptr<Device> create_default_device() {
    return metal_create_device();
}
const char *default_backend_name() {
    return "metal";
}
void *test_native_surface(int w, int h) {
    return metal_test_native_surface(w, h);
}
#else
std::unique_ptr<Device> create_default_device() {
    return nullptr;
}
const char *default_backend_name() {
    return "none";
}
void *test_native_surface(int, int) {
    return nullptr;
}
#endif

} // namespace gpu
