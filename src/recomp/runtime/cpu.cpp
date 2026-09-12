// cpu.cpp - the CPU-level call-outs x86.h declares as "provided by
// src/recomp/runtime/": import dispatch, unknown call targets, divide errors
// and the handful of privileged or environment-sensing instructions.
#include "imports.h"
#include "profile.h"
#include "mods_seam.h"
#include "intrinsics.h"
#include "win32.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>

// ---------------------------------------------------------------------------
// State for the _setjmp/_longjmp intrinsics, outside extern "C" because these
// helpers have C++ types.
// ---------------------------------------------------------------------------
namespace {
struct SetjmpRecord {
    uint32_t buf = 0;
    X86 saved{};
    jmp_buf env;
    bool armed = false;
    uint32_t profile_depth = 0;
};
// Keyed by the guest jmp_buf address. Heap allocated so a record outlives the
// frame that created it.
std::map<uint32_t, SetjmpRecord *> &setjmps() {
    static std::map<uint32_t, SetjmpRecord *> m;
    return m;
}
// Read after a longjmp, so it must not be a local of the returning frame.
X86 *g_setjmp_ctx = nullptr;
} // namespace

// ---------------------------------------------------------------------------
// Undeliverable call targets. A call the address table cannot deliver is
// always a translator bug - a missing entry point, or a call target inside a
// block nothing recovered - and the guest carries on with the wrong answer
// rather than stopping. Recording each distinct one lets a run report them
// instead of leaving them to be found in a log.
// ---------------------------------------------------------------------------
namespace {
std::vector<UnknownCall> &unknown_calls() {
    static std::vector<UnknownCall> v;
    return v;
}
} // namespace

const std::vector<UnknownCall> &recomp_unknown_calls() {
    return unknown_calls();
}

