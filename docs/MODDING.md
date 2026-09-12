# Modding the recompiled Populous: The Beginning

This is the reference for the mod foundation: what a mod is, what it can reach,
what it cannot, and what the host promises about the order things happen in.

A mod is a directory with a `mod.toml` in it. It may ship a native plugin, a
Lua script, a tree of asset files, or any combination. The five examples under
`mods/examples/` are working mods of each kind and are referred to throughout.

The translated game code is immutable. Nothing here patches an instruction:
mods are called through a dispatch layer the build emits, and a build with no
mods installed runs the same bytes as a build without the foundation at all.

---

## 1. Manifest reference

`mod.toml` is read by the loader before anything is opened. A manifest that
does not parse, or that fails any rule below, rejects the mod with a reason
that appears in the log and in the run record.

### Top-level keys

| key | type | required | default | rule |
| --- | --- | --- | --- | --- |
| `id` | string | yes | - | The mod's identity everywhere else: in `requires`, in `conflicts`, in settings keys, in the run record. Reverse-DNS style by convention (`example.logger`). Two mods with the same `id` are a duplicate; the first one discovery reached is kept and the second is rejected. |
| `name` | string | yes | - | Shown to a person. Never used to identify the mod. |
| `version` | string | yes | - | A semantic version: `major.minor.patch`, each component numeric and without a leading zero, with an **optional** prerelease tag after `-` and optional build metadata after `+` which is ignored. `1.0.0` and `1.0.0-beta.1` are both accepted; `1.0`, `1.2.banana`, `v1.0.0`, `1.0.0+` and `1.0.0-alpha..1` are rejected. Precedence is SemVer's: `major`, then `minor`, then `patch`, and **a version with a prerelease tag is lower than the same version without one**, with prerelease identifiers compared dot by dot, numeric ones as numbers and alphanumeric ones as text. So `1.0.0-beta < 1.0.0`, and `requires = ["x >= 1.0.0"]` is not satisfied by `1.0.0-beta`. |
| `api` | integer | yes | - | The API generation the mod was written against. This foundation is `1`. A mod declaring anything else is rejected rather than loaded and hoped for. |
| `game` | string | yes | - | The SHA-256 of the guest executable the mod was built against, either in full or as a prefix of **at least 16 hex characters**. The supported executable is `815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd`, so `game = "815ba8a550f571c3"` is the shortest form that is accepted. A shorter prefix is rejected, because it would match executables the mod has never seen. |
| `requires` | array of strings | no | `[]` | Each entry is `id op semver`, with `op` one of `>=`, `=` or `<`, for example `"core.overlay >= 1.0.0"`. A requirement that names nothing present rejects the mod. |
| `conflicts` | array of strings | no | `[]` | Ids this mod refuses to run beside. When both are present the one **later in resolved load order** is rejected, whichever of the two declared the conflict: the winner is the mod that would have loaded first, so the outcome does not depend on which side wrote the line. A mod that loses a conflict stops participating immediately, so it cannot go on to reject a third mod it also conflicts with. |
| `affects_simulation` | boolean | no | `false` | A declaration, not a switch. See section 6. |

### Sections

```toml
[plugin]
path = "logger.dylib"      # relative to the mod directory

[script]
path = "main.lua"          # relative to the mod directory

[assets]
path = "assets"            # the root of this mod's overlay layer

[settings]
ui_scale   = { type = "int",  default = 2, min = 1, max = 4, label = "UI scale" }
show_hints = { type = "bool", default = true, label = "Show hints" }
```

- `[plugin]` and `[script]` may both be present; the plugin's `pop_mod_init`
  runs first, then the script.
- `[assets] path` is the root of the mod's overlay layer. A file at
  `<path>/data/mods-example.txt` answers the guest path `data\mods-example.txt`:
  the shape under the root is the shape the game asks for.
- `[settings]` entries are inline tables, and every field in one is optional.
  The defaults are: `type` is `"int"` (any value other than `"bool"` reads as
  `int`), `default` is `0`, `min` is `0`, `max` is `0`, and `label` is the key.
  A setting with no `min` and no `max` therefore admits only the value 0, which
  is a declaration nobody means: give an `int` a range. `default` is a number
  for an `int` and a **boolean** for a `bool` (`default = true`, not
  `default = 1`). **A `bool`'s range is `0..1` whatever the manifest says**, and
  its default is normalised to 0 or 1.
- `[plugin] path`, `[script] path` and `[assets] path` each default to absent,
  and a section whose `path` is missing or is not a string is the same as no
  section at all. A mod with none of the three is legal: it loads, declares its
  settings and does nothing else.

A setting may also be declared from code with `register_setting`; both forms
end up in the same store, under the same `<mod id>/<key>` name.

---

## 2. Load order and lifecycle

### Discovery

The loader reads every immediate subdirectory of the mods directory
(`POPM_MODS_DIR`, default `mods/`) that contains a `mod.toml`. Discovery order
is the directory order, which only matters for breaking duplicate-id ties.

