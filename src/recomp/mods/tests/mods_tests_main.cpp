// mods_tests_main.cpp - runs every registered suite. Headless: no window, no
// audio device, no application object. Run from the repository root.
#include "mods_tests.h"
#include <stdio.h>
#include <stdlib.h>
#include <map>
#include <string>

namespace {
ModTestSuite *g_head = nullptr;
int g_checks = 0, g_failures = 0;
} // namespace

void mod_test_register(ModTestSuite *s) {
    s->next = g_head;
    g_head = s;
}
ModTestSuite *mod_test_suites() {
    return g_head;
}
void mod_test_pass() {
    ++g_checks;
}
int mod_test_failures() {
    return g_failures;
}
void mod_test_fail(const char *what, const char *file, int line) {
    ++g_checks;
    ++g_failures;
    fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what);
}

// Each suite name gets its OWN storage, and it is never rewritten.
//
// This used to build every answer in one static string and hand back a pointer
// into it, so the next call silently changed what every earlier caller was
// holding. Two suites keep the pointer rather than copying it, and after any
// later call their ROOT and PROFILE named whichever directory had been
// prepared most recently: the overlay suite wrote and unlinked inside the
// loader's tree, which made a plugin the loader had just installed vanish, and
// the settings suite read values out of the wrong profile. Which suites broke
// depended on suite order, so the same binary gave 0, 2, 4 or 10 failures on
// consecutive runs, and sometimes a segfault - the shared string reallocating
// between a short path and a long one, with a held pointer still aimed at the
// old buffer.
//
// A map keyed by suite name fixes it for every caller at once, including the
// ones that have not been written yet. The alternative - each caller copying
// into a string of its own - leaves the trap loaded for the next one.
const char *mod_test_dir(const char *suite) {
    static std::map<std::string, std::string> kept;
    std::string &path = kept[suite];
    const char *root = getenv("POPM_TEST_DIR");
    path = std::string(root && *root ? root : "build/recomp/mods-test") + "/" + suite;
    std::string cmd = "rm -rf '" + path + "' && mkdir -p '" + path + "'";
    if (system(cmd.c_str()) != 0)
        fprintf(stderr, "mod_test_dir: cannot prepare %s\n", path.c_str());
    return path.c_str();
}

int main() {
    // Settings apply now persists immediately. Even suites which only exercise
    // the UI/API must have a scratch profile rather than the player's profile.
    setenv("POPM_PROFILE_DIR", mod_test_dir("default-profile"), 1);
    // Line buffered, always. Redirected to a file or a pipe, stdout is block
    // buffered, and a suite that crashes takes every earlier suite's result
    // down with it - the run then looks as though nothing ran at all, which
    // is the opposite of what a crashing test run should tell you.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (ModTestSuite *s = mod_test_suites(); s; s = s->next) {
        int before = g_failures;
        s->fn();
        printf("%-34s %s\n", s->name, g_failures == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all mod tests passed\n");
    return g_failures ? 1 : 0;
}
