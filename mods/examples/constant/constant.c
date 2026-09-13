/* constant.c - the replace-hook example.
 *
 * 0040c670 is `void load_objs_1(char)`: `if (arg == 0) arg = 2; load_objs(arg);`
 * - a leaf thunk with one constant in it and no other side effects, which is
 * why it is a safe thing to replace. This hook substitutes its own constant
 * (from a setting) and delegates, so the run stays deterministic while the
 * replacement really happens: call_original performs the guest RET, and this
 * hook therefore must NOT call hook_return.
 *
 * The non-delegating form is in docs/MODDING.md; it is not used here, because
 * an example that skips object loading would break the game it ships with. */
#include "pop_mod_api.h"
#include <stdio.h>
#include <stdlib.h>

POP_MOD_DECLARE_ABI();

static const PopModApi *g_api;
static uint32_t g_hook;
static unsigned g_replaced, g_substituted;

static void replace_thunk(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                          void *user) {
    int64_t want = 2;
    uint8_t arg = 0;
    (void)inv;
    (void)user;
    ++g_replaced;
    api->settings_get(api, "default_set", &want);
    /* The argument is the byte at [ESP+4], since the guest RET has not run. */
    if (api->guest_read_u8(api, cpu->esp + 4, &arg) == POP_OK && arg == 0) {
        api->guest_write_u8(api, cpu->esp + 4, (uint8_t)want);
        ++g_substituted;
    }
    api->call_original(api, cpu->target, cpu);
}

PopModStatus pop_mod_init(const PopModApi *api) {
    uint32_t addr = 0;
    g_api = api;
    if (api->symbol(api, "level_startup_thunk", &addr) != POP_OK)
        return POP_E_NOSYMBOL;
    return api->hook_install(api, addr, replace_thunk, POP_HOOK_REPLACE, 0, &g_hook);
}

PopModStatus pop_mod_exit(void) {
    const char *dir = getenv("RECOMP_PROFILE_DIR");
    char path[512];
    FILE *f;
    snprintf(path, sizeof path, "%s/example-constant.txt",
             dir && *dir ? dir : "build/recomp/profile");
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "replaced %u\nsubstituted %u\n", g_replaced, g_substituted);
        fclose(f);
    }
    return g_api->hook_remove(g_api, g_hook);
}
