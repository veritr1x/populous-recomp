/* logger.c - the before-hook example. Its observable effect is a file in the
 * PROFILE directory naming the symbol it hooked and the turns it counted. */
#include "pop_mod_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

POP_MOD_DECLARE_ABI();

static const PopModApi *g_api;
static uint32_t g_hook;
static unsigned g_turns;

static void on_turn(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_turns;
}

static void profile_path(char *out, size_t n, const char *leaf) {
    const char *dir = getenv("POPM_PROFILE_DIR");
    snprintf(out, n, "%s/%s", dir && *dir ? dir : "build/recomp/profile", leaf);
}

PopModStatus pop_mod_init(const PopModApi *api) {
    uint32_t addr = 0;
    g_api = api;
    if (api->symbol(api, "main_loop_inner", &addr) != POP_OK)
        return POP_E_NOSYMBOL;
    api->log(api, "logger: hooking main_loop_inner");
    return api->hook_install(api, addr, on_turn, POP_HOOK_BEFORE, 0, &g_hook);
}

PopModStatus pop_mod_exit(void) {
    char path[512];
    FILE *f;
    profile_path(path, sizeof path, "example-logger.txt");
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "symbol main_loop_inner\nturns %u\n", g_turns);
        fclose(f);
    }
    return g_api->hook_remove(g_api, g_hook);
}
