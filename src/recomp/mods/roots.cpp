#include "roots.h"
#include "../platform/os.h"
#include <cstdlib>
#include <string>

// The core root defaults to the checkout's build tree. Inside a macOS bundle
// the executable sits in Contents/MacOS and the packaged core mods beside it
// in Contents/Resources, so a relocated bundle still finds its own plugins.
ModsRoots mods_roots() {
    const char *core = std::getenv("POPM_CORE_MODS_DIR");
    const char *user = std::getenv("POPM_MODS_DIR");
    ModsRoots roots{core && *core ? core : "build/recomp/mods/core", user && *user ? user : "mods"};
    if (!core || !*core) {
        char path[4096];
        if (os_exe_path(path, sizeof path) == 0) {
            std::string exe(path);
            auto slash = exe.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = exe.substr(0, slash);
                if (dir.ends_with("/Contents/MacOS"))
                    roots.core = dir + "/../Resources/mods/core";
            }
        }
    }
    return roots;
}