### Built-in capabilities

Three ids are always present at version `1.0.0` and are not directories:

| capability | what requiring it means |
| --- | --- |
| `core.hooks` | the hook and symbol API is available |
| `core.overlay` | the asset overlay is available |
| `core.lua` | the Lua runtime is linked in, so a `[script]` mod can run |

A mod that uses hooks should say `requires = ["core.hooks >= 1.0.0"]`. It costs
nothing and turns "my mod does nothing" into a message naming what is missing.

### Resolution

In this order, so each stage sees a clean input:

1. **Duplicates** are pruned, keeping the first.
2. **Requirements** are checked and unsatisfiable mods pruned, transitively:
   a mod that requires a rejected mod is itself rejected, with the chain in its
   reason.
3. The survivors are **sorted** topologically, with ties broken by `id`
   ascending, so the order is the same on every machine.
4. **Conflicts** are applied to the sorted set.
5. What remains is pruned and sorted once more, so a mod dropped in step 4
   takes its dependents with it.

### Lifecycle

1. **Discovery and resolution**, above.
2. **The Lua runtime's own init**, before any user mod, so a `[script]` mod
   never runs before the interpreter exists.
3. **Each mod's init, in load order.** Every mod gets its own `PopModApi`
   instance carrying its identity; every registration is attributed to it.
4. **The run.**
5. **Shutdown**, in reverse load order: each `pop_mod_exit`, then the Lua
   runtime last. Plugins are unloaded only after every `pop_mod_exit` has run
   and no guest thread is inside a callback, so a `dlclose` can never pull the
   ground from under a running hook.

### Four guarantees about when a change takes effect

- **A pending operation is registry state, not a promise.** An install or a
  removal made from a thread that does not hold the scheduler's baton is
  validated immediately - it returns the real status, `POP_OK` or the error -
  and is queued for publication. Until it is published it is part of what the
  registry **says**, though not yet of what it **does**: a second install of a
  conflicting `replace` is refused against committed plus pending state rather
  than being accepted twice, and a queued removal is taken into account by that
  validation. The hook itself keeps running until the checkpoint publishes the
  removal - dispatch changes at publication, not at the call that asked for it.
- **Publication happens at a checkpoint**, under the baton, so the dispatch
  tables never change under a running dispatcher.
- **What a mod's init registers is applied before the guest starts.** After
  every init has run and before guest entry, the queue is published once
  directly: no guest thread exists yet and nothing is dispatching, which is the
  only moment where that is safe. A hook installed in `pop_mod_init` therefore
  sees the first frame, rather than missing everything up to the first
  checkpoint. Asked at any other time that direct publication refuses and
  leaves the queue for the checkpoint.
- **Loading happens once.** A second `mods_load_all` in a production run
  **returns `false`** and changes nothing; `POP_E_STATE` appears in its log
  line, not in its return value, because it returns a boolean. There is no
  reload: plugins are unloaded at shutdown and not before.
- **A failed init is reclaimed as soon as it is safe.** Everything it
  registered (hooks, events, input, overlay layers, settings) is removed at
  once. Its memory and its Lua state go the moment nothing of it can still be
  running - immediately, in the usual case where no guest thread is inside one
  of its callbacks, and at shutdown otherwise. What necessarily waits for
  shutdown is only the loaded module itself, which stays mapped.

### What `pop_mod_exit` may do

**A mod's API is live throughout its own `pop_mod_exit`**, and is revoked the
moment that function returns. An exit handler may read and write its settings,
and a value written there reaches the profile on disk: persisting state at exit
is the supported thing to do, not a thing to work around.

**After `pop_mod_exit` returns the context is revoked**, including for a call
through a function pointer the mod saved earlier, because the revocation is on
the context and not on the copy of the pointer. So the exit handler is the last
place a mod may *use* its API, not merely the last place it is called.

What a revoked call gives back depends on what it returns. A call that returns
a `PopModStatus` returns `POP_E_STATE` and does nothing. A call that returns a
value has no way to say that, so it returns a defined default instead: `""`
from `mod_dir`, `0` from `entity_count`. A mod that keeps calling after its
exit therefore sees `POP_E_STATE` from every status-returning call and an
empty, uneventful world from the accessors, which is another reason not to.

Shutdown does three things per mod, in this order and separately:

1. **exit** - `pop_mod_exit` runs, with the API live;
2. **revoke** - the context is revoked, and every later call is refused;
3. **reclaim** - hooks, subscriptions, settings, layers and the mod's guest
   allocations are released.

They are three steps for three reasons: the exit needs the API, nothing after
the exit may use it, and reclaiming before either would pull the ground from
under both. A mod's `guest_alloc` blocks are therefore **still valid inside
`pop_mod_exit`**, which is where a mod that wants to free them explicitly
should do it.

Removing hooks at exit is still not required - reclaim removes everything the
mod registered - but it now works if you do it.

`mods/examples/settingsmenu` reads both of its settings from inside
`pop_mod_exit` and writes what it read; if the API were dead there, the file it
leaves would say `-1`.

