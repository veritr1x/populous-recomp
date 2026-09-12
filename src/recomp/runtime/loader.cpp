#include "loader.h"
#include "memory.h"
#include "imports.h"
#include "win32.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>

const char *const LOADER_DEFAULT_EXE = "original/gog/D3DPopTB.exe";
// The digest of the image actually mapped, recorded by loader_load.
static std::string g_exe_sha;

const char *const LOADER_EXPECTED_SHA256 =
    "815ba8a550f571c38b602cf3386f65aab942667a4a2d9c7096b3660deac2eacd";

namespace {

std::string g_error;
std::string g_exe_path;
uint32_t g_base = 0, g_size = 0, g_entry = 0, g_iat_patched = 0, g_iat_data = 0;
std::vector<SectionInfo> g_sections;
X86 g_ctx;

// --------------------------------------------------------------------------
// SHA-256 (FIPS 180-4), just enough to fingerprint the image.
// --------------------------------------------------------------------------
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t len = 0;
    uint8_t buf[64];
    size_t have = 0;

    static uint32_t ror(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void block(const uint8_t *p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
            0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
            0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
            0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
            0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
            0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const uint8_t *p, size_t n) {
        len += n;
        while (n) {
            size_t take = 64 - have;
            if (take > n)
                take = n;
            memcpy(buf + have, p, take);
            have += take;
            p += take;
            n -= take;
            if (have == 64) {
                block(buf);
                have = 0;
            }
        }
    }

    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (have != 56)
            update(&z, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i)
            tail[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(tail, 8);
        char out[65];
        for (int i = 0; i < 8; ++i)
            snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

std::string sha256_hex(const std::vector<uint8_t> &data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

std::string dirname_of(const std::string &p) {
    size_t s = p.find_last_of('/');
    if (s == std::string::npos)
        return ".";
    if (s == 0)
        return "/";
    return p.substr(0, s);
}

template <typename T> T rd(const std::vector<uint8_t> &d, size_t off) {
    T v{};
    if (off + sizeof(T) <= d.size())
        memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

// Writes trampolines over every IAT slot of the mapped image.
// True when [rva, rva+len) lies inside the mapped image.
bool rva_ok(uint32_t rva, uint32_t len) {
    return rva < g_size && len <= g_size - rva;
}

// Resolve the PE import table to runtime trampolines or registered guest data storage.
// Bound every descriptor and RVA by the loaded image before reading or patching it.
bool patch_iat(const std::vector<uint8_t> &file, size_t opt_off, uint16_t opt_magic) {
    size_t dd_off = opt_off + (opt_magic == 0x20b ? 112 : 96);
    if (dd_off + 16 > file.size()) {
        g_error = "PE data directories are truncated";
        return false;
    }
    uint32_t imp_rva = rd<uint32_t>(file, dd_off + 8 * 1 + 0);
    uint32_t imp_size = rd<uint32_t>(file, dd_off + 8 * 1 + 4);
    if (!imp_rva || !imp_size) {
        g_error = "image has no import directory";
        return false;
    }
    if (!rva_ok(imp_rva, imp_size)) {
        g_error = "import directory lies outside the image";
        return false;
    }

    // One descriptor per DLL, bounded by the directory size.
    uint32_t max_desc = imp_size / 20 + 1;
    uint32_t desc_rva = imp_rva;
    for (uint32_t d = 0; d < max_desc; ++d, desc_rva += 20) {
        if (!rva_ok(desc_rva, 20)) {
            g_error = "import descriptor runs past the image";
            return false;
        }
        uint32_t desc = g_base + desc_rva;
        uint32_t orig_thunk = rd32(desc + 0);
        uint32_t name_rva = rd32(desc + 12);
        uint32_t first_thunk = rd32(desc + 16);
        if (!name_rva && !first_thunk && !orig_thunk)
            break;
        if (!rva_ok(name_rva, 1) || !rva_ok(first_thunk, 4)) {
            g_error = "import descriptor points outside the image";
            return false;
        }
        std::string dll = gm_str(g_base + name_rva, 260);

        uint32_t lookup_rva = orig_thunk ? orig_thunk : first_thunk;
        uint32_t slot_rva = first_thunk;
        for (;; lookup_rva += 4, slot_rva += 4) {
            if (!rva_ok(lookup_rva, 4) || !rva_ok(slot_rva, 4)) {
                g_error = "import thunk array runs past the image";
                return false;
            }
            uint32_t lookup = g_base + lookup_rva;
            uint32_t slot = g_base + slot_rva;
            uint32_t t = rd32(lookup);
            if (!t)
                break;
            char namebuf[280];
            if (t & 0x80000000u) {
                snprintf(namebuf, sizeof namebuf, "ord%u", t & 0xffff);
            } else {
                if (!rva_ok(t, 3)) {
                    g_error = "import name lies outside the image";
                    return false;
                }
                std::string n = gm_str(g_base + t + 2, 260);
                snprintf(namebuf, sizeof namebuf, "%s", n.c_str());
            }
            uint32_t data = imports_alloc_data(dll.c_str(), namebuf);
            if (data) {
                // An imported variable, not a function: the slot holds the
                // address of zeroed guest storage.
                wr32(slot, data);
                ++g_iat_patched;
                ++g_iat_data;
                continue;
            }
            uint32_t tramp = imports_alloc_trampoline(dll.c_str(), namebuf, nullptr, ARGC_UNKNOWN);
            if (!tramp) {
                g_error = "ran out of import trampolines";
                return false;
            }
            wr32(slot, tramp);
            ++g_iat_patched;
        }
    }
    return true;
}

} // namespace

const char *loader_error() {
    return g_error.c_str();
}
uint32_t loader_image_base() {
    return g_base;
}
uint32_t loader_image_size() {
    return g_size;
}
uint32_t loader_image_limit() {
    return g_base && g_size ? g_base + g_size : 0;
}
uint32_t loader_entry_point() {
    return g_entry;
}
const std::vector<SectionInfo> &loader_sections() {
    return g_sections;
}
const std::string &loader_exe_path() {
    return g_exe_path;
}
uint32_t loader_iat_patched() {
    return g_iat_patched;
}
uint32_t loader_iat_data_imports() {
    return g_iat_data;
}
X86 *loader_context() {
    return &g_ctx;
}

// Verify the supported executable hash, map PE sections and bind its imports.
// All later address-based translation assumes this exact image; a mismatch is a hard failure.
bool loader_load(const char *exe_path) {
    g_error.clear();
    g_sections.clear();
    g_iat_patched = 0;
    g_iat_data = 0;
    g_exe_path = exe_path && *exe_path ? exe_path : LOADER_DEFAULT_EXE;

    std::vector<uint8_t> file;
    if (!read_file(g_exe_path.c_str(), file)) {
        g_error = "cannot read " + g_exe_path;
        return false;
    }

    // The content hash is a hard gate: there is no override. Everything below
    // trusts the layout of this exact image.
    std::string digest = sha256_hex(file);
    // Kept so a mod can key its manifest to the exact image it was built
    // against. It is the digest of what was really mapped, not of what was
    // expected, which is why it is recorded here and not made a constant.
    g_exe_sha = digest;
    if (digest != LOADER_EXPECTED_SHA256) {
        g_error = g_exe_path + " has SHA-256 " + digest + ", expected " + LOADER_EXPECTED_SHA256;
        return false;
    }

    if (file.size() < 0x40 || rd<uint16_t>(file, 0) != 0x5a4d) {
        g_error = "not an MZ image";
        return false;
    }
    uint32_t pe_off = rd<uint32_t>(file, 0x3c);
    if (pe_off + 24 > file.size() || rd<uint32_t>(file, pe_off) != 0x00004550) {
        g_error = "not a PE image";
        return false;
    }

    size_t fh_off = pe_off + 4;
    uint16_t machine = rd<uint16_t>(file, fh_off + 0);
    uint16_t nsections = rd<uint16_t>(file, fh_off + 2);
    uint16_t opt_size = rd<uint16_t>(file, fh_off + 16);
    size_t opt_off = fh_off + 20;
    if (opt_off + opt_size > file.size() || opt_size < 96) {
        g_error = "PE optional header is truncated";
        return false;
    }
    uint16_t opt_magic = rd<uint16_t>(file, opt_off);
    if (machine != 0x014c || opt_magic != 0x10b) {
        g_error = "expected a 32-bit x86 PE image";
        return false;
    }
    size_t sh_off = opt_off + opt_size;
    if (nsections == 0 || sh_off + 40ull * nsections > file.size()) {
        g_error = "PE section headers are truncated";
        return false;
    }

    uint32_t entry_rva = rd<uint32_t>(file, opt_off + 16);
    uint32_t image_base = rd<uint32_t>(file, opt_off + 28);
    uint32_t size_image = rd<uint32_t>(file, opt_off + 56);
    uint32_t size_hdrs = rd<uint32_t>(file, opt_off + 60);

    if (image_base != IMAGE_BASE) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "image base %08x is not the expected %08x (no relocation support)", image_base,
                 IMAGE_BASE);
        g_error = buf;
        return false;
    }
    if ((uint64_t)image_base + size_image > HEAP_BASE) {
        g_error = "image does not fit below the heap arena";
        return false;
    }

    mem_init();
    g_base = image_base;
    g_size = size_image;
    g_entry = image_base + entry_rva;

    // PE headers, then each section at its virtual address. The arena is zero
    // filled by mmap, so the tail of a section past SizeOfRawData (.bss) is
    // already zero.
    size_t hdr_copy = size_hdrs < file.size() ? size_hdrs : file.size();
    memcpy(g_mem + g_base, file.data(), hdr_copy);

    for (uint16_t i = 0; i < nsections; ++i) {
        size_t s = sh_off + 40 * i;
        char nm[9] = {0};
        memcpy(nm, file.data() + s, 8);
        SectionInfo si;
        si.name = nm;
        si.vsize = rd<uint32_t>(file, s + 8);
        si.va = image_base + rd<uint32_t>(file, s + 12);
        si.raw_size = rd<uint32_t>(file, s + 16);
        si.raw_ptr = rd<uint32_t>(file, s + 20);
        si.characteristics = rd<uint32_t>(file, s + 36);

        uint32_t span = si.vsize ? si.vsize : si.raw_size;
        if (si.va < image_base || (uint64_t)si.va + span > (uint64_t)image_base + size_image) {
            g_error = "section " + si.name + " lies outside the image";
            return false;
        }
        uint32_t copy = si.raw_size;
        if (si.vsize && copy > si.vsize)
            copy = si.vsize;
        if (si.raw_ptr && copy) {
            if ((uint64_t)si.raw_ptr + copy > file.size()) {
                g_error = "section " + si.name + " raw data is truncated";
                return false;
            }
            memcpy(g_mem + si.va, file.data() + si.raw_ptr, copy);
        }
        g_sections.push_back(si);
    }

    imports_init();
    win32_init(dirname_of(g_exe_path));

    // POPM_IMPORT_STATS=1 prints the implemented / not-reached / logging-only
    // classification at exit, which is how the "not reached" column of the
    // coverage table gets filled in from a real run.
    if (getenv("POPM_IMPORT_STATS")) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            atexit([] { imports_dump_report(stderr); });
        }
    }

