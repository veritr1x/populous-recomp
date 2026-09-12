// layout.h - where this executable's resources and the player's profile live:
// a macOS bundle's Contents/Resources, a resources/ directory beside the
// executable (the Windows and Linux archives), or a developer checkout found
// above the executable. Computed once from os_exe_path().
#pragma once
#include <string>

struct HostLayout {
    std::string resources_dir; // "" when nothing was found
    std::string profile_dir;   // settings, saves, game-path.txt
    std::string checkout_root; // "" outside a checkout
    bool developer = false;    // checkout_root is set
};
const HostLayout &host_layout();
// resources_dir + "/" + rel, with the developer mapping for the three names
// "mods/core", "texture-pack" and "classic-modes.json"; "" when unknown.
std::string host_resource(const char *rel);
// Test seam: recompute from this executable path and the current environment
// (nullptr restores the real path). Not thread-safe; tests only.
void host_layout_set_exe_path_for_test(const char *exe_path);