### The transaction

Each mod's init runs in a transaction. If `pop_mod_init` returns anything other
than `POP_OK`, or the plugin is unusable, everything that init registered is
rolled back: hooks, event subscriptions, input handlers, settings declarations
and their persisted values, overlay layers, menu entries, host services and any
guest memory it allocated through `guest_alloc`.

**The documented gap:** guest memory a mod *wrote* through `guest_write_u8`,
`guest_write_u16` or `guest_write_u32` is not restored. Rollback returns the
registry to its previous state; it does not journal the game's own memory. A
mod that writes to the game during init and then fails has changed the game.
Do the writing from a hook or an event, where a failure has a smaller blast
radius.

---

## 3. The API by group

Every call takes its `const PopModApi*` as the first argument, including the
ones that look like they would not need it: that pointer is the mod's identity,
and it is how the host attributes what happens next.

A plugin declares its ABI once and exports two entry points:

```c
#include "pop_mod_api.h"

POP_MOD_DECLARE_ABI();

PopModStatus pop_mod_init(const PopModApi* api);
PopModStatus pop_mod_exit(void);
```

`POP_MOD_DECLARE_ABI()` records `sizeof(PopModAbi)`, `POP_MOD_API_VERSION`,
`sizeof(PopModApi)` and `sizeof(pop_cpu_v1)` as they were when the plugin was
compiled. A plugin without it is refused with `POP_E_ABI`; a plugin built
against an older header still works, and every copy in and out of its CPU
structure is bounded by the smaller of the two sizes.

### Status codes

`POP_OK` is 0. Everything else is negative: `POP_E_NOSYMBOL`,
`POP_E_CONFLICT`, `POP_E_REENTRY`, `POP_E_LIMIT`, `POP_E_STATE`,
`POP_E_WRONG_THREAD`, `POP_E_RANGE`, `POP_E_NOMEM`, `POP_E_INVAL`,
`POP_E_NOTFOUND`, `POP_E_ABI`.

### Identity and logging

```c
PopModStatus (*log)(const PopModApi* api, const char* text);
const char*  (*mod_dir)(const PopModApi* api);
PopModStatus (*settings_get)(const PopModApi* api, const char* key, int64_t* out);
PopModStatus (*settings_set)(const PopModApi* api, const char* key, int64_t v);
```

`api->log` prefixes the line with the mod's id, so a log is attributable
without the mod having to remember to say who it is.

### Hooks and symbols

```c
PopModStatus (*hook_install)(const PopModApi* api, uint32_t addr, PopHookFn fn,
                             int32_t mode, void* user, uint32_t* out_id);
/* Optional appended v1 service; check api->size and the pointer first. */
PopModStatus (*hook_install_at_callsite)(const PopModApi* api, uint32_t addr,
    uint32_t return_pc, PopHookFn fn, int32_t mode, void* user, uint32_t* out_id);
PopModStatus (*hook_remove)(const PopModApi* api, uint32_t id);
PopModStatus (*call_original)(const PopModApi* api, uint32_t addr, pop_cpu_v1* cpu);
PopModStatus (*call_next)(const PopModApi* api, PopHookInvocation* inv, pop_cpu_v1* cpu);
PopModStatus (*hook_return)(const PopModApi* api, pop_cpu_v1* cpu,
                            uint32_t eax, uint32_t arg_bytes);
PopModStatus (*symbol)(const PopModApi* api, const char* name, uint32_t* out_addr);
PopModStatus (*symbols_matching)(const PopModApi* api, const char* prefix,
                                 uint32_t* out_addrs, uint32_t cap,
                                 uint32_t* out_count);
```

`hook_install_at_callsite` filters on the exact nonzero return PC at the top
of the guest stack when the function is entered. This is the instruction
after the call, not the calling function's entry address. Verify it against
the pinned executable. The host captures the match before any BEFORE hook
runs and uses that decision for all phases, even if a callback changes the
stack or return address. Nonmatching calls do not enter the callback or copy
its game-view snapshot. Matching callbacks retain normal snapshot semantics.
Ordering, chain limits, replacement conflicts, queued registration, owner
cleanup and `hook_remove` work as for `hook_install`. Zero is invalid for this
service; use `hook_install` for an unfiltered hook. The field is appended at
offset 352 on the native 64-bit ABI; older plugins retain their existing layout.

`api->symbol` resolves a name from `build/recomp/symbols.json` - a function
name, a curated alias or a curated global - to a guest address. Ask for a
symbol by name; do not paste an address, because an address is a fact about one
build of the translator and a name is a fact about the game.

**Not every symbol is hookable.** `symbols.json` marks each one. The hookable
set is the listed function starts and the alternate entries a relocated pointer
names: 4529 of 10724 symbols in the current build. The rest - recovered blocks,
jump-table targets, listing-gap continuations and the two intrinsic
substitutions - can be dispatched to but not hooked, and `hook_install` returns
`POP_E_NOSYMBOL` for them. **A known limitation of v1:** 956 functions that the
translator recovered from the executable and that a relocated pointer names are
real single-entry functions, but they are not in the listing the symbol index
was built from, so they are classified as blocks and are not hookable. If you
need one, say so and it can be curated by name.

