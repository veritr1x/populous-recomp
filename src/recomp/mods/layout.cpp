#include "layout.h"

#include "../platform/os.h"

#include <stdlib.h>
#include <string.h>

namespace {

std::string g_test_exe;
HostLayout g_layout;
bool g_computed = false;

bool exists(const std::string &p) {
    OsStat st;
    return os_stat(p.c_str(), &st) == 0;
}

std::string parent(const std::string &p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "" : p.substr(0, s);
}

bool ends_with(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

HostLayout compute() {
    HostLayout l;
    std::string exe = g_test_exe;
    if (exe.empty()) {
        char path[4096];
        if (os_exe_path(path, sizeof path) == 0)
            exe = path;
    }
    for (char &c : exe)
        if (c == '\\')
            c = '/';
    const std::string dir = parent(exe);
    if (ends_with(dir, "/Contents/MacOS"))
        l.resources_dir = parent(dir) + "/Resources";
    else if (exists(dir + "/resources"))
        l.resources_dir = dir + "/resources";
    else {
        std::string up = dir;
        for (int depth = 0; depth < 12 && !up.empty(); ++depth) {
            if (exists(up + "/tools/recomp/baseline/classic-modes.json")) {
                l.checkout_root = up;
                l.developer = true;
                l.resources_dir = up;
                break;
            }
            up = parent(up);
        }
    }
    const char *env = getenv("POPM_PROFILE_DIR");
    if (env && *env)
        l.profile_dir = env;
    else if (l.developer)
        l.profile_dir = l.checkout_root + "/build/recomp/profile";
    else {
        char buf[4096];
        if (os_user_data_dir("PopRecomp", buf, sizeof buf) == 0)
            l.profile_dir = buf;
        else
            l.profile_dir = "profile"; // no home at all: beside the cwd
    }
    return l;
}

} // namespace

const HostLayout &host_layout() {
    if (!g_computed) {
        g_layout = compute();
        g_computed = true;
    }
    return g_layout;
}

std::string host_resource(const char *rel) {
    const HostLayout &l = host_layout();
    if (l.resources_dir.empty())
        return "";
    if (l.developer) {
        if (strcmp(rel, "mods/core") == 0)
            return l.checkout_root + "/build/recomp/mods/core";
        if (strcmp(rel, "texture-pack") == 0)
            return l.checkout_root + "/build/texture-pack";
        if (strcmp(rel, "classic-modes.json") == 0)
            return l.checkout_root + "/tools/recomp/baseline/classic-modes.json";
    }
    return l.resources_dir + "/" + rel;
}

void host_layout_set_exe_path_for_test(const char *exe_path) {
    g_test_exe = exe_path ? exe_path : "";
    g_computed = false;
}
