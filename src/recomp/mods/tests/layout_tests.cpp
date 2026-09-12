// layout_tests.cpp - resource and profile resolution for the three layouts
// (resources/ beside the executable, a macOS bundle, a developer checkout)
// and the environment override. Label nogame.
#include "../../platform/os.h"
#include "../layout.h"

#include <stdio.h>
#include <string.h>
#include <string>

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)

static std::string temp_root() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pop-layout-XXXXXX", os_temp_dir());
    return os_mkdtemp(dir) == 0 ? dir : "";
}

static void touch(const std::string &path) {
    if (FILE *f = fopen(path.c_str(), "wb"))
        fclose(f);
}

static void mkdir_p(const std::string &path) {
    std::string acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < path.size())
            acc.push_back(path[i]);
    }
}

int main() {
    os_unsetenv("POPM_PROFILE_DIR");
    std::string root = temp_root();
    CHECK(!root.empty());
    // 1. resources/ beside the executable (Windows and Linux archives).
    mkdir_p(root + "/portable/resources/mods/core");
    touch(root + "/portable/PopRecomp");
    host_layout_set_exe_path_for_test((root + "/portable/PopRecomp").c_str());
    CHECK(host_layout().resources_dir == root + "/portable/resources");
    CHECK(!host_layout().developer);
    CHECK(host_resource("mods/core") == root + "/portable/resources/mods/core");
    CHECK(host_layout().profile_dir.find("PopRecomp") != std::string::npos);
    CHECK(host_layout().profile_dir.find(root) ==
          std::string::npos); // per-user, not beside the exe
    // 2. A macOS bundle.
    mkdir_p(root + "/X.app/Contents/MacOS");
    mkdir_p(root + "/X.app/Contents/Resources");
    touch(root + "/X.app/Contents/MacOS/X");
    host_layout_set_exe_path_for_test((root + "/X.app/Contents/MacOS/X").c_str());
    CHECK(host_layout().resources_dir == root + "/X.app/Contents/Resources");
    CHECK(host_resource("classic-modes.json") ==
          root + "/X.app/Contents/Resources/classic-modes.json");
    // 3. A checkout: the marker file above the executable.
    mkdir_p(root + "/co/tools/recomp/baseline");
    mkdir_p(root + "/co/build/recomp");
    touch(root + "/co/tools/recomp/baseline/classic-modes.json");
    touch(root + "/co/build/recomp/pop_headless");
    host_layout_set_exe_path_for_test((root + "/co/build/recomp/pop_headless").c_str());
    CHECK(host_layout().developer);
    CHECK(host_layout().checkout_root == root + "/co");
    CHECK(host_resource("mods/core") == root + "/co/build/recomp/mods/core");
    CHECK(host_resource("texture-pack") == root + "/co/build/texture-pack");
    CHECK(host_resource("classic-modes.json") ==
          root + "/co/tools/recomp/baseline/classic-modes.json");
    CHECK(host_layout().profile_dir == root + "/co/build/recomp/profile");
    // 4. POPM_PROFILE_DIR wins everywhere.
    os_setenv("POPM_PROFILE_DIR", "/elsewhere/profile");
    host_layout_set_exe_path_for_test((root + "/co/build/recomp/pop_headless").c_str());
    CHECK(host_layout().profile_dir == "/elsewhere/profile");
    os_unsetenv("POPM_PROFILE_DIR");
    // 5. Nothing found: empty resources, per-user profile.
    mkdir_p(root + "/bare");
    touch(root + "/bare/exe");
    host_layout_set_exe_path_for_test((root + "/bare/exe").c_str());
    CHECK(host_layout().resources_dir.empty());
    CHECK(host_resource("mods/core").empty());
    CHECK(!host_layout().developer);
    host_layout_set_exe_path_for_test(nullptr);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all layout tests passed\n");
    return g_failures ? 1 : 0;
}
