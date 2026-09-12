#include "roots.h"
#include <cstdlib>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

ModsRoots mods_roots() {
    const char *core = std::getenv("POPM_CORE_MODS_DIR");
    const char *user = std::getenv("POPM_MODS_DIR");
    ModsRoots roots{core && *core ? core : "build/recomp/mods/core", user && *user ? user : "mods"};
#ifdef __APPLE__
    if (!core || !*core) {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> path(size);
        if (size && _NSGetExecutablePath(path.data(), &size) == 0) {
            std::string exe(path.data());
            auto slash = exe.rfind('/');
            std::string dir = exe.substr(0, slash);
            if (dir.ends_with("/Contents/MacOS"))
                roots.core = dir + "/../Resources/mods/core";
        }
    }
#endif
    return roots;
}
