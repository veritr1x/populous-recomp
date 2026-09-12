/* probe.c - the smallest plugin that can prove a host runs plugins.
 *
 * It hooks two real entry symbols, counts how often each is reached, and
 * writes the totals to <profile>/probe.txt from pop_mod_exit. The profile
 * directory comes from POPM_PROFILE_DIR with the same default the runtime
 * uses, because the file has to land where the harness is looking.
 *
 * WHY load_objs AND NOT ONLY THE TURN SCHEDULER. main_loop_inner (004ec6f0)
 * and main_loop_outer (004a5590) are the in-level loops, and a host booted to
 * the front end never reaches either: measured over a 60-frame headless run,
 * both are hit zero times while load_objs (0040c690) is hit once and the
 * window procedure nine times. The parity fixture is the other way round - it
 * scripts a level startup and then drives 004a5590 by hand. So the probe
 * counts both, and what the harness asserts is that SOME hook ran, which is
 * the claim that matters: a plugin's code executed inside translated code.
 *
 * Nothing here tests the mod API's surface - the module's own suites do that.
 * What this checks is the one thing they cannot: that the host shipped the
 * runtime, found this directory, loaded this dylib and ran its hooks. */
#include "pop_mod_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

POP_MOD_DECLARE_ABI();

static unsigned long g_loads, g_turns, g_entries;

static void on_level_load(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                          void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_loads;
}

static void on_turn(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_turns;
}

/* The process entry point. A hook installed during pop_mod_init only fires
 * here if the loader published its pending registrations BEFORE the guest
 * entry ran: the install happens on a thread that holds no baton, so it is
 * queued, and a queue applied at the first scheduler checkpoint would already
 * be too late for the very first function the guest runs. */
static void on_entry(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    (void)api;
    (void)cpu;
    (void)inv;
    (void)user;
    ++g_entries;
}

static PopModStatus hook_named(const PopModApi *api, const char *name, PopHookFn fn) {
    uint32_t addr = 0, id = 0;
    if (api->symbol(api, name, &addr) != POP_OK)
        return POP_E_NOSYMBOL;
    return api->hook_install(api, addr, fn, POP_HOOK_BEFORE, 0, &id);
}

PopModStatus pop_mod_init(const PopModApi *api) {
    PopModStatus s = hook_named(api, "load_objs", on_level_load);
    if (s != POP_OK)
        return s;
    s = hook_named(api, "main_loop_inner", on_turn);
    if (s != POP_OK)
        return s;
    s = hook_named(api, "entry", on_entry);
    if (s != POP_OK)
        return s;
    api->log(api, "probe: hooked entry, load_objs and main_loop_inner");
    return POP_OK;
}

PopModStatus pop_mod_exit(void) {
    /* The same default mods_overlay_profile_dir uses, so a run that sets
     * nothing still writes somewhere predictable. */
    const char *dir = getenv("POPM_PROFILE_DIR");
    char path[1024];
    if (!dir || !*dir)
        dir = "build/recomp/profile";
    snprintf(path, sizeof path, "mkdir -p '%s'", dir);
    if (system(path) != 0)
        return POP_E_STATE;
    snprintf(path, sizeof path, "%s/probe.txt", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return POP_E_STATE;
    fprintf(f, "hooks %lu\nentries %lu\nloads %lu\nturns %lu\n", g_loads + g_turns + g_entries,
            g_entries, g_loads, g_turns);
    fclose(f);
    return POP_OK;
}