extern "C" {

// Consumes the return address the caller pushed and continues after the call,
// which is what the callee's RET would have done. Without this a call that the
// runtime cannot deliver leaves a dword on the guest stack and every later
// frame is displaced by four bytes.
static void return_as_if_ret(X86 *c) {
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
    c->r[R_EAX] = 0;
}

// An indirect call whose target landed in the trampoline range.
void recomp_shim_call(X86 *c, uint32_t target) {
    if (imports_dispatch(c, target))
        return;
    LOGW("recomp_shim_call: %08x is not an allocated trampoline (ESP=%08x, return=%08x)", target,
         c->r[R_ESP], rd32(c->r[R_ESP]));
    return_as_if_ret(c);
}

// An indirect call to an address that is neither a translated function nor a
// shim. Almost always a translator bug or an uninitialised function pointer,
// so it is logged with the target and the guest continues with EAX = 0.
void recomp_unknown_call(X86 *c, uint32_t target) {
    if (target == GUEST_RETURN_SENTINEL) {
        // The guest returned to the address the runtime pushes for a callback
        // and then called it, or a callback's RET was translated as a call.
        LOGW("call to the runtime callback return address %08x: a guest callback "
             "returned into its own return sentinel",
             target);
        return_as_if_ret(c);
        return;
    }
    uint32_t ret = rd32(c->r[R_ESP]);
    char key[64];
    snprintf(key, sizeof key, "unknown-call:%08x", target);
    if (log_once(key, "call to unknown target %08x (ESP=%08x, return=%08x): returning 0", target,
                 c->r[R_ESP], ret))
        unknown_calls().push_back(UnknownCall{target, ret});
    return_as_if_ret(c);
}

// DIV/IDIV with a zero divisor or an out-of-range quotient. On real hardware
// this raises #DE; there is no SEH here, so it is reported and the registers
// are left untouched.
void recomp_div_error(X86 *c, uint32_t addr) {
    LOGW("divide error at %08x (EAX=%08x EDX=%08x)", addr, c->r[R_EAX], c->r[R_EDX]);
}

// A monotonically increasing cycle counter derived from the millisecond clock,
// so repeated runs see the same ordering.
void recomp_rdtsc(X86 *c) {
    static uint64_t tsc = 0;
    uint64_t from_ms = (uint64_t)host_millis() * 1000000ull;
    if (from_ms > tsc)
        tsc = from_ms;
    tsc += 1000;
    c->r[R_EAX] = (uint32_t)tsc;
    c->r[R_EDX] = (uint32_t)(tsc >> 32);
}

// Deterministic CPUID per the plan: GenuineIntel, family 6, model 3, and only
// FPU, TSC and CMOV. No MMX, so the guest cannot pick a path the translator
// does not cover.
void recomp_cpuid(X86 *c) {
    switch (c->r[R_EAX]) {
    case 0:
        c->r[R_EAX] = 1;
        memcpy(&c->r[R_EBX], "Genu", 4);
        memcpy(&c->r[R_EDX], "ineI", 4);
        memcpy(&c->r[R_ECX], "ntel", 4);
        break;
    case 1:
        c->r[R_EAX] = 0x00000633; // family 6, model 3, stepping 3
        c->r[R_EBX] = 0;
        c->r[R_ECX] = 0;
        c->r[R_EDX] = 0x00008011; // FPU | TSC | CMOV, nothing else
        break;
    default:
        c->r[R_EAX] = c->r[R_EBX] = c->r[R_ECX] = c->r[R_EDX] = 0;
        break;
    }
}

uint32_t recomp_in(X86 *c, uint32_t port, int size) {
    (void)c;
    char key[48];
    snprintf(key, sizeof key, "in:%04x", port);
    log_once(key, "IN from port %04x (%d bytes): returning 0", port, size);
    return 0;
}

void recomp_out(X86 *c, uint32_t port, uint32_t val, int size) {
    (void)c;
    char key[48];
    snprintf(key, sizeof key, "out:%04x", port);
    log_once(key, "OUT to port %04x value %08x (%d bytes): ignored", port, val, size);
}

void recomp_cli(X86 *c) {
    (void)c;
    log_once("cli", "CLI ignored");
}
void recomp_sti(X86 *c) {
    (void)c;
    log_once("sti", "STI ignored");
}
void recomp_hlt(X86 *c) {
    (void)c;
    log_once("hlt", "HLT ignored");
}

void recomp_int(X86 *c, uint32_t vec) {
    LOGW("INT %02x at EIP %08x: no interrupt handling, continuing", vec, c->eip);
}

// ---------------------------------------------------------------------------
// _setjmp / _longjmp intrinsics. See intrinsics.h for the two substitution
// forms and why the two-call one is correct.
// ---------------------------------------------------------------------------
jmp_buf *recomp_setjmp_prepare(X86 *c) {
    uint32_t buf = rd32(c->r[R_ESP] + 4); // cdecl argument 0
    SetjmpRecord *&rec = setjmps()[buf];
    if (!rec)
        rec = new SetjmpRecord();
    rec->buf = buf;
    rec->saved = *c;
    rec->profile_depth = recomp_profile_depth();
    rec->armed = true;
    g_setjmp_ctx = c;
    // Leave a marker in the guest jmp_buf so a stale buffer is recognisable.
    if (buf) {
        wr32(buf, 0x4d504f50u /* 'POPM' */);
        wr32(buf + 4, buf);
    }
    LOGV("_setjmp(%08x): saved guest state, ESP=%08x", buf, c->r[R_ESP]);
    return &rec->env;
}

void recomp_setjmp_return(X86 *c, int value) {
    uint32_t ret = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4; // cdecl: emulate RET, the caller pops the argument
    c->eip = ret;
    c->r[R_EAX] = (uint32_t)value;
}

// Deliberately fatal. Taking the host setjmp here would save a frame that has
// returned by the time _longjmp fires, so the only safe substitution is the
// two-call form the translator now emits at the call site.
void recomp_setjmp(X86 *c) {
    LOGW("_setjmp reached the single-call intrinsic at ESP=%08x. The call site must emit "
         "{ jmp_buf *b = recomp_setjmp_prepare(c); recomp_setjmp_return(c, setjmp(*b)); } "
         "so the host setjmp belongs to a frame that is still live when _longjmp runs.",
         c->r[R_ESP]);
    abort();
}

void recomp_longjmp(X86 *c) {
    uint32_t buf = rd32(c->r[R_ESP] + 4);
    int value = (int)rd32(c->r[R_ESP] + 8);
    auto it = setjmps().find(buf);
    if (it == setjmps().end() || !it->second->armed) {
        LOGW("_longjmp(%08x, %d): no matching _setjmp, cannot unwind", buf, value);
        abort();
    }
    if (value == 0)
        value = 1; // longjmp(buf, 0) makes setjmp return 1
    SetjmpRecord *rec = it->second;
    // A guest longjmp jumps over every host frame between here and the
    // setjmp, including any mod hook invocation sitting on them. Tell the mod
    // layer so those invocations are abandoned rather than left to run their
    // after hooks against a stack that no longer exists. Before the registers
    // are restored, because the ESP being unwound to is the saved one.
    mods_hooks_unwind_to_esp(rec->saved.r[R_ESP]);
    recomp_profile_truncate(rec->profile_depth);
    *c = rec->saved; // guest registers as they were at the _setjmp
    g_setjmp_ctx = c;
    LOGV("_longjmp(%08x, %d): restoring ESP=%08x", buf, value, c->r[R_ESP]);
    longjmp(rec->env, value);
}

} // extern "C"
