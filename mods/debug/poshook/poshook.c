/* poshook.c - who asks for a positional sound, and with what.
 *
 * WHY THIS EXISTS
 *
 * 0056fb10 is one virtual method that picks between two QMixer exports on its
 * fourth argument:
 *
 *     0056fb2d  CMP dword ptr [ESP + 0x34], 0
 *     0056fb32  JZ 0x0056fbaf      taken     -> QSWaveMixSetPosition
 *                                  not taken -> QSWaveMixSetSourcePosition
 *
 * On identical rolling-demo content the original takes the positional arm 0
 * times and we take it 424, so that argument is nonzero on our side and zero
 * on the original's. This mod reads it, and reads who passed it, because there
 * is no static call site to read instead: 0056fb10 is slot +0xfc of the vtable
 * at 00593ea8 that the constructor at 0056ebe0 installs, and no CALL anywhere
 * in the image names that slot. The dispatch is computed, so the caller is a
 * run-time fact or it is nothing.
 *
 * WHAT IT READS
 *
 * A before hook sees the callee entry, so ESP still points at the return
 * address and nothing has been pushed:
 *
 *     [ESP + 0x00]  the caller's return address
 *     [ESP + 0x04]  arg0 .. [ESP + 0x18] arg5     (RET 0x18, six dwords)
 *     ECX           this
 *
 * That matches the function's own reads: it takes arg0 from [ESP + 0x24] after
 * pushing 0x20 bytes, and the branch reads [ESP + 0x34] after pushing 0x24.
 *
 * It aggregates rather than logging four thousand lines: a total, a positional
 * count, a table by caller and one by argument value. The first few calls are
 * logged in full because a summary cannot show an argument's shape.
 *
 * WHERE THE ARGUMENT COMES FROM
 *
 * The only caller is 0056dc90, inside 0056db50, and it computes the argument
 * from two bit tests:
 *
 *     0056dc3f  TEST byte ptr [ESI + 0x30], 0x2    set -> 1
 *     0056dc45  TEST byte ptr [EBX + 0xc], 0x20    set -> 1
 *                                                  neither -> 0
 *
 * Both operands are reachable from what a hook already sees. ESI is that
 * function's `this` (0056db56 MOV ESI,ECX), so a second hook on 0056db50 reads
 * [ecx + 0x30] directly. EBX is recoverable at the inner hook without a third
 * hook: the caller pushes LEA EAX,[EBX + 0x38] as arg2, so EBX is arg2 - 0x38
 * and the tested byte is arg2 - 0x2c. Tallying that bit against the argument
 * says which of the two tests is the one that fires.
 */
#include "pop_mod_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

POP_MOD_DECLARE_ABI();

#define MAX_CALLERS 24
#define MAX_VALUES 24
#define FULL_LOGGED 8

static const PopModApi *g_api;
static uint32_t g_hook;

static unsigned g_calls;      /* every entry to 0056fb10 */
static unsigned g_positional; /* those whose arg3 was nonzero */
static unsigned g_unreadable; /* a stack read that failed */
static unsigned g_logged;

/* The two tests, counted separately. `both` and `neither` are what say whether
 * one test explains the argument or the two disagree. */
static unsigned g_req_bit_set; /* [EBX + 0xc] & 0x20 at the inner hook */
static unsigned g_req_unreadable;
static unsigned g_pos_and_req, g_pos_not_req, g_flat_and_req;

static uint32_t g_outer_hook;
static unsigned g_outer_calls, g_outer_sys_bit, g_outer_unreadable;

struct Bucket {
    uint32_t key;
    unsigned count, positional;
};
static struct Bucket g_callers[MAX_CALLERS];
static unsigned g_caller_count;
static unsigned g_callers_dropped;

static struct Bucket g_values[MAX_VALUES];
static unsigned g_value_count;
static unsigned g_values_dropped;

/* Who calls 0056db50, and which of those callers hands it a request whose
 * 0x20 bit is set. EBX is that function's arg1 (0056db85 MOV EBX,[ESP+0x38],
 * which is arg1 once its 0x30 bytes of prologue are accounted for), so at the
 * entry hook it is [esp + 8] and the tested byte is arg1 + 0xc. */
static struct Bucket g_outer_callers[MAX_CALLERS];
static unsigned g_outer_caller_count, g_outer_callers_dropped;

/* A fixed table, not a growing one: a hook runs on the guest's thread inside
 * the call it is observing, and allocating there would change the timing of
 * the thing being measured. Anything past the table is counted as dropped
 * rather than silently folded into a neighbour. */
static void tally(struct Bucket *t, unsigned *n, unsigned *dropped, unsigned cap, uint32_t key,
                  int positional) {
    unsigned i;
    for (i = 0; i < *n; ++i) {
        if (t[i].key == key) {
            ++t[i].count;
            if (positional)
                ++t[i].positional;
            return;
        }
    }
    if (*n == cap) {
        ++*dropped;
        return;
    }
    t[*n].key = key;
    t[*n].count = 1;
    t[*n].positional = positional ? 1u : 0u;
    ++*n;
}