### Guest memory

```c
PopModStatus (*guest_ptr)(const PopModApi* api, uint32_t addr, uint32_t len, void** out);
PopModStatus (*guest_read_u8)(const PopModApi* api, uint32_t a, uint8_t* v);
PopModStatus (*guest_read_u16)(const PopModApi* api, uint32_t a, uint16_t* v);
PopModStatus (*guest_read_u32)(const PopModApi* api, uint32_t a, uint32_t* v);
PopModStatus (*guest_write_u8)(const PopModApi* api, uint32_t a, uint8_t v);
PopModStatus (*guest_write_u16)(const PopModApi* api, uint32_t a, uint16_t v);
PopModStatus (*guest_write_u32)(const PopModApi* api, uint32_t a, uint32_t v);
PopModStatus (*guest_alloc)(const PopModApi* api, uint32_t size, uint32_t* out_addr);
PopModStatus (*guest_free)(const PopModApi* api, uint32_t addr);
```

Every address is bounds-checked and out-of-range reads return `POP_E_RANGE`
rather than reading something. `guest_alloc` allocates from a **mod heap that is
not the game's heap**, so a mod's allocation can never move the game's, and a
leak is a mod's leak.

### The game view, read-only

```c
uint32_t     (*entity_count)(const PopModApi* api);
PopModStatus (*entity_slot)(const PopModApi* api, uint32_t nth, uint32_t* out_slot);
PopModStatus (*entity)(const PopModApi* api, uint32_t slot, PopEntityView* out);
PopModStatus (*tribe)(const PopModApi* api, uint32_t i, PopTribeView* out);
```

The view is an immutable snapshot taken when the callback was entered and valid
until it returns. `entity_slot(nth)` gives the **physical slot** of the `nth`
allocated entity, and `entity(slot)` takes a physical slot - `slot` and the
entity's own `id` are different numbers, and iterating ids and indexing by them
reads the wrong entities. **An entity is allocated when its kind byte at
offset 42 is non-zero.**

Display callbacks that only use CPU registers, direct guest-memory access or
host services can explicitly opt out of the entity/tribe snapshot with the
optional v1 tail `hook_install_ex(api, addr, return_pc, fn, mode, flags, user,
out_id)`. A zero return PC disables filtering; `flags=0` preserves ordinary
snapshot behavior. `POP_HOOK_NO_GAME_VIEW` suppresses snapshot capture for that
callback. It returns an entity count of zero and `POP_E_STATE` from valid
entity/slot/tribe queries, even when an enclosing callback has a view. Nested
ordinary callbacks still capture their own entry snapshots, and unwinding or
returning restores the enclosing scope. Unknown flags are rejected. Check
`api->size` through `hook_install_ex` before accessing this optional field;
existing installation APIs retain their snapshot behavior.

### Events

```c
PopModStatus (*on_frame)(const PopModApi* api, int32_t phase, PopEventFn fn,
                         void* user, uint32_t* out_id);
PopModStatus (*on_turn)(const PopModApi* api, int32_t phase, PopEventFn fn,
                        void* user, uint32_t* out_id);
PopModStatus (*on_level_load)(const PopModApi* api, PopEventFn fn, void* user,
                              uint32_t* out_id);
PopModStatus (*on_level_end)(const PopModApi* api, PopEventFn fn, void* user,
                             uint32_t* out_id);
PopModStatus (*on_key)(const PopModApi* api, PopKeyFn fn, void* user, uint32_t* out_id);
PopModStatus (*on_mouse)(const PopModApi* api, PopMouseFn fn, void* user, uint32_t* out_id);
```

`phase` is `POP_EVENT_BEFORE` or `POP_EVENT_AFTER`. A key or mouse handler
returning non-zero **consumes** the input: the guest sees it through none of
DirectInput's immediate state, DirectInput's buffered data, a posted Win32
message or `GetAsyncKeyState`. A consumed press also consumes **its repeats and
its release**, so the guest is never left believing a key is down that it never
saw go down.

### Host services

```c
PopModStatus (*register_menu_item)(const PopModApi* api, const char* path,
                                   const char* label, PopMenuFn cb, void* user);
PopModStatus (*register_setting)(const PopModApi* api, const PopSettingDesc* desc);
PopModStatus (*texture_override_provider)(const PopModApi* api,
                                          PopTextureProviderFn cb, void* user);
PopModStatus (*overlay_push)(const PopModApi* api, const char* dir, uint32_t* out_id);
PopModStatus (*open_settings_page)(const PopModApi* api, const char* mod_id);
```

These are main-thread only and return `POP_E_WRONG_THREAD` anywhere else.
`overlay_push` is valid **only during the mod's own `pop_mod_init`**: layer
order is load order, and a layer pushed later would have an order nothing
declared.

---

