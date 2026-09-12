#include "roots.h"
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    ModsRoots roots = mods_roots();
    std::printf("core=%s user=%s\n", roots.core.c_str(), roots.user.c_str());
    return roots.core == argv[1] && roots.user == argv[2] ? 0 : 1;
}