    if (!patch_iat(file, opt_off, opt_magic))
        return false;

    if (g_entry != LOADER_EXPECTED_ENTRY)
        LOGW("entry point is %08x, expected %08x", g_entry, LOADER_EXPECTED_ENTRY);

    loader_init_context(&g_ctx);
    LOGV("loaded %s: base %08x size %08x entry %08x, %u IAT slots, %u trampolines",
         g_exe_path.c_str(), g_base, g_size, g_entry, g_iat_patched, imports_count());
    return true;
}

void loader_init_context(X86 *c) {
    memset(c, 0, sizeof(*c));
    // Process-start CPU state. The x87 values are the post-FINIT ones the CRT
    // assumes: control word 0x037f (round to nearest, 64-bit precision, all
    // exceptions masked), clear status word, all-empty tag word. __ftol,
    // __ctrlfp and __statfp diverge immediately if these are wrong.
    c->fpu_cw = 0x037f;
    c->fpu_sw = 0;
    c->fpu_tag = 0xffff;
    c->fpu_top = 0;
    // EFLAGS with only the reserved bit 1 and IF set, so PUSHFD reads 0x202.
    c->eflags_misc = 0x00000202u;
    c->fs_base = TEB_BASE;

    // TEB: FS:[0] SEH chain head (empty = -1), FS:[4] stack base (high),
    // FS:[8] stack limit (low), FS:[0x18] TEB self pointer, FS:[0x2c] TLS array.
    memset(g_mem + TEB_BASE, 0, TEB_SIZE);
    wr32(TEB_BASE + 0x00, 0xffffffffu);
    wr32(TEB_BASE + 0x04, STACK_TOP);
    wr32(TEB_BASE + 0x08, STACK_LIMIT);
    wr32(TEB_BASE + 0x18, TEB_BASE);
    wr32(TEB_BASE + 0x2c, TLS_BASE);
    wr32(TEB_BASE + 0x30, TEB_BASE - 0x1000); // PEB placeholder (zeroed page)
    memset(g_mem + TLS_BASE, 0, TLS_SLOTS * 4);

    // Stack: 16-byte aligned, sentinel return address on top so a RET from the
    // entry point lands somewhere recognisable.
    uint32_t esp = (STACK_TOP - 0x20) & ~0xfu;
    esp -= 4;
    wr32(esp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = esp;
    c->r[R_EBP] = 0;
    c->eip = g_entry;
}

const char *loader_exe_sha256() {
    return g_exe_sha.c_str();
}

std::string loader_hash_file(const char *path) {
    std::vector<uint8_t> file;
    if (!path || !read_file(path, file))
        return "";
    return sha256_hex(file);
}

void run_entry(X86 *c) {
    if (!g_entry) {
        LOGW("run_entry: no image loaded");
        return;
    }
    c->eip = g_entry;
    // This is threads()[0], and it is a guest thread for as long as guest code
    // runs on it. Registered rather than inferred, for the reason in
    // kernel32.cpp: t_self defaults to 0, so inference would make every host
    // thread claim to be this one.
    sched_set_guest_thread(true);
    // ExitProcess/TerminateProcess/ExitThread-outside-a-thread longjmp here.
    if (setjmp(*process_exit_jmp()) == 0)
        recomp_call(c, g_entry);
    else
        LOGW("guest process exited with code %u", process_exit_code());
    sched_set_guest_thread(false);
}

void run_entry() {
    run_entry(&g_ctx);
}
