#include "memory.h"

#include <sys/mman.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <set>
#include <string>

uint8_t *g_mem = nullptr;

// ---------------------------------------------------------------------------
// Logging helpers (declared in guest.h; kept here so every translation unit in
// the runtime gets them without a separate object file).
// ---------------------------------------------------------------------------
int log_level() {
    static int lvl = -1;
    if (lvl < 0) {
        const char *e = getenv("POPM_LOG");
        lvl = e ? atoi(e) : 1;
    }
    return lvl;
}

void log_msg(int level, const char *fmt, ...) {
    if (log_level() < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[popm] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

bool log_once(const char *key, const char *fmt, ...) {
    static std::set<std::string> seen;
    if (!seen.insert(key).second)
        return false;
    if (log_level() < 1)
        return true;
    va_list ap;
    va_start(ap, fmt);
    fputs("[popm] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return true;
}

std::string gm_str(uint32_t a, size_t max_len) {
    if (a == 0)
        return std::string();
    std::string out;
    for (size_t i = 0; i < max_len && a + i < GUEST_SIZE; ++i) {
        char ch = (char)g_mem[a + i];
        if (!ch)
            break;
        out.push_back(ch);
    }
    return out;
}

uint32_t gm_put_str(uint32_t a, const char *s, uint32_t cap) {
    if (!a || !cap)
        return 0;
    uint32_t n = 0;
    while (s[n] && n + 1 < cap) {
        g_mem[a + n] = (uint8_t)s[n];
        ++n;
    }
    g_mem[a + n] = 0;
    return n;
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------
namespace {

struct Blk {
    uint32_t size; // rounded, multiple of 16
    uint32_t req;  // bytes the guest asked for
    bool used;
};

// Every block in address order, plus an index of just the free ones so that
// first fit walks free blocks instead of the whole heap.
std::map<uint32_t, Blk> *g_blocks = nullptr;
std::set<uint32_t> *g_free = nullptr;
uint64_t g_total_allocs = 0, g_total_frees = 0;

inline uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

// Largest request the arena could ever satisfy. Anything at or above this
// would overflow the rounding in align_up (0xffffffff rounds to 0), so it is
// rejected before any arithmetic on it.
const uint32_t HEAP_ARENA_BYTES = HEAP_LIMIT - HEAP_BASE;
inline bool size_is_sane(uint32_t size) {
    return size <= HEAP_ARENA_BYTES;
}

// Records a block, keeping the free index in step.
void put_block(uint32_t addr, uint32_t size, uint32_t req, bool used) {
    (*g_blocks)[addr] = Blk{size, req, used};
    if (used)
        g_free->erase(addr);
    else
        g_free->insert(addr);
}

void drop_block(std::map<uint32_t, Blk>::iterator it) {
    g_free->erase(it->first);
    g_blocks->erase(it);
}

void heap_reset() {
    if (!g_blocks)
        g_blocks = new std::map<uint32_t, Blk>();
    if (!g_free)
        g_free = new std::set<uint32_t>();
    g_blocks->clear();
    g_free->clear();
    put_block(HEAP_BASE, HEAP_LIMIT - HEAP_BASE, 0, false);
    g_total_allocs = g_total_frees = 0;
}

} // namespace

void mem_init() {
    if (g_mem) {
        munmap(g_mem, GUEST_SIZE);
        g_mem = nullptr;
    }
    void *p = mmap(nullptr, GUEST_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[popm] fatal: cannot map %u bytes of guest memory\n", GUEST_SIZE);
        abort();
    }
    g_mem = (uint8_t *)p;
    heap_reset();
}

void mem_shutdown() {
    if (g_mem) {
        munmap(g_mem, GUEST_SIZE);
        g_mem = nullptr;
    }
    if (g_blocks) {
        delete g_blocks;
        g_blocks = nullptr;
    }
    if (g_free) {
        delete g_free;
        g_free = nullptr;
    }
}

uint32_t heap_alloc(uint32_t size, bool zero, uint32_t align) {
    if (!g_blocks)
        return 0;
    if (!size_is_sane(size)) {
        LOGW("heap_alloc: refusing a %u byte request, the arena is %u bytes", size,
             HEAP_ARENA_BYTES);
        return 0;
    }
    if (align < 16)
        align = 16;
    uint32_t need = align_up(size ? size : 1, 16);

    for (uint32_t start : *g_free) {
        auto it = g_blocks->find(start);
        if (it == g_blocks->end() || it->second.used)
            continue; // index out of step
        uint32_t len = it->second.size;
        uint32_t user = align_up(start, align);
        if (user < start)
            continue; // overflow
        uint32_t pad = user - start;
        if (pad > len || need > len - pad)
            continue; // does not fit

        uint32_t tail = len - pad - need;
        drop_block(it);
        if (pad)
            put_block(start, pad, 0, false);
        put_block(user, need, size, true);
        if (tail)
            put_block(user + need, tail, 0, false);
        if (zero)
            memset(g_mem + user, 0, need);
        ++g_total_allocs;
        return user;
    }
    LOGW("heap_alloc: out of guest heap (%u bytes requested)", size);
    return 0;
}

bool heap_owns(uint32_t addr) {
    if (!g_blocks)
        return false;
    auto it = g_blocks->find(addr);
    return it != g_blocks->end() && it->second.used;
}

uint32_t heap_size(uint32_t addr) {
    if (!g_blocks)
        return 0xffffffffu;
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used)
        return 0xffffffffu;
    return it->second.req;
}

bool heap_free(uint32_t addr) {
    if (!g_blocks || !addr)
        return false;
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used) {
        LOGW("heap_free: %08x is not a live allocation", addr);
        return false;
    }
    it->second.used = false;
    it->second.req = 0;
    g_free->insert(addr);
    ++g_total_frees;

    // Coalesce with the following block.
    auto next = std::next(it);
    if (next != g_blocks->end() && !next->second.used &&
        next->first == it->first + it->second.size) {
        it->second.size += next->second.size;
        drop_block(next);
    }
    // Coalesce with the preceding block.
    if (it != g_blocks->begin()) {
        auto prev = std::prev(it);
        if (!prev->second.used && prev->first + prev->second.size == it->first) {
            prev->second.size += it->second.size;
            drop_block(it);
        }
    }
    return true;
}

// Resize a guest heap allocation in place when possible, otherwise copy into a new block.
// Reject arena-sized overflow requests and leave the original allocation live if growth fails.
uint32_t heap_realloc(uint32_t addr, uint32_t new_size, bool zero) {
    if (!addr)
        return heap_alloc(new_size, zero);
    if (!g_blocks)
        return 0;
    if (!size_is_sane(new_size)) {
        LOGW("heap_realloc: refusing a %u byte request, the arena is %u bytes", new_size,
             HEAP_ARENA_BYTES);
        return 0;
    }
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used) {
        LOGW("heap_realloc: %08x is not a live allocation", addr);
        return 0;
    }
    uint32_t old_req = it->second.req;
    uint32_t need = align_up(new_size ? new_size : 1, 16);
    uint32_t have = it->second.size;

    if (need <= have) {
        uint32_t tail = have - need;
        if (tail >= 16) {
            it->second.size = need;
            auto next = std::next(it);
            if (next != g_blocks->end() && !next->second.used && next->first == addr + have) {
                uint32_t merged = tail + next->second.size;
                drop_block(next);
                put_block(addr + need, merged, 0, false);
            } else {
                put_block(addr + need, tail, 0, false);
            }
        }
        it->second.req = new_size;
        if (zero && new_size > old_req)
            memset(g_mem + addr + old_req, 0, new_size - old_req);
        return addr;
    }

    // Try to grow into a free neighbour.
    auto next = std::next(it);
    if (next != g_blocks->end() && !next->second.used && next->first == addr + have &&
        have + next->second.size >= need) {
        uint32_t total = have + next->second.size;
        drop_block(next);
        uint32_t tail = total - need;
        it->second.size = need;
        it->second.req = new_size;
        if (tail)
            put_block(addr + need, tail, 0, false);
        if (zero)
            memset(g_mem + addr + old_req, 0, new_size - old_req);
        return addr;
    }

    uint32_t fresh = heap_alloc(new_size, false);
    if (!fresh)
        return 0;
    uint32_t copy = old_req < new_size ? old_req : new_size;
    memmove(g_mem + fresh, g_mem + addr, copy);
    if (zero && new_size > copy)
        memset(g_mem + fresh + copy, 0, new_size - copy);
    heap_free(addr);
    return fresh;
}

HeapStats heap_stats() {
    HeapStats s{};
    s.total_allocs = g_total_allocs;
    s.total_frees = g_total_frees;
    if (!g_blocks)
        return s;
    for (auto &kv : *g_blocks) {
        if (kv.second.used) {
            ++s.used_blocks;
            s.used_bytes += kv.second.size;
            s.req_bytes += kv.second.req;
        } else {
            ++s.free_blocks;
            s.free_bytes += kv.second.size;
            if (kv.second.size > s.largest_free)
                s.largest_free = kv.second.size;
        }
    }
    return s;
}

// Check that the heap block map covers the arena without gaps, overlap or adjacent free blocks.
// Return the first invariant failure, or an empty string when the allocation structure is consistent.
std::string heap_check() {
    if (!g_blocks)
        return "heap not initialised";
    uint32_t cursor = HEAP_BASE;
    bool prev_free = false;
    char buf[160];
    for (auto &kv : *g_blocks) {
        if (kv.first != cursor) {
            snprintf(buf, sizeof buf, "gap or overlap at %08x (expected %08x)", kv.first, cursor);
            return buf;
        }
        if (kv.second.size == 0 || (kv.second.size & 15)) {
            snprintf(buf, sizeof buf, "block %08x has bad size %u", kv.first, kv.second.size);
            return buf;
        }
        if (!kv.second.used && prev_free) {
            snprintf(buf, sizeof buf, "adjacent free blocks at %08x", kv.first);
            return buf;
        }
        prev_free = !kv.second.used;
        cursor += kv.second.size;
    }
    if (cursor != HEAP_LIMIT) {
        snprintf(buf, sizeof buf, "heap ends at %08x, expected %08x", cursor, HEAP_LIMIT);
        return buf;
    }
    for (uint32_t a : *g_free) {
        auto it = g_blocks->find(a);
        if (it == g_blocks->end() || it->second.used) {
            snprintf(buf, sizeof buf, "free index holds %08x, which is not a free block", a);
            return buf;
        }
    }
    size_t free_count = 0;
    for (auto &kv : *g_blocks)
        if (!kv.second.used)
            ++free_count;
    if (free_count != g_free->size()) {
        snprintf(buf, sizeof buf, "free index has %zu entries, the heap has %zu free blocks",
                 g_free->size(), free_count);
        return buf;
    }
    return std::string();
}