## 4. Hook modes, with worked examples

```c
typedef void (*PopHookFn)(const PopModApi* api, pop_cpu_v1* cpu,
                          PopHookInvocation* inv, void* user);
```

| mode | when it runs | the base function |
| --- | --- | --- |
| `POP_HOOK_BEFORE` | before anything else | still runs |
| `POP_HOOK_AFTER` | after everything else | already ran |
| `POP_HOOK_REPLACE` | instead of the function body | runs only if you delegate |
| `POP_HOOK_WRAP` | around the body and any inner replace or wrap | runs when you delegate |

**The order, exactly.** One invocation runs:

1. every `before` hook, in load order;
2. the replace/wrap chain, outermost `wrap` first, each delegating inward with
   `call_next` until the innermost delegates to the base with `call_original` -
   or the base directly, if no mod replaced or wrapped this address;
3. every `after` hook, in reverse load order.

So `wrap` does **not** wrap the `before` and `after` hooks: those run outside
the chain, before it and after it. A wrap sees the body and anything a later
mod put between it and the body, which is what makes two wraps compose.

`replace` and `wrap` share one chain, and **a `replace` is refused with
`POP_E_CONFLICT` whenever that chain is not empty** - including when the only
thing in it is a `wrap`, and whether or not anything in it delegates. A
replacement wants the chain to itself; a wrap is already in it. Delegation does
not enter into it: what is checked is the chain, at install time. A `replace`
that delegates and is then re-entered within the same call is a different
matter and returns `POP_E_REENTRY`, rather than running the base twice.

### before

Runs on the way in, with the arguments still on the stack and the guest `RET`
not yet performed. It cannot change whether the body runs; to do that, replace
it.

```c
static void count_turns(const PopModApi* api, pop_cpu_v1* cpu,
                        PopHookInvocation* inv, void* user)
{
    (void)api; (void)inv; (void)user;
    ++g_turns;                    /* cpu->esp + 4 is the first argument */
}
/* in pop_mod_init: */
api->hook_install(api, addr, count_turns, POP_HOOK_BEFORE, 0, &g_hook);
```

### after

Runs once the body has returned. `cpu->eip` is the caller's return address by
now, and **only `eax` and `edx` may be edited** - here the result is clamped.

```c
static void clamp_result(const PopModApi* api, pop_cpu_v1* cpu,
                         PopHookInvocation* inv, void* user)
{
    (void)api; (void)inv; (void)user;
    if (cpu->eax > 100u) cpu->eax = 100u;
}
/* in pop_mod_init: */
api->hook_install(api, addr, clamp_result, POP_HOOK_AFTER, 0, &g_hook);
```

### wrap

Sees both sides of the body in one callback. `call_next` runs the rest of the
chain - the next wrap, the replacement, or the base - and performs the guest
`RET`, so a wrap that delegates must **not** call `hook_return` either.

```c
static void time_it(const PopModApi* api, pop_cpu_v1* cpu,
                    PopHookInvocation* inv, void* user)
{
    uint64_t t0 = now_ns();
    (void)user;
    api->call_next(api, inv, cpu);          /* the rest of the chain, then RET */
    g_total_ns += now_ns() - t0;            /* cpu->eax is the result */
}
/* in pop_mod_init: */
api->hook_install(api, addr, time_it, POP_HOOK_WRAP, 0, &g_hook);
```

### replace

#### The delegating case

`call_original` and `call_next` run the base (or the next hook in the chain)
**and perform the guest `RET`**. A hook that delegates must therefore **not**
call `hook_return`: the return has already happened, and calling it again
would unwind a frame that is no longer there.

`mods/examples/constant/constant.c`, in full:

```c
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

static const PopModApi* g_api;
static uint32_t g_hook;
static unsigned g_replaced, g_substituted;

static void replace_thunk(const PopModApi* api, pop_cpu_v1* cpu,
                          PopHookInvocation* inv, void* user)
{
    int64_t want = 2;
    uint8_t arg = 0;
    (void)inv; (void)user;
    ++g_replaced;
    api->settings_get(api, "default_set", &want);
    /* The argument is the byte at [ESP+4], since the guest RET has not run. */
    if (api->guest_read_u8(api, cpu->esp + 4, &arg) == POP_OK && arg == 0) {
        api->guest_write_u8(api, cpu->esp + 4, (uint8_t)want);
        ++g_substituted;
    }
    api->call_original(api, cpu->target, cpu);
}

PopModStatus pop_mod_init(const PopModApi* api)
{
    uint32_t addr = 0;
    g_api = api;
    if (api->symbol(api, "level_startup_thunk", &addr) != POP_OK) return POP_E_NOSYMBOL;
    return api->hook_install(api, addr, replace_thunk, POP_HOOK_REPLACE, 0, &g_hook);
}

PopModStatus pop_mod_exit(void)
{
    const char* dir = getenv("POPM_PROFILE_DIR");
    char path[512];
    FILE* f;
    snprintf(path, sizeof path, "%s/example-constant.txt",
             dir && *dir ? dir : "build/recomp/profile");
    f = fopen(path, "wb");
    if (f) { fprintf(f, "replaced %u\nsubstituted %u\n", g_replaced, g_substituted); fclose(f); }
    return g_api->hook_remove(g_api, g_hook);
}
```