static void on_set_position(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv,
                            void *user) {
    uint32_t esp = cpu->esp, caller = 0, a[6];
    int i, ok = 1;
    (void)inv;
    (void)user;

    if (api->guest_read_u32(api, esp, &caller) != POP_OK)
        ok = 0;
    for (i = 0; i < 6; ++i)
        if (api->guest_read_u32(api, esp + 4u + 4u * (uint32_t)i, &a[i]) != POP_OK)
            ok = 0;
    if (!ok) {
        ++g_unreadable;
        return;
    }

    ++g_calls;
    if (a[3])
        ++g_positional;

    /* [EBX + 0xc], the request flag the caller tested, recovered from arg2. */
    {
        uint8_t req = 0;
        if (api->guest_read_u8(api, a[2] - 0x2cu, &req) != POP_OK) {
            ++g_req_unreadable;
        } else {
            int set = (req & 0x20u) != 0;
            if (set)
                ++g_req_bit_set;
            if (a[3] && set)
                ++g_pos_and_req;
            else if (a[3] && !set)
                ++g_pos_not_req;
            else if (!a[3] && set)
                ++g_flat_and_req;
        }
    }
    tally(g_callers, &g_caller_count, &g_callers_dropped, MAX_CALLERS, caller, a[3] != 0);
    tally(g_values, &g_value_count, &g_values_dropped, MAX_VALUES, a[3], a[3] != 0);

    if (g_logged < FULL_LOGGED) {
        char line[256];
        ++g_logged;
        snprintf(line, sizeof line,
                 "poshook: caller %08x this %08x args %08x %08x %08x %08x %08x %08x", caller,
                 cpu->ecx, a[0], a[1], a[2], a[3], a[4], a[5]);
        api->log(api, line);
    }
}

/* 0056db50, whose `this` is the ESI the first test reads. */
static void on_outer(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint8_t sys = 0;
    (void)inv;
    (void)user;
    ++g_outer_calls;
    if (api->guest_read_u8(api, cpu->ecx + 0x30u, &sys) != POP_OK)
        ++g_outer_unreadable;
    else if (sys & 0x2u)
        ++g_outer_sys_bit;

    {
        uint32_t caller = 0, req = 0;
        uint8_t flags = 0;
        if (api->guest_read_u32(api, cpu->esp, &caller) == POP_OK &&
            api->guest_read_u32(api, cpu->esp + 8u, &req) == POP_OK &&
            api->guest_read_u8(api, req + 0xcu, &flags) == POP_OK)
            tally(g_outer_callers, &g_outer_caller_count, &g_outer_callers_dropped, MAX_CALLERS,
                  caller, (flags & 0x20u) != 0);
        else
            ++g_outer_unreadable;
    }
}

static void report(FILE *f) {
    unsigned i;
    fprintf(f, "calls %u\npositional %u\nflat %u\nunreadable %u\n", g_calls, g_positional,
            g_calls - g_positional, g_unreadable);
    fprintf(f, "callers %u dropped %u\n", g_caller_count, g_callers_dropped);
    for (i = 0; i < g_caller_count; ++i)
        fprintf(f, "  caller %08x calls %u positional %u\n", g_callers[i].key, g_callers[i].count,
                g_callers[i].positional);
    fprintf(f, "arg3 values %u dropped %u\n", g_value_count, g_values_dropped);
    for (i = 0; i < g_value_count; ++i)
        fprintf(f, "  arg3 %08x calls %u\n", g_values[i].key, g_values[i].count);
    fprintf(f, "request flag [EBX+0xc] & 0x20 set %u unreadable %u\n", g_req_bit_set,
            g_req_unreadable);
    fprintf(f, "  positional with the request bit    %u\n", g_pos_and_req);
    fprintf(f, "  positional without it              %u\n", g_pos_not_req);
    fprintf(f, "  flat with it                       %u\n", g_flat_and_req);
    fprintf(f, "outer 0056db50 calls %u, [this+0x30] & 0x2 set %u, unreadable %u\n", g_outer_calls,
            g_outer_sys_bit, g_outer_unreadable);
    fprintf(f, "outer callers %u dropped %u\n", g_outer_caller_count, g_outer_callers_dropped);
    for (i = 0; i < g_outer_caller_count; ++i)
        fprintf(f, "  caller %08x calls %u with the 0x20 bit %u\n", g_outer_callers[i].key,
                g_outer_callers[i].count, g_outer_callers[i].positional);
}

PopModStatus pop_mod_init(const PopModApi *api) {
    uint32_t addr = 0;
    g_api = api;
    if (api->symbol(api, "FUN_0056fb10", &addr) != POP_OK)
        return POP_E_NOSYMBOL;
    api->log(api, "poshook: hooking FUN_0056fb10, the source-position setter");
    if (api->hook_install(api, addr, on_set_position, POP_HOOK_BEFORE, 0, &g_hook) != POP_OK)
        return POP_E_INVAL;
    if (api->symbol(api, "FUN_0056db50", &addr) != POP_OK)
        return POP_E_NOSYMBOL;
    api->log(api, "poshook: hooking FUN_0056db50, its only caller");
    return api->hook_install(api, addr, on_outer, POP_HOOK_BEFORE, 0, &g_outer_hook);
}

PopModStatus pop_mod_exit(void) {
    const char *dir = getenv("RECOMP_PROFILE_DIR");
    char path[512], line[128];
    FILE *f;
    snprintf(path, sizeof path, "%s/poshook.txt", dir && *dir ? dir : "build/recomp/profile");
    f = fopen(path, "wb");
    if (f) {
        report(f);
        fclose(f);
    }
    /* Also in the run log, because the run log is what a comparison reads and
     * a file in a profile directory is easy to forget to collect. */
    snprintf(line, sizeof line, "poshook: %u calls, %u positional, %u flat", g_calls, g_positional,
             g_calls - g_positional);
    g_api->log(g_api, line);
    g_api->hook_remove(g_api, g_outer_hook);
    return g_api->hook_remove(g_api, g_hook);
}
