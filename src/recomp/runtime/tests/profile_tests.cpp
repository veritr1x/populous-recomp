// Real generated dispatch and a pinned-image string-copy fixture; no fake samples.
#include "../profile.h"
extern "C" {
#include "funcs.h"
}
#include "../loader.h"
#include "../memory.h"
#include "../intrinsics.h"
#include "../win32.h"
#include "../imports.h"
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <setjmp.h>
#include "../../platform/os.h"
static int failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
static constexpr uint32_t target = 0x00500040; // listing name memcpy_1 (NUL string copy)
static uint32_t index_;
static void direct_call(X86 *c) {
    CALL_FN(00500040);
}
static bool nested_ok;
static void nested(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 2);
    CHECK(sched_current_holder_slot()->top.load() == i);
    recomp_hooked[i] = 0;
    recomp_call(c, target);
    CHECK(recomp_profile_depth() == 2);
    CHECK(sched_current_holder_slot()->top.load() == i);
    nested_ok = true;
}
static uint32_t import_call(X86 *c, const char *name, std::initializer_list<uint32_t> args) {
    uint32_t saved = c->r[R_ESP];
    uint32_t sp = saved - 4 * (args.size() + 1);
    uint32_t p = sp + 4;
    for (auto arg : args) {
        wr32(p, arg);
        p += 4;
    }
    wr32(sp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = sp;
    uint32_t address = imports_resolve("kernel32.dll", name);
    CHECK(address != 0);
    imports_dispatch(c, address);
    CHECK(c->r[R_ESP] == saved);
    return c->r[R_EAX];
}
static bool worker_ran;
static ProfileSlot *worker_slot;
static void worker(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 1);
    worker_slot = sched_current_holder_slot();
    CHECK(worker_slot && worker_slot->top.load() == i);
    import_call(c, "Sleep", {1});
    CHECK(sched_current_holder_slot() == worker_slot);
    CHECK(worker_slot->top.load() == i);
    worker_ran = true;
    // ExitThread abandons recomp_call's pop; scheduler cleanup must clear it.
    import_call(c, "ExitThread", {0});
    CHECK(false);
}
static bool sync_ran;
static void sync_worker(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 2);
    recomp_profile_push(i); // abandoned by ExitThread's synchronous landing pad
    sync_ran = true;
    import_call(c, "ExitThread", {0});
    CHECK(false);
}
int main(int argc, char **argv) {
    bool enabled = argc > 1 && strcmp(argv[1], "enabled") == 0;
    CHECK(bool(recomp_profile_enabled) == enabled);
    mem_init();
    if (!loader_load()) {
        fprintf(stderr, "loader: %s\n", loader_error());
        return 1;
    }
    X86 &c = *loader_context();
    loader_init_context(&c);
    sched_set_guest_thread(true);
    int32_t found = recomp_index_of(target);
    CHECK(found >= 0);
    if (found < 0)
        return 1;
    index_ = uint32_t(found);
    constexpr uint32_t n = 65536;
    uint32_t src = heap_alloc(n), dst = heap_alloc(n);
    CHECK(src && dst);
    if (!src || !dst)
        return 1;
    memset(g_mem + src, 0x5a, n);
    g_mem[src + n - 1] = 0;
    uint32_t sp = c.r[R_ESP] - 32;
    auto setup = [&] {
        c.r[R_ESP] = sp;
        wr32(sp, GUEST_RETURN_SENTINEL);
        wr32(sp + 4, dst);
        wr32(sp + 8, src);
    };
    // Caller stays published across all 10,000 translated calls; gaps therefore
    // count against the >90% requirement instead of disappearing as idle time.
    recomp_profile_push(0xfffffffe);
    for (int i = 0; i < 10000; ++i) {
        setup();
        if (i & 1)
            direct_call(&c);
        else
            recomp_call(&c, target);
    }
    CHECK(memcmp(g_mem + src, g_mem + dst, n) == 0);
    CHECK(recomp_profile_depth() == (enabled ? 1u : 0u));
    if (enabled) {
        auto rows = profile_snapshot();
        uint64_t total = 0, hot = 0;
        for (auto r : rows) {
            total += r.samples;
            if (r.index == index_)
                hot = r.samples;
        }
        printf("fixture: 10000 translated memcpy_1 calls, hot=%llu total=%llu share=%.2f%%\n",
               (unsigned long long)hot, (unsigned long long)total, total ? 100.0 * hot / total : 0);
        CHECK(total >= 100);
        CHECK(hot * 100 > total * 90);
        setup();
        recomp_hook_ptrs[index_] = nested;
        recomp_hooked[index_] = 1;
        recomp_call(&c, target);
        CHECK(nested_ok);
        CHECK(recomp_profile_depth() == 1);
        CHECK(sched_current_holder_slot()->top.load() == 0xfffffffe);
        // Match the generated two-call intrinsic: the saved depth must survive
        // jumping over two published callees, and preserve the original caller.
        uint32_t buf = heap_alloc(64);
        c.r[R_ESP] = sp;
        wr32(sp, GUEST_RETURN_SENTINEL);
        wr32(sp + 4, buf);
        jmp_buf *env = recomp_setjmp_prepare(&c);
        int value = setjmp(*env);
        recomp_setjmp_return(&c, value);
        if (!value) {
            recomp_profile_push(index_);
            recomp_profile_push(123);
            c.r[R_ESP] = sp - 32;
            wr32(sp - 28, buf);
            wr32(sp - 24, 7);
            recomp_longjmp(&c);
            CHECK(false);
        }
        CHECK(value == 7);
        CHECK(recomp_profile_depth() == 1);
        CHECK(sched_current_holder_slot()->top.load() == 0xfffffffe);
        ProfileSlot *main_slot = sched_current_holder_slot();
        recomp_hook_ptrs[index_] = worker;
        recomp_hooked[index_] = 1;
        import_call(&c, "CreateThread", {0, 0, target, 0, 0, 0});
        for (int tries = 0; tries < 100 && !worker_ran; ++tries)
            import_call(&c, "Sleep", {2});
        CHECK(worker_ran);
        CHECK(worker_slot && worker_slot != main_slot);
        CHECK(worker_slot && worker_slot->top.load() == PROFILE_IDLE);
        CHECK(sched_current_holder_slot() == main_slot);
        CHECK(main_slot->top.load() == 0xfffffffe);
        recomp_hook_ptrs[index_] = sync_worker;
        os_setenv("POPM_CREATETHREAD", "sync");
        import_call(&c, "CreateThread", {0, 0, target, 0, 0, 0});
        os_unsetenv("POPM_CREATETHREAD");
        CHECK(sync_ran);
        CHECK(recomp_profile_depth() == 1);
        CHECK(main_slot->top.load() == 0xfffffffe);
        recomp_hooked[index_] = 0;
        recomp_profile_pop();
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
        recomp_profile_push(index_);
        sched_run_thread_unwind_frames();
        CHECK(recomp_profile_depth() == 0);
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
        recomp_profile_push(index_);
        sched_set_guest_thread(false);
        CHECK(recomp_profile_depth() == 0);
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
    } else {
        CHECK(profile_snapshot().empty());
        CHECK(sched_current_holder_slot() == nullptr);
    }
    profile_stop();
    printf("profile tests: %s (%d failures; %s)\n", failures ? "FAIL" : "PASS", failures,
           enabled ? "enabled" : "disabled");
    return failures ? 1 : 0;
}