#### The non-delegating case

A `replace` hook that does not delegate has to perform the return itself:

```c
static void never_load(const PopModApi* api, pop_cpu_v1* cpu,
                       PopHookInvocation* inv, void* user)
{
    (void)inv; (void)user;
    api->hook_return(api, cpu, 0u, 0u);   /* EAX = 0; load_objs_1 is cdecl */
}
```

`hook_return(cpu, eax, arg_bytes)` sets the result and performs the guest
`RET`. `arg_bytes` is the **callee's own** cleanup size, and the function being
replaced is what decides it. **Read the callee's last instruction:**

| the callee ends with | convention | `arg_bytes` |
| --- | --- | --- |
| `RET` | `cdecl`: the caller cleans up | `0` |
| `RET 0x8` | `stdcall`: the callee cleans up | the immediate, `8` |

`load_objs_1` ends `0040c683 RET`, with no immediate, so it is caller-cleaned
and `arg_bytes` is **0** even though it takes one dword argument. Taking the
argument count for the cleanup size is the mistake to avoid: the two are
unrelated, and a wrong value moves the caller's stack pointer by that many
bytes and corrupts the frame it returns into.

Find the form the same way the address was found: look the symbol up in
`build/recomp/symbols.json` and read the last instruction of its listing under
`analysis/decompiled/D3DPopTB.exe/functions/<addr>.asm`. The API cannot work
this out for you, because by the time a hook runs the return has not happened
and there is nothing to observe.

---

## 5. CPU state and calling conventions

```c
typedef struct pop_cpu_v1 {
    uint32_t size;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t eip;
    uint32_t target;
    uint32_t phase;
    uint32_t cf, zf, sf, of, pf, af, df;
    double   st[8];
    uint32_t fpu_top;
    uint16_t fpu_cw, fpu_sw, fpu_tag;
    uint16_t reserved0;
} pop_cpu_v1;
```

`pop_cpu_v1` is versioned **separately** from `PopModApi`: `POP_MOD_DECLARE_ABI`
records both sizes, and the host transfers only the bytes both sides have.

`target` is the address that was hooked and `phase` says which phase is
running. **`eip` means different things by phase**: for `before` and `replace`
it is the callee's entry address, normalised by the host; for `after` it is the
caller's return address, because the guest `RET` has already run. `target` is
what identifies the hooked function once `eip` has moved on.

**What a callback may edit is also phase-specific.** A `before` or `replace`
callback may edit any field, and every field inside `size` is copied back. An
`after` callback may edit **only `eax` and `edx`** - the frame it would have
been editing is gone. Memory edits through `guest_read_*`/`guest_write_*` apply
directly in every phase. `size` is set by the host from the size the plugin
declared, and a callback must never enlarge it.

**Flags are as observed, and may be stale.** The translator keeps a flag only
where liveness analysis found a later read of it, so `cf`, `zf`, `sf`, `of`,
`pf` and `af` carry a meaningful value only where the original code was about
to use one. Do not branch on a flag that the function under the hook was not
itself about to test. `df` and the FPU fields - `st`, `fpu_top`, `fpu_cw`,
`fpu_sw`, `fpu_tag` - are always live and always meaningful.

Arguments are on the guest stack. During a `before` or `replace` hook the guest
`RET` has not run, so the first dword argument is at `esp + 4`, the second at
`esp + 8`, and so on.

---

## 6. Determinism and the run record

Determinism is **declared, never enforced**.

`affects_simulation` is informational. Nothing checks it, nothing gates on it,
and a mod that declares `false` and then writes to the simulation is not
stopped - it is recorded. Every run writes a record naming the mod set, each
mod's version, its payload hash and what it declared, so a difference between
two runs can be attributed rather than argued about.

No claim is made that a mod which declares `affects_simulation = false` cannot
change the output. Native writes, allocation shifts and timing can all affect a
run regardless of what a manifest says.

### What the record contains

Every run writes `build/recomp/mods/run.json`, or the path in `POPM_RUN_RECORD` when that is set, whether or not any mod loaded
and whether or not mods were enabled: a run with none records an **explicit
empty set**, so "no mods ran" and "nothing wrote a record" are different things
in the output. It holds:

| field | what it is |
| --- | --- |
| `exe_sha256` | the guest executable this build translated |
| `generated_archive`, `symbols` | the generated archive and symbol index, hashed **as they were when the run started** |
| `build_identity_source` | where those two hashes came from. Always `"archive-at-start"` in this version; see below |
| `override_count`, `override_set` | which translated functions this binary replaced, from its own tables rather than from any file |
| `pins` | the fixture, thread and frame pins as they were at start, and `clock`, which is what the host actually installed rather than a pin at all |
| `input` | the input script's path, size, hash and **its full text**, so a replay can feed the same steps back |
| `initial_settings` | the settings file as it was at start, in full |
| `mods_enabled`, `mods_dir` | whether the **environment** left mods enabled, and where they were looked for |
| `mods` | each loaded mod's id, version, load order, payload identity and `affects_simulation` |
| `settings` | every declared setting's value at the end of the run |
| `rejected` | each rejected mod and the reason |

