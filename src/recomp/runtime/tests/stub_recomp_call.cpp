// stub_recomp_call.cpp - test-only recomp_call.
//
// In the real build build/recomp/gen/table.c defines recomp_call: it binary
// searches the generated function table and, for addresses in the trampoline
// range, calls imports_dispatch(). The runtime tests link this stand-in so they
// can exercise the shims without the generated code.
#include "../imports.h"
#include <stdio.h>

extern "C" void recomp_call(X86 *c, uint32_t target) {
    if (imports_dispatch(c, target))
        return;
    fprintf(stderr, "[stub] recomp_call to %08x: no generated function table in this build\n",
            target);
    c->r[R_EAX] = 0;
}