**The build identity is the archive on disk at process start, not the archive
this binary was linked against.** A running binary cannot ask itself which
bytes it was linked from, so the record hashes `build/recomp/librecomp_gen.a`
and `build/recomp/symbols.json` as its very first act and reports
`build_identity_source: "archive-at-start"` to say exactly that. The two differ
only if the archive is rebuilt between linking a binary and launching it. Every
build takes the build lock for as long as it uses those files, so this does not
happen to a binary already built, but it is a limitation of this version rather
than a guarantee, and the field is there so a reader comparing two runs knows
what was compared.

**`POPM_RUN_RECORD` gives a run a record of its own.** Every host writes the
same fixed path by default, so two runs at once write the same file and the
same temporary beside it, and a reader gets whichever finished last. Setting
this variable per run removes the collision at its source: a run given its own
path cannot be reached by a run that did not agree to a lock. Gate B, the mod
example checks and the smoke make targets each set it, which is why two of them
can run at the same time without one asserting against the other's record.

**`pins.clock` names the clock the run ran on, and it is not an environment
variable.** The parity fixture pins time to a counter and reports
`pinned start=100 step=50`; every boot-based host, the smoke host included,
reads the real monotonic clock and reports `monotonic`. Two runs are replays of
each other only if this matches, and Gate B compares it between its two runs of
each host.

It is read when the record is written rather than at process start, because a
host installs its time source from `main`, after the record's own capture has
already run. An earlier version recorded a `clock_ms` field taken from
`POP_RECOMP_CLOCK_MS`, which no host, runtime file or tool read: setting it
changed nothing, and two runs matching on it said nothing about their clocks
agreeing.

**`mods_enabled` reports the environment, not the host's own choice.** Setting
`POPM_NO_MODS` disables mods, whatever the value: `POPM_NO_MODS=` disables them
exactly as `POPM_NO_MODS=1` does, because every host asks whether the variable
is *present*. A host may also disable mods through its own options, which the
record cannot see - it is written from a capture taken before any host exists.
When the distinction matters, read the `mods` array: it is empty for a run in
which nothing loaded, whichever way it was turned off.

**A payload's identity is the one the run ran.** Each mod directory is hashed
in full - manifest, plugin, script and assets, including dot-prefixed files,
which the overlay serves - when the loader commits the mod, after its init has
succeeded (`payload_at: "load"`). Replacing a file on disk after that does not
change what the run recorded. `"shutdown"` appears only where nothing captured
the mod, and says plainly that its identity was taken late and describes the
files as they were at the end rather than as they ran.

The record is written to a temporary file and renamed into place, so a reader
sees the previous record or this one and never half of one, and every string in
it is escaped losslessly: control bytes, quotes in a rejection reason, and
bytes that are not valid UTF-8 all survive as `\u00XX` escapes rather than
making the file unparseable.

---

## 7. Asset overlay, with `mods/examples/overlay`

The overlay answers the game's own file requests. It has three tiers, searched
in this order:

1. **the profile directory** - where everything a mod or the game writes lands
2. **mod layers**, in reverse load order - the last mod loaded wins
3. **the original game data**

`mods/examples/overlay` ships one file:

```
mods/examples/overlay/
  mod.toml                       [assets] path = "assets"
  assets/data/mods-example.txt   "overlay layer reached"
```

The layer's root is `assets/`, so `assets/data/mods-example.txt` answers the
guest path `data\mods-example.txt`. The smoke script proves it through the
game's own file API rather than by looking at the file on disk, which would
prove only that the file exists:

```
readfile data\mods-example.txt overlay
```

Rules:

- **Writes only ever land in the profile directory.** Nothing a mod or the game
  does can modify a mod's shipped assets or the original game data.
- **A write to a lower-tier file copies it up first.** The file is copied into
  the profile, the write applies there, and every later read sees the profile
  copy.
- **v1 has no tombstones.** A mod cannot hide a file that a lower tier
  provides; it can only shadow it with its own. Deleting a shadow reveals the
  file underneath rather than removing it.

---

## 8. Lua, with `mods/examples/luawalk`

A `[script]` mod gets its own interpreter, its own globals and the `pop` table.
The binding surface is curated by hand - there is no automatic forwarding, so
nothing appears in it by accident:

| binding | what it does |
| --- | --- |
| `pop.log(text)` | a log line attributed to this mod |
| `pop.mod_id()` | this mod's id |
| `pop.settings_get(key)` / `pop.settings_set(key, value)` | this mod's settings |
| `pop.guest_read_u8/u16/u32(addr)` | a bounds-checked read, `nil` out of range |
| `pop.symbol(name)` | a symbol's address, or `nil` |
| `pop.entity_count()`, `pop.entity_slot(nth)`, `pop.entity(slot)` | the read-only view |
| `pop.tribe(i)` | one of the four tribe records |
| `pop.paused()`, `pop.simulation_turn()`, `pop.command_frame()` | pinned globals |
| `pop.on_frame(phase, fn)`, `pop.on_turn(phase, fn)` | `phase` is `"before"` or `"after"` |
| `pop.on_level_load(fn)`, `pop.on_level_end(fn)` | level events |

**There is no guest-memory write access at any name**, no hook installation and
no allocation. A script that needs those is a plugin.

The standard library is curated too: `io` and `package` are absent, so is
`debug` - it would hand a script the registry, where every callback lives - and
`load`, `loadfile`, `dofile` and `require` are removed. `os` is cut to `time`
and `clock`.

**An error disables one callback, not the mod and not the game.** The first
time a callback throws, the error is logged with the mod's name and that
callback is disabled; its siblings keep running, and a script that throws every
turn costs nothing after the first.

`mods/examples/luawalk/main.lua`, complete, walks the entity view every turn:

```lua
-- main.lua - reads entities every turn through the read-only view.
local most, kinds, reported = 0, {}, 0

local function shapes()
  local n = 0
  for _ in pairs(kinds) do n = n + 1 end
  return n
end

pop.on_turn("after", function()
  local n = pop.entity_count()
  if n > most then most = n end
  for i = 0, n - 1 do
    local e = pop.entity(pop.entity_slot(i))     -- a real decode, per entity
    kinds[e.kind] = (kinds[e.kind] or 0) + 1
  end
  -- Report the first time there is anything to report, then every 100 turns,
  -- so the log carries the metric whatever the run's length turns out to be.
  local turn = pop.simulation_turn()
  if most > 0 and (reported == 0 or turn - reported >= 100) then
    reported = turn
    pop.log("luawalk saw " .. most .. " entities of " .. shapes() ..
            " kinds, turn " .. turn)
  end
end)

pop.on_level_end(function()
  pop.log("luawalk saw " .. most .. " entities of " .. shapes() ..
          " kinds, turn " .. pop.simulation_turn())
end)
```

It reports from `on_turn` rather than only from `on_level_end`, because a run
that starts a level and quits never leaves it.

---

## 9. Settings and the page

Declare settings in `mod.toml`, or from code:

```c
PopSettingDesc desc;
memset(&desc, 0, sizeof desc);
desc.size = (uint32_t)sizeof desc;
desc.key = "hud_rows";
desc.label = "HUD rows";
desc.kind = POP_SETTING_INT;      /* or POP_SETTING_BOOL */
desc.def = 3;
desc.min = 1;
desc.max = 8;
api->register_setting(api, &desc);
```

Read and write with `api->settings_get` and `api->settings_set`. Values are
`int64_t`; a `bool` is 0 or 1. A value outside `min..max` is refused. Settings
persist per profile, under `<mod id>/<key>`. Outside plugin initialization,
a changed value is saved immediately to `mod-settings.json` with an atomic
replacement. A write failure returns `POP_E_STATE` and leaves the previous
value in memory and on disk. Initialization stays transactional: provisional
changes are saved after successful initialization, and failed initialization
does not publish them.

**The page.** `F10` is reserved by the foundation: it opens the settings page,
and the game never sees the key. The interception begins when the **host**
initialises the page, on the first frame it presents - not when the mods are
loaded. A host that never draws has no page to open and takes no keys, and the
loader deliberately does not install it, because the loader has no way to know
whether anything will ever be presented. A mod's menu entry is **a row on its own
page**, registered with `register_menu_item`, and its callback opens the page
through the only public way in:

```c
static void open_page(const PopModApi* api, void* user)
{
    (void)user;
    api->open_settings_page(api, "example.settingsmenu");   /* the public contract */
}
```

Passing a null or empty `mod_id` shows every mod's settings. `open_settings_page`
is the only way to open the page: the host's page functions are internal, and a
mod never calls one.

---

## 10. How packs will plug in

Campaign packs and gameplay extensions use this mod foundation.
There is no second mechanism and no privileged path:

- **assets** - new levels, sprites, palettes and text - ship through
  `[assets]`, in the same overlay the example uses;
- **behaviour** ships through `[plugin]` for native code or `[script]` for Lua,
  using the same hooks, events and read-only view documented above;
- **options** ship through `[settings]`, appearing on the same page, persisted
  in the same profile;
- **identity and compatibility** come from the same manifest: `requires` names
  the capabilities and the other packs a pack needs, `conflicts` names what it
  cannot sit beside, and `game` pins the executable it was built against.

What a campaign pack's own files look like - the level format, the campaign
description, how a pack declares its missions - belongs to the campaign-pack
sub-project rather than to this foundation. This document defines the ground it
stands on: how it is discovered, in what order it loads, what it can reach and
what happens when it fails.
