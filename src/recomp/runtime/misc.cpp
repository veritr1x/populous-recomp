// misc.cpp - the smaller import sets: GDI32, ADVAPI32 (registry), SHELL32,
// ole32, IMM32, WSOCK32 and the non-audio half of WINMM (the millisecond
// clock, multimedia timers and mmio file access). MIDI and aux output are
// logging-only here; real audio belongs to the audio task.
#include "imports.h"
#include "layout.h"
#include "memory.h"
#include "win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/os.h"
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
namespace {
uint64_t g_epoch_us = 0;
uint32_t (*g_time_source)() = nullptr;
// Not owned and not copied: every caller passes a string literal, and this is
// read from a run record writer that may run at exit, by which time anything
// with a lifetime would be a question.
const char *g_clock_desc = nullptr;

uint64_t now_us() {
    return os_wall_time_us();
}
} // namespace

uint32_t host_millis() {
    if (g_time_source)
        return g_time_source();
    if (!g_epoch_us)
        g_epoch_us = now_us();
    return (uint32_t)((now_us() - g_epoch_us) / 1000ull);
}
void host_set_time_source(uint32_t (*fn)()) {
    g_time_source = fn;
}

// ---------------------------------------------------------------------------
// The pinned clock. One counter, here, so that the parity fixture and the boot
// hosts pin the same thing rather than each keeping their own.
//
// Atomic because the guest reads it from every thread it runs, while the host
// advances it from the one that presents. Relaxed is enough: nothing is
// published through it, and a reader a step behind sees a time that was true a
// moment ago, which is the most any clock offers.
// ---------------------------------------------------------------------------
namespace {
std::atomic<bool> g_pin_on{false};
std::atomic<uint32_t> g_pin_ms{0};
uint32_t g_pin_step = 0;
// Static, because host_set_clock_description does not copy: it is read at exit
// by the run record writer, so anything with a shorter life would be a
// question. Sixty-four bytes holds "pinned start=4294967295 step=4294967295".
char g_pin_desc[64];

uint32_t pinned_millis() {
    return g_pin_ms.load(std::memory_order_relaxed);
}
} // namespace

void host_set_time_source_pinned(uint32_t start_ms, uint32_t step_ms) {
    g_pin_ms.store(start_ms, std::memory_order_relaxed);
    g_pin_step = step_ms;
    g_pin_on.store(true, std::memory_order_release);
    snprintf(g_pin_desc, sizeof g_pin_desc, "pinned start=%u step=%u", start_ms, step_ms);
    host_set_clock_description(g_pin_desc);
    host_set_time_source(pinned_millis);
}

void host_pinned_clock_advance() {
    if (!g_pin_on.load(std::memory_order_acquire))
        return;
    g_pin_ms.fetch_add(g_pin_step, std::memory_order_relaxed);
}

void host_clear_time_source() {
    g_pin_on.store(false, std::memory_order_release);
    g_pin_step = 0;
    g_time_source = nullptr;
    g_clock_desc = nullptr; // back to "monotonic"
}

bool host_time_source_is_pinned() {
    return g_pin_on.load(std::memory_order_acquire);
}
uint32_t host_pinned_clock_value() {
    return pinned_millis();
}
uint32_t host_pinned_clock_step() {
    return g_pin_on.load(std::memory_order_acquire) ? g_pin_step : 0u;
}
void host_set_clock_description(const char *text) {
    g_clock_desc = text;
}
const char *host_clock_description() {
    return g_clock_desc ? g_clock_desc : "monotonic";
}

// ---------------------------------------------------------------------------
// The cadence trace.
//
// What it is for: the display work needs to know how often the guest is asking
// the time and how often anything ticks, because a frame rate that looks wrong
// is often a loop that is being paced by something other than frames. Recording
// intervals rather than absolute times is deliberate - two runs start at
// different moments and a diff of absolute stamps is all noise - and measuring
// them on the GUEST's clock rather than the wall is deliberate too, so that a
// pinned run's cadence is the pin's steps and two pinned runs can be compared
// at all.
//
// A note on what is NOT here: this game installs no window timer. There is no
// SetTimer shim in the runtime and no WM_TIMER anywhere in it, so the WM_TIMER
// kind records what reaches host_post_message and nothing else, and a real
// run's trace has no WM_TIMER lines in it. That is an absence of timers, not an
// absence of instrumentation, and the difference matters to whoever reads the
// trace: a missing kind here means the game never did it.
// ---------------------------------------------------------------------------
namespace {
std::mutex g_cadence_mutex;
FILE *g_cadence = nullptr;
// Last event time per kind, on the guest's clock. Small and fixed: three kinds.
struct CadenceKind {
    const char *name;
    uint32_t last;
    bool seen;
};
CadenceKind g_cadence_kinds[] = {
    {"timeGetTime", 0, false}, {"GetTickCount", 0, false}, {"QueryPerformanceCounter", 0, false},
    {"WM_TIMER", 0, false},    {"mm_callback", 0, false},
};
} // namespace

void host_set_cadence_trace(const char *path) {
    std::lock_guard<std::mutex> lock(g_cadence_mutex);
    if (g_cadence) {
        fclose(g_cadence);
        g_cadence = nullptr;
    }
    for (CadenceKind &k : g_cadence_kinds)
        k.seen = false;
    if (!path || !*path)
        return;
    g_cadence = fopen(path, "w");
    if (!g_cadence)
        LOGW("cadence trace: cannot write %s; the run continues untraced", path);
}

void host_note_cadence(const char *kind) {
    if (!g_cadence || !kind)
        return; // the common case, unlocked
    // NOT host_millis(). A boot host's time source is boot_time_source, which
    // counts a poll on every read and takes a stall-breaking step after 256 of
    // them - so reading the clock to timestamp a trace would make a traced run
    // pace differently from an untraced one, and the trace would be measuring
    // its own effect on the thing it measures.
    //
    // Pinned, the counter is readable directly with no side effect. Unpinned,
    // boot_clock_poll() returns immediately and there is no poll to miscount,
    // so host_millis() is safe and is the only way to get a real time.
    uint32_t now = host_time_source_is_pinned() ? host_pinned_clock_value() : host_millis();
    std::lock_guard<std::mutex> lock(g_cadence_mutex);
    if (!g_cadence)
        return; // closed while we waited
    for (CadenceKind &k : g_cadence_kinds) {
        if (strcmp(k.name, kind) != 0)
            continue;
        // The first event of a kind sets the baseline and prints nothing:
        // there is no interval before the first one, and printing a zero
        // would put a gap in the histogram that no run ever had.
        if (k.seen)
            fprintf(g_cadence, "%s,%u\n", k.name, now - k.last);
        k.last = now;
        k.seen = true;
        fflush(g_cadence); // a killed run keeps its trace
        return;
    }
}

// ---------------------------------------------------------------------------
// A very small JSON reader/writer for the registry file. Supports objects,
// strings and integers, which is all the registry backing store needs.
// ---------------------------------------------------------------------------
namespace {

struct JValue {
    enum Kind { OBJ, STR, NUM } kind = OBJ;
    std::map<std::string, JValue> obj;
    std::string str;
    long long num = 0;
};

struct JParser {
    const std::string &s;
    size_t i = 0;
    bool ok = true;
    explicit JParser(const std::string &src) : s(src) {}

    void ws() {
        while (i < s.size() && isspace((unsigned char)s[i]))
            ++i;
    }

    std::string parse_string() {
        std::string out;
        if (i >= s.size() || s[i] != '"') {
            ok = false;
            return out;
        }
        ++i;
        while (i < s.size() && s[i] != '"') {
            char ch = s[i++];
            if (ch == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                case 'n':
                    out.push_back('\n');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 'u': {
                    if (i + 4 <= s.size()) {
                        int v = (int)strtol(s.substr(i, 4).c_str(), nullptr, 16);
                        i += 4;
                        out.push_back((char)(v & 0xff));
                    }
                    break;
                }
                default:
                    out.push_back(e);
                    break;
                }
            } else {
                out.push_back(ch);
            }
        }
        if (i < s.size())
            ++i;
        return out;
    }

    JValue parse() {
        ws();
        JValue v;
        if (i >= s.size()) {
            ok = false;
            return v;
        }
        if (s[i] == '{') {
            ++i;
            v.kind = JValue::OBJ;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return v;
            }
            while (ok && i < s.size()) {
                ws();
                std::string key = parse_string();
                ws();
                if (i < s.size() && s[i] == ':')
                    ++i;
                else {
                    ok = false;
                    break;
                }
                v.obj[key] = parse();
                ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    break;
                }
                ok = false;
            }
        } else if (s[i] == '"') {
            v.kind = JValue::STR;
            v.str = parse_string();
        } else {
            v.kind = JValue::NUM;
            size_t start = i;
            while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+'))
                ++i;
            if (start == i) {
                ok = false;
                return v;
            }
            v.num = strtoll(s.substr(start, i - start).c_str(), nullptr, 10);
        }
        return v;
    }
};

std::string json_escape(const std::string &s) {
    std::string out;
    for (unsigned char ch : s) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20 || ch >= 0x7f) {
                char buf[8];
                snprintf(buf, sizeof buf, "\\u%04x", ch);
                out += buf;
            } else {
                out.push_back((char)ch);
            }
        }
    }
    return out;
}

// -------------------------------------------------------------------------
// Registry store
// -------------------------------------------------------------------------
struct RegValue {
    uint32_t type = 1;        // REG_SZ
    std::string str;          // REG_SZ / REG_EXPAND_SZ
    uint32_t dword = 0;       // REG_DWORD
    std::vector<uint8_t> bin; // REG_BINARY and everything else
};

// Registry key paths and value names are case-insensitive on Windows.
struct CiLess {
    bool operator()(const std::string &a, const std::string &b) const {
        return os_strcasecmp(a.c_str(), b.c_str()) < 0;
    }
};
typedef std::map<std::string, RegValue, CiLess> RegValues;

std::map<std::string, RegValues, CiLess> &regstore() {
    static std::map<std::string, RegValues, CiLess> m;
    return m;
}
std::map<uint32_t, std::string> &regkeys() {
    static std::map<uint32_t, std::string> m;
    return m;
}
uint32_t g_next_hkey = 0x00030004;
bool g_registry_dirty = false;

const char *hive_name(uint32_t h) {
    switch (h) {
    case 0x80000000u:
        return "HKEY_CLASSES_ROOT";
    case 0x80000001u:
        return "HKEY_CURRENT_USER";
    case 0x80000002u:
        return "HKEY_LOCAL_MACHINE";
    case 0x80000003u:
        return "HKEY_USERS";
    case 0x80000005u:
        return "HKEY_CURRENT_CONFIG";
    default:
        return nullptr;
    }
}

std::string key_path(uint32_t hkey, const std::string &sub) {
    std::string base;
    if (const char *h = hive_name(hkey))
        base = h;
    else {
        auto it = regkeys().find(hkey);
        if (it == regkeys().end())
            return std::string();
        base = it->second;
    }
    if (sub.empty())
        return base;
    return base + "\\" + sub;
}

std::vector<uint8_t> hex_to_bytes(const std::string &s) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back((uint8_t)strtol(s.substr(i, 2).c_str(), nullptr, 16));
    return out;
}

std::string bytes_to_hex(const std::vector<uint8_t> &b) {
    std::string out;
    char buf[3];
    for (uint8_t v : b) {
        snprintf(buf, sizeof buf, "%02x", v);
        out += buf;
    }
    return out;
}

} // namespace

std::string registry_path() {
    if (const char *e = getenv("POPM_REGISTRY"))
        return e;
    return host_state_file("registry.json");
}

// Load the profile registry into the guest key/value store and reset transient key handles.
// An absent file leaves defaults available through the normal registry shim behavior.
void registry_load() {
    regstore().clear();
    regkeys().clear();
    g_next_hkey = 0x00030004;
    g_registry_dirty = false;

    std::string path = registry_path();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        LOGV("registry: %s does not exist yet, starting empty", path.c_str());
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);

    JParser p(text);
    JValue root = p.parse();
    if (!p.ok || root.kind != JValue::OBJ) {
        LOGW("registry: %s is not valid JSON, ignoring it", path.c_str());
        return;
    }
    for (const auto &key : root.obj) {
        if (key.second.kind != JValue::OBJ)
            continue;
        auto &values = regstore()[key.first];
        for (const auto &val : key.second.obj) {
            if (val.second.kind != JValue::OBJ)
                continue;
            RegValue rv;
            auto t = val.second.obj.find("type");
            if (t != val.second.obj.end() && t->second.kind == JValue::NUM)
                rv.type = (uint32_t)t->second.num;
            auto d = val.second.obj.find("data");
            if (d != val.second.obj.end()) {
                if (rv.type == 4 || rv.type == 5) {
                    rv.dword = d->second.kind == JValue::NUM
                                   ? (uint32_t)d->second.num
                                   : (uint32_t)strtoul(d->second.str.c_str(), nullptr, 0);
                } else if (rv.type == 1 || rv.type == 2) {
                    rv.str = d->second.str;
                } else {
                    rv.bin = hex_to_bytes(d->second.str);
                }
            }
            values[val.first] = rv;
        }
    }
    LOGV("registry: loaded %zu keys from %s", regstore().size(), path.c_str());
}

// Persist the registry when changed, creating the destination directories as needed.
// An open failure leaves the dirty flag set so a later flush can retry.
void registry_flush() {
    if (!g_registry_dirty)
        return;
    std::string path = registry_path();
    // Create the parent directory chain (build/recomp) if it is missing.
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos)
        os_mkdir(path.substr(0, pos).c_str());
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        LOGW("registry: cannot write %s", path.c_str());
        return;
    }
    fprintf(f, "{\n");
    bool first_key = true;
    for (const auto &key : regstore()) {
        if (!first_key)
            fprintf(f, ",\n");
        first_key = false;
        fprintf(f, "  \"%s\": {\n", json_escape(key.first).c_str());
        bool first_val = true;
        for (const auto &val : key.second) {
            if (!first_val)
                fprintf(f, ",\n");
            first_val = false;
            const RegValue &rv = val.second;
            if (rv.type == 4 || rv.type == 5)
                fprintf(f, "    \"%s\": {\"type\": %u, \"data\": %u}",
                        json_escape(val.first).c_str(), rv.type, rv.dword);
            else if (rv.type == 1 || rv.type == 2)
                fprintf(f, "    \"%s\": {\"type\": %u, \"data\": \"%s\"}",
                        json_escape(val.first).c_str(), rv.type, json_escape(rv.str).c_str());
            else
                fprintf(f, "    \"%s\": {\"type\": %u, \"data\": \"%s\"}",
                        json_escape(val.first).c_str(), rv.type, bytes_to_hex(rv.bin).c_str());
        }
        fprintf(f, "\n  }");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    g_registry_dirty = false;
}

namespace {

// -------------------------------------------------------------------------
// ADVAPI32
// -------------------------------------------------------------------------
void a_RegOpenKeyEx(X86 *c, uint32_t hkey, uint32_t psub, uint32_t presult) {
    std::string sub = gm_str(psub, 512);
    std::string path = key_path(hkey, sub);
    if (path.empty()) {
        set_eax(c, 6);
        return;
    } // ERROR_INVALID_HANDLE
    if (regstore().find(path) == regstore().end()) {
        LOGV("RegOpenKeyExA(%s): not found", path.c_str());
        set_eax(c, 2); // ERROR_FILE_NOT_FOUND
        return;
    }
    uint32_t h = g_next_hkey;
    g_next_hkey += 4;
    regkeys()[h] = path;
    if (presult)
        wr32(presult, h);
    set_eax(c, 0);
}

void a_RegOpenKeyA(X86 *c) {
    a_RegOpenKeyEx(c, arg(c, 0), arg(c, 1), arg(c, 2));
}
void a_RegOpenKeyExA(X86 *c) {
    a_RegOpenKeyEx(c, arg(c, 0), arg(c, 1), arg(c, 4));
}

void a_RegCreateKeyExA(X86 *c) {
    std::string path = key_path(arg(c, 0), gm_str(arg(c, 1), 512));
    uint32_t presult = arg(c, 7), pdisp = arg(c, 8);
    if (path.empty()) {
        set_eax(c, 6);
        return;
    }
    bool existed = regstore().find(path) != regstore().end();
    if (!existed) {
        regstore()[path];
        g_registry_dirty = true;
    }
    uint32_t h = g_next_hkey;
    g_next_hkey += 4;
    regkeys()[h] = path;
    if (presult)
        wr32(presult, h);
    if (pdisp)
        wr32(pdisp, existed ? 2 : 1); // OPENED_EXISTING_KEY / CREATED_NEW_KEY
    set_eax(c, 0);
}

void a_RegCloseKey(X86 *c) {
    regkeys().erase(arg(c, 0));
    registry_flush();
    set_eax(c, 0);
}

void a_RegQueryValueExA(X86 *c) {
    uint32_t hkey = arg(c, 0);
    std::string name = gm_str(arg(c, 1), 256);
    uint32_t ptype = arg(c, 3), pdata = arg(c, 4), pcb = arg(c, 5);

    auto ki = regkeys().find(hkey);
    std::string path =
        ki != regkeys().end() ? ki->second : std::string(hive_name(hkey) ? hive_name(hkey) : "");
    if (path.empty()) {
        set_eax(c, 6);
        return;
    }
    auto si = regstore().find(path);
    if (si == regstore().end()) {
        set_eax(c, 2);
        return;
    }
    auto vi = si->second.find(name);
    if (vi == si->second.end()) {
        set_eax(c, 2);
        return;
    }

    const RegValue &rv = vi->second;
    std::vector<uint8_t> bytes;
    if (rv.type == 4 || rv.type == 5) {
        bytes.resize(4);
        memcpy(bytes.data(), &rv.dword, 4);
    } else if (rv.type == 1 || rv.type == 2) {
        bytes.assign(rv.str.begin(), rv.str.end());
        bytes.push_back(0);
    } else {
        bytes = rv.bin;
    }
    if (ptype)
        wr32(ptype, rv.type);
    uint32_t have = pcb ? rd32(pcb) : 0;
    if (pcb)
        wr32(pcb, (uint32_t)bytes.size());
    if (!pdata) {
        set_eax(c, 0);
        return;
    }
    if (have < bytes.size()) {
        set_eax(c, 234);
        return;
    } // ERROR_MORE_DATA
    memcpy(g_mem + pdata, bytes.data(), bytes.size());
    set_eax(c, 0);
}

void a_RegSetValueExA(X86 *c) {
    uint32_t hkey = arg(c, 0);
    std::string name = gm_str(arg(c, 1), 256);
    uint32_t type = arg(c, 3), pdata = arg(c, 4), cb = arg(c, 5);

    auto ki = regkeys().find(hkey);
    std::string path =
        ki != regkeys().end() ? ki->second : std::string(hive_name(hkey) ? hive_name(hkey) : "");
    if (path.empty()) {
        set_eax(c, 6);
        return;
    }

    RegValue rv;
    rv.type = type;
    if (type == 4 || type == 5) {
        rv.dword = cb >= 4 && pdata ? rd32(pdata) : 0;
    } else if (type == 1 || type == 2) {
        rv.str = pdata ? gm_str(pdata, cb ? cb : 0x8000) : std::string();
    } else if (pdata) {
        rv.bin.assign(g_mem + pdata, g_mem + pdata + cb);
    }
    regstore()[path][name] = rv;
    g_registry_dirty = true;
    registry_flush();
    set_eax(c, 0);
}

// -------------------------------------------------------------------------
// GDI32
// -------------------------------------------------------------------------
void g_GetStockObject(X86 *c) {
    set_eax(c, 0x00031000 + arg(c, 0));
}
void g_GetSystemPaletteEntries(X86 *c) {
    // There is no host palette behind these DCs. Reporting zero entries is the
    // documented "no palette" answer; the DirectDraw shims own real palettes.
    log_once("GetSystemPaletteEntries",
             "GetSystemPaletteEntries: no host palette, returning 0 entries");
    set_eax(c, 0);
}

// -------------------------------------------------------------------------
// SHELL32 / ole32
// -------------------------------------------------------------------------
void s_ShellExecuteA(X86 *c) {
    LOGW("ShellExecuteA(\"%s\", \"%s\"): nothing is launched from the runtime",
         gm_str(arg(c, 1), 64).c_str(), gm_str(arg(c, 2), 260).c_str());
    set_eax(c, 2); // ERROR_FILE_NOT_FOUND: the shell did not start anything
}

void o_CoInitialize(X86 *c) {
    set_eax(c, 0);
} // S_OK
void o_CoUninitialize(X86 *c) {
    set_eax(c, 0);
}

// -------------------------------------------------------------------------
// IMM32: no input method is attached.
// -------------------------------------------------------------------------
void i_ImmGetContext(X86 *c) {
    set_eax(c, 0);
}
void i_ImmReleaseContext(X86 *c) {
    set_eax(c, 1);
}
void i_ImmGetOpenStatus(X86 *c) {
    set_eax(c, 0);
}
void i_ImmSetOpenStatus(X86 *c) {
    set_eax(c, 1);
}
void i_ImmGetCompositionStringA(X86 *c) {
    set_eax(c, 0);
}
void i_ImmGetCandidateListA(X86 *c) {
    set_eax(c, 0);
}
void i_ImmSetCompositionWindow(X86 *c) {
    set_eax(c, 1);
}

// -------------------------------------------------------------------------
// WSOCK32: enough for startup probing; real networking is a later task.
// -------------------------------------------------------------------------
void w_WSAStartup(X86 *c) {
    uint32_t version = arg(c, 0), pdata = arg(c, 1);
    if (pdata) {
        memset(g_mem + pdata, 0, 400);
        wr16(pdata + 0, (uint16_t)version);
        wr16(pdata + 2, 0x0202);
        gm_put_str(pdata + 4, "pop-metal recomp sockets", 257);
        gm_put_str(pdata + 4 + 257, "Running", 129);
        wr16(pdata + 4 + 257 + 129, 32);       // iMaxSockets
        wr16(pdata + 4 + 257 + 129 + 2, 1024); // iMaxUdpDg
    }
    set_eax(c, 0);
}
void w_WSACleanup(X86 *c) {
    set_eax(c, 0);
}
void w_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (buf && len)
        gm_put_str(buf, "populous", len);
    set_eax(c, 0);
}
void w_gethostbyname(X86 *c) {
    log_once("gethostbyname", "gethostbyname(\"%s\"): name resolution is not implemented",
             gm_str(arg(c, 0), 256).c_str());
    set_eax(c, 0);
}
void w_inet_ntoa(X86 *c) {
    // The returned pointer is reused across calls, as the real inet_ntoa does.
    // heap_owns re-checks it because mem_init() invalidates the whole heap.
    static uint32_t buf = 0;
    if (!buf || !heap_owns(buf))
        buf = heap_alloc(32, true);
    uint32_t addr = arg(c, 0);
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%u.%u.%u.%u", addr & 0xff, (addr >> 8) & 0xff, (addr >> 16) & 0xff,
             (addr >> 24) & 0xff);
    if (buf)
        gm_put_str(buf, tmp, 32);
    set_eax(c, buf);
}

// -------------------------------------------------------------------------
// WINMM: clock, multimedia timers and mmio.
// -------------------------------------------------------------------------
struct MmTimer {
    uint32_t id = 0;
    uint32_t callback = 0; // a guest function, or an event handle in event mode
    uint32_t user = 0;
    uint32_t period = 0;
    uint32_t next_due = 0;
    bool periodic = false;
    bool alive = false;
    // TIME_CALLBACK_EVENT_SET / _PULSE: lpTimeProc is an event handle.
    bool event_mode = false;
    bool event_pulse = false;
};

std::map<uint32_t, MmTimer> &timers() {
    static std::map<uint32_t, MmTimer> m;
    return m;
}
uint32_t g_next_timer_id = 1;

void m_timeGetTime(X86 *c) {
    host_note_cadence("timeGetTime");
    set_eax(c, host_millis());
}

void m_timeSetEvent(X86 *c) {
    uint32_t delay = arg(c, 0), callback = arg(c, 2), user = arg(c, 3), flags = arg(c, 4);
    MmTimer t;
    t.id = g_next_timer_id++;
    t.callback = callback;
    t.user = user;
    t.period = delay ? delay : 1;
    t.periodic = (flags & 1) != 0; // TIME_PERIODIC
    t.next_due = host_millis() + t.period;
    t.alive = true;
    uint32_t cb_mode = flags & 0x30;
    t.event_mode = cb_mode != 0; // TIME_CALLBACK_EVENT_SET/_PULSE
    t.event_pulse = cb_mode == 0x20;
    timers()[t.id] = t;
    LOGV("timeSetEvent(%u ms, %s, %s=%08x) -> id %u", delay, t.periodic ? "periodic" : "one-shot",
         t.event_mode ? "event" : "callback", callback, t.id);
    set_eax(c, t.id);
}

void m_timeKillEvent(X86 *c) {
    timers().erase(arg(c, 0));
    set_eax(c, 0); // TIMERR_NOERROR
}

void m_timeBeginPeriod(X86 *c) {
    set_eax(c, 0);
}
void m_timeEndPeriod(X86 *c) {
    set_eax(c, 0);
}

// mmio: a thin wrapper over the same case-insensitive file layer as CreateFileA.
struct MmioFile {
    FILE *fp = nullptr;
    std::string path;
    bool writable = false;
};
std::map<uint32_t, MmioFile> &mmios() {
    static std::map<uint32_t, MmioFile> m;
    return m;
}
uint32_t g_next_mmio = 0x00040004;

void m_mmioOpenA(X86 *c) {
    std::string name = gm_str(arg(c, 0), 260);
    uint32_t pinfo = arg(c, 1), flags = arg(c, 2);
    bool write = (flags & 0x00000001) != 0;     // MMIO_WRITE
    bool readwrite = (flags & 0x00000002) != 0; // MMIO_READWRITE
    bool create = (flags & 0x00001000) != 0;    // MMIO_CREATE

    std::string host = win32_host_path(name, write || readwrite || create);
    if (host.empty()) {
        LOGV("mmioOpenA(%s): not found", name.c_str());
        if (pinfo)
            wr32(pinfo + 4, 258); // MMIOINFO.wErrorRet = MMIOERR_FILENOTFOUND
        set_eax(c, 0);
        return;
    }
    const char *mode = create      ? (readwrite ? "w+b" : "wb")
                       : readwrite ? "r+b"
                       : write     ? "r+b"
                                   : "rb";
    FILE *fp = fopen(host.c_str(), mode);
    if (!fp) {
        LOGV("mmioOpenA(%s, mode %s): open failed", name.c_str(), mode);
        if (pinfo)
            wr32(pinfo + 4, 262); // MMIOERR_CANNOTOPEN
        set_eax(c, 0);
        return;
    }
    if (create || write || readwrite)
        win32_invalidate_dir_cache();
    uint32_t h = g_next_mmio;
    g_next_mmio += 4;
    mmios()[h] = MmioFile{fp, host, write || readwrite || create};
    set_eax(c, h);
}

void m_mmioWrite(X86 *c) {
    auto it = mmios().find(arg(c, 0));
    uint32_t buf = arg(c, 1), n = arg(c, 2);
    if (it == mmios().end() || !it->second.writable || !gm_valid(buf, n)) {
        set_eax(c, 0xffffffffu);
        return;
    }
    set_eax(c, (uint32_t)fwrite(g_mem + buf, 1, n, it->second.fp));
}

void m_mmioFlush(X86 *c) {
    auto it = mmios().find(arg(c, 0));
    if (it != mmios().end())
        fflush(it->second.fp);
    set_eax(c, 0);
}

void m_mmioRead(X86 *c) {
    auto it = mmios().find(arg(c, 0));
    uint32_t buf = arg(c, 1), n = arg(c, 2);
    if (it == mmios().end() || !gm_valid(buf, n)) {
        set_eax(c, 0xffffffffu);
        return;
    }
    size_t got = fread(g_mem + buf, 1, n, it->second.fp);
    set_eax(c, (uint32_t)got);
}

void m_mmioSeek(X86 *c) {
    auto it = mmios().find(arg(c, 0));
    int32_t off = (int32_t)arg(c, 1);
    uint32_t origin = arg(c, 2);
    if (it == mmios().end()) {
        set_eax(c, 0xffffffffu);
        return;
    }
    int whence = origin == 1 ? SEEK_CUR : origin == 2 ? SEEK_END : SEEK_SET;
    if (fseek(it->second.fp, off, whence) != 0) {
        set_eax(c, 0xffffffffu);
        return;
    }
    set_eax(c, (uint32_t)ftell(it->second.fp));
}

void m_mmioClose(X86 *c) {
    auto it = mmios().find(arg(c, 0));
    if (it != mmios().end()) {
        fclose(it->second.fp);
        mmios().erase(it);
    }
    set_eax(c, 0);
}

void m_mmioSetBuffer(X86 *c) {
    set_eax(c, 0);
} // MMSYSERR_NOERROR

// ---------------------------------------------------------------------------
// MIDI out.
//
// The game's music is MIDI through a SoundFont. 0x575e40 walks the midiOut
// devices, calls midiOutGetDevCapsA on each, and compares the first nine
// characters of szPname against "SoundFont" at 0x5eb5b0; if no device matches
// it returns -1 and there is no music. It then opens that device with a null
// callback, sends a twelve-byte sysex through midiOutPrepareHeader and
// midiOutLongMsg, and drives the music note by note with midiOutShortMsg
// (0x576140 is its all-notes-off: control change 0x7b on channel 0).
//
// So the device this reports has to be named the way the game is looking for,
// and the bank it plays is Sound/POPFIGHT.SF2 next to the executable.
// ---------------------------------------------------------------------------
const uint32_t MMSYSERR_NOERROR = 0;
const uint32_t MMSYSERR_BADDEVICEID = 2;
const uint32_t MMSYSERR_INVALPARAM = 11;
const uint32_t MMSYSERR_NODRIVER = 6;
const uint32_t MIDIERR_UNPREPARED = 64;

// MIDIOUTCAPSA: wMid, wPid, vDriverVersion, szPname[32], wTechnology, wVoices,
// wNotes, wChannelMask, dwSupport. Fifty-two bytes, which is the 0x34 the game
// passes.
const uint32_t MIDIOUTCAPSA_SIZE = 52;
const uint16_t MOD_SWSYNTH = 7;

// MIDIHDR: lpData, dwBufferLength, dwBytesRecorded, dwUser, dwFlags, lpNext,
// reserved, dwOffset, dwReserved[8]. Sixty-four bytes.
const uint32_t MIDIHDR_SIZE = 64;
const uint32_t MHDR_OFF_lpData = 0, MHDR_OFF_dwBufferLength = 4;
const uint32_t MHDR_OFF_dwUser = 12, MHDR_OFF_dwFlags = 16;
const uint32_t MHDR_DONE = 0x1, MHDR_PREPARED = 0x2, MHDR_INQUEUE = 0x4;

// The one open device. The game opens exactly one; the handle is a token
// rather than an index so a stale one is caught rather than followed.
const uint32_t MIDI_HANDLE = 0x4d494449u; // 'MIDI'
struct MidiOut {
    bool open = false;
    bool synth = false;    // the host has a synth; false means silence
    uint32_t callback = 0; // guest function, 0 for CALLBACK_NULL
    uint32_t instance = 0;
    uint32_t flags = 0;
    uint32_t shorts = 0, sysexes = 0;
};
MidiOut &midi() {
    static MidiOut m;
    return m;
}

// Where the bank lives. The game never names it - SFMAN32.DLL would have - so
// the path is the one the retail install uses, resolved through the file shim
// so it follows whatever root this run was given.
std::string midi_soundfont_path() {
    static const char *candidates[] = {
        "Sound\\POPFIGHT.SF2",
        "sound\\popfight.sf2",
        "POPFIGHT.SF2",
    };
    for (const char *g : candidates) {
        std::string host = win32_host_path(g);
        if (host.empty())
            continue;
        OsStat st;
        if (os_stat(host.c_str(), &st) == 0 && st.is_regular)
            return host;
    }
    return std::string();
}

void m_midiOutGetNumDevs(X86 *c) {
    set_eax(c, 1);
}
void m_auxGetNumDevs(X86 *c) {
    set_eax(c, 0);
} // no aux devices

void m_midiOutGetDevCapsA(X86 *c) {
    uint32_t id = arg(c, 0), out = arg(c, 1), size = arg(c, 2);
    // MIDI_MAPPER is (UINT)-1 and names the system's default device, which
    // here is the only device there is.
    if (id != 0 && id != 0xffffffffu) {
        set_eax(c, MMSYSERR_BADDEVICEID);
        return;
    }
    if (!out || !size) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t n = size < MIDIOUTCAPSA_SIZE ? size : MIDIOUTCAPSA_SIZE;
    if (!gm_valid(out, n)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }

    uint8_t caps[MIDIOUTCAPSA_SIZE];
    memset(caps, 0, sizeof caps);
    // The name is the whole point: the game accepts a device only if its first
    // nine characters are "SoundFont".
    const char *name = "SoundFont Synth";
    memcpy(caps + 8, name, strlen(name));
    caps[40] = (uint8_t)(MOD_SWSYNTH & 0xff); // wTechnology
    caps[41] = (uint8_t)(MOD_SWSYNTH >> 8);
    caps[42] = 32;
    caps[43] = 0; // wVoices
    caps[44] = 32;
    caps[45] = 0; // wNotes
    caps[46] = 0xff;
    caps[47] = 0xff; // wChannelMask: all sixteen
    for (uint32_t i = 0; i < n; ++i)
        wr8(out + i, caps[i]);
    set_eax(c, MMSYSERR_NOERROR);
}

// Open the supported MIDI output and publish the host synth prepared before guest startup.
// Avoid parsing sound banks while holding the guest scheduler baton, which would stall every worker.
void m_midiOutOpen(X86 *c) {
    uint32_t out = arg(c, 0), id = arg(c, 1);
    uint32_t callback = arg(c, 2), instance = arg(c, 3), flags = arg(c, 4);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    if (id != 0 && id != 0xffffffffu) {
        set_eax(c, MMSYSERR_BADDEVICEID);
        return;
    }

    std::string sf2 = midi_soundfont_path();
    MidiOut &m = midi();

    // This runs on a guest thread, which holds the scheduler baton for the
    // whole call, so every other guest thread is stopped until it returns.
    // The host builds its synth before the guest starts and this call only
    // publishes it, so the measurement below should always read zero. It is
    // kept because it is the cheap guard against that changing: if a host ever
    // parses a bank here instead, the game freezes at the moment the music
    // starts with nothing anywhere to say why, and this turns that into a line.
    uint32_t before = host_millis();
    m.synth = host_midi_open(sf2.empty() ? nullptr : sf2.c_str()) != 0;
    uint32_t took = host_millis() - before;
    // Two milliseconds, not fifty. The expected reading is zero, so the
    // threshold has to be tight enough to catch the mistake that can actually
    // happen now - a parse moving back into the open - rather than only a
    // repeat of the one that was designed out. A twenty-millisecond parse
    // stutters every time the music starts and would sail under fifty.
    if (took > 2) {
        LOGW("midiOutOpen: the host took %u ms to open the synth, and it was "
             "holding the scheduler baton for all of it - every guest thread "
             "was stopped. This call should publish a synth that already "
             "exists; whatever it is doing belongs in host_midi_startup, "
             "before the guest runs",
             took);
    }
    // A host with no synth is not a reason to fail the open. The game has one
    // MIDI path and no fallback: refusing here loses the music and nothing
    // else, and a device that accepts messages and plays nothing is what a
    // silent build should look like.
    if (!m.synth) {
        log_once("midiOutOpen",
                 "midiOutOpen: the host has no synth%s, so the music is "
                 "accepted and not heard",
                 sf2.empty() ? " and Sound/POPFIGHT.SF2 was not found" : "");
    } else {
        LOGW("midiOutOpen: SoundFont synth open on %s", sf2.c_str());
    }
    m.open = true;
    // CALLBACK_FUNCTION is 0x30000 in fdwOpen. The game passes zero, so this
    // is here to be right rather than because it is used.
    m.callback = (flags & 0x70000u) == 0x30000u ? callback : 0;
    m.instance = instance;
    m.flags = flags;
    m.shorts = m.sysexes = 0;
    wr32(out, MIDI_HANDLE);
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutClose(X86 *c) {
    MidiOut &m = midi();
    if (arg(c, 0) != MIDI_HANDLE || !m.open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    host_midi_close();
    LOGV("midiOutClose: %u short messages and %u sysexes were sent", m.shorts, m.sysexes);
    m = MidiOut();
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutReset(X86 *c) {
    if (arg(c, 0) != MIDI_HANDLE || !midi().open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    host_midi_reset();
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutShortMsg(X86 *c) {
    MidiOut &m = midi();
    if (arg(c, 0) != MIDI_HANDLE || !m.open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    ++m.shorts;
    host_midi_short(arg(c, 1));
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutPrepareHeader(X86 *c) {
    uint32_t hdr = arg(c, 1), size = arg(c, 2);
    if (arg(c, 0) != MIDI_HANDLE || !midi().open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    if (!hdr || size < MIDIHDR_SIZE || !gm_valid(hdr, MIDIHDR_SIZE)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    // Preparing is page-locking the buffer on Windows. There is nothing to
    // lock here, but the flag has to be set: midiOutLongMsg refuses a header
    // that does not carry it, and so does this one.
    wr32(hdr + MHDR_OFF_dwFlags, (rd32(hdr + MHDR_OFF_dwFlags) | MHDR_PREPARED) & ~MHDR_DONE);
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutUnprepareHeader(X86 *c) {
    uint32_t hdr = arg(c, 1), size = arg(c, 2);
    if (arg(c, 0) != MIDI_HANDLE || !midi().open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    if (!hdr || size < MIDIHDR_SIZE || !gm_valid(hdr, MIDIHDR_SIZE)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t flags = rd32(hdr + MHDR_OFF_dwFlags);
    // A header still in the queue may not be unprepared; nothing here queues,
    // so that can only happen if the guest never let go of one.
    if (flags & MHDR_INQUEUE) {
        set_eax(c, 65 /* MIDIERR_STILLPLAYING */);
        return;
    }
    wr32(hdr + MHDR_OFF_dwFlags, flags & ~MHDR_PREPARED);
    set_eax(c, MMSYSERR_NOERROR);
}

void m_midiOutLongMsg(X86 *c) {
    uint32_t hdr = arg(c, 1), size = arg(c, 2);
    MidiOut &m = midi();
    if (arg(c, 0) != MIDI_HANDLE || !m.open) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    if (!hdr || size < MIDIHDR_SIZE || !gm_valid(hdr, MIDIHDR_SIZE)) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    uint32_t flags = rd32(hdr + MHDR_OFF_dwFlags);
    if (!(flags & MHDR_PREPARED)) {
        set_eax(c, MIDIERR_UNPREPARED);
        return;
    }

    uint32_t data = rd32(hdr + MHDR_OFF_lpData);
    uint32_t len = rd32(hdr + MHDR_OFF_dwBufferLength);
    if (len && (!data || !gm_valid(data, len))) {
        set_eax(c, MMSYSERR_INVALPARAM);
        return;
    }
    if (len) {
        ++m.sysexes;
        host_midi_sysex(gm_ptr(data), len);
    }

    // Sent, so it is done and out of the queue before this returns. Windows
    // sets these before the MOM_DONE callback and the guest reads them to know
    // it may reuse the buffer.
    wr32(hdr + MHDR_OFF_dwFlags, (flags | MHDR_DONE) & ~MHDR_INQUEUE);
    if (m.callback) {
        // MOM_DONE is 0x3c9. Windows calls this from its own thread; here it
        // is called from the guest thread that sent the message, which is the
        // only thread allowed to re-enter translated code.
        guest_call(c, m.callback, MIDI_HANDLE, 0x3c9, m.instance, hdr);
    }
    set_eax(c, MMSYSERR_NOERROR);
}
void m_mciSendCommandA(X86 *c) {
    log_once("mciSendCommandA", "mciSendCommandA(%08x): MCI is not implemented", arg(c, 1));
    set_eax(c, 266); // MCIERR_DEVICE_NOT_INSTALLED
}

} // namespace

// The bank's path, for a host that loads it before the guest starts. Inside
// midiOutOpen would be a guest thread holding the scheduler baton, and a
// SoundFont parse there stops every other guest thread for its duration.
std::string win32_midi_soundfont_path() {
    return midi_soundfont_path();
}

// ---------------------------------------------------------------------------
// The synth, when there is no host to provide one. Weak, so a host's own
// definitions win; a build without one accepts every message and plays
// nothing, which is what the shims above already report.
// ---------------------------------------------------------------------------
extern "C" {
__attribute__((weak)) int host_midi_open(const char *) {
    return 0;
}
__attribute__((weak)) void host_midi_short(uint32_t) {}
__attribute__((weak)) void host_midi_sysex(const void *, uint32_t) {}
__attribute__((weak)) void host_midi_reset(void) {}
__attribute__((weak)) void host_midi_close(void) {}
}

// More than one subsystem needs the once-a-frame seam, so this is a list and
// not a slot.
//
// It was a slot, and the audio shim held it, because audio was the first thing
// that needed a per-frame tick. The display then needed the same seam - this
// is the only place the guest's own message loop reaches once a frame - and
// the shape of the tree invited putting a display call inside the audio pump.
// That would have made the display's frame boundary a detail of qmixer.cpp,
// where nobody looking for it would find it, and would have made removing the
// audio pump silently remove the display's.
//
// Four is more than the subsystems that exist; registering more is a
// programming error rather than a condition to handle at runtime, so it says
// so and drops the extra rather than growing.
namespace {
const int kMaxFramePumps = 4;
void (*g_frame_pumps[kMaxFramePumps])(X86 *) = {nullptr, nullptr, nullptr, nullptr};
int g_frame_pump_count = 0;
} // namespace

void host_set_frame_pump(void (*fn)(X86 *c)) {
    if (!fn) {
        g_frame_pump_count = 0;
        return;
    } // clearing takes them all
    for (int i = 0; i < g_frame_pump_count; ++i)
        if (g_frame_pumps[i] == fn)
            return; // registering twice is once
    if (g_frame_pump_count >= kMaxFramePumps) {
        log_once("frame-pump-full",
                 "more than %d frame pumps registered; the last is dropped and "
                 "its subsystem will not tick",
                 kMaxFramePumps);
        return;
    }
    g_frame_pumps[g_frame_pump_count++] = fn;
}

void host_pump_timers(X86 *c) {
    // Before the timers, because a shim registered here may want to start a
    // sound that a timer callback then asks about.
    for (int i = 0; i < g_frame_pump_count; ++i)
        g_frame_pumps[i](c);

    if (timers().empty())
        return;
    uint32_t now = host_millis();
    std::vector<uint32_t> due;
    for (auto &kv : timers())
        if (kv.second.alive && (int32_t)(now - kv.second.next_due) >= 0)
            due.push_back(kv.first);
    for (uint32_t id : due) {
        auto it = timers().find(id);
        if (it == timers().end())
            continue;
        MmTimer t = it->second;
        if (t.periodic)
            it->second.next_due = now + t.period;
        else
            timers().erase(it);
        if (!t.callback)
            continue;
        if (t.event_mode) {
            if (!win32_signal_event(t.callback, t.event_pulse))
                log_once("timer-event-handle",
                         "timeSetEvent timer %u refers to handle %08x, which is not an event", t.id,
                         t.callback);
        } else {
            host_note_cadence("mm_callback");
            uint32_t args[5] = {t.id, 0, t.user, 0, 0};
            guest_call(c, t.callback, args, 5);
        }
    }
}

const ImportShim g_misc_shims[] = {
    // ADVAPI32
    {"ADVAPI32.dll", "RegOpenKeyA", 3, a_RegOpenKeyA},
    {"ADVAPI32.dll", "RegOpenKeyExA", 5, a_RegOpenKeyExA},
    {"ADVAPI32.dll", "RegCreateKeyExA", 9, a_RegCreateKeyExA},
    {"ADVAPI32.dll", "RegCloseKey", 1, a_RegCloseKey},
    {"ADVAPI32.dll", "RegQueryValueExA", 6, a_RegQueryValueExA},
    {"ADVAPI32.dll", "RegSetValueExA", 6, a_RegSetValueExA},
    // GDI32
    {"GDI32.dll", "GetStockObject", 1, g_GetStockObject},
    {"GDI32.dll", "GetSystemPaletteEntries", 4, g_GetSystemPaletteEntries},
    // SHELL32
    {"SHELL32.dll", "ShellExecuteA", 6, s_ShellExecuteA},
    // ole32
    {"ole32.dll", "CoInitialize", 1, o_CoInitialize},
    {"ole32.dll", "CoUninitialize", 0, o_CoUninitialize},
    // IMM32
    {"IMM32.dll", "ImmGetContext", 1, i_ImmGetContext},
    {"IMM32.dll", "ImmReleaseContext", 2, i_ImmReleaseContext},
    {"IMM32.dll", "ImmGetOpenStatus", 1, i_ImmGetOpenStatus},
    {"IMM32.dll", "ImmSetOpenStatus", 2, i_ImmSetOpenStatus},
    {"IMM32.dll", "ImmGetCompositionStringA", 4, i_ImmGetCompositionStringA},
    {"IMM32.dll", "ImmGetCandidateListA", 4, i_ImmGetCandidateListA},
    {"IMM32.dll", "ImmSetCompositionWindow", 2, i_ImmSetCompositionWindow},
    // WSOCK32. D3DPopTB.exe imports these five by ordinal, so each shim is
    // registered under both the ordinal the IAT uses and the documented name
    // (which is what GetProcAddress would ask for).
    {"WSOCK32.dll", "WSAStartup", 2, w_WSAStartup},
    {"WSOCK32.dll", "ord115", 2, w_WSAStartup},
    {"WSOCK32.dll", "WSACleanup", 0, w_WSACleanup},
    {"WSOCK32.dll", "ord116", 0, w_WSACleanup},
    {"WSOCK32.dll", "gethostname", 2, w_gethostname},
    {"WSOCK32.dll", "ord57", 2, w_gethostname},
    {"WSOCK32.dll", "gethostbyname", 1, w_gethostbyname},
    {"WSOCK32.dll", "ord52", 1, w_gethostbyname},
    {"WSOCK32.dll", "inet_ntoa", 1, w_inet_ntoa},
    {"WSOCK32.dll", "ord11", 1, w_inet_ntoa},
    // WINMM: implemented
    {"WINMM.dll", "timeGetTime", 0, m_timeGetTime},
    {"WINMM.dll", "timeSetEvent", 5, m_timeSetEvent},
    {"WINMM.dll", "timeKillEvent", 1, m_timeKillEvent},
    {"WINMM.dll", "timeBeginPeriod", 1, m_timeBeginPeriod},
    {"WINMM.dll", "timeEndPeriod", 1, m_timeEndPeriod},
    {"WINMM.dll", "mmioOpenA", 3, m_mmioOpenA},
    {"WINMM.dll", "mmioRead", 3, m_mmioRead},
    {"WINMM.dll", "mmioSeek", 3, m_mmioSeek},
    {"WINMM.dll", "mmioClose", 2, m_mmioClose},
    {"WINMM.dll", "mmioSetBuffer", 4, m_mmioSetBuffer},
    // Not imported by this EXE, but mmio is only coherent with both halves.
    {"WINMM.dll", "mmioWrite", 3, m_mmioWrite},
    {"WINMM.dll", "mmioFlush", 2, m_mmioFlush},
    {"WINMM.dll", "midiOutGetNumDevs", 0, m_midiOutGetNumDevs},
    {"WINMM.dll", "auxGetNumDevs", 0, m_auxGetNumDevs},
    {"WINMM.dll", "midiOutGetDevCapsA", 3, m_midiOutGetDevCapsA},
    {"WINMM.dll", "midiOutOpen", 5, m_midiOutOpen},
    {"WINMM.dll", "midiOutClose", 1, m_midiOutClose},
    {"WINMM.dll", "midiOutReset", 1, m_midiOutReset},
    {"WINMM.dll", "midiOutShortMsg", 2, m_midiOutShortMsg},
    {"WINMM.dll", "midiOutLongMsg", 3, m_midiOutLongMsg},
    {"WINMM.dll", "midiOutPrepareHeader", 3, m_midiOutPrepareHeader},
    {"WINMM.dll", "midiOutUnprepareHeader", 3, m_midiOutUnprepareHeader},
    {"WINMM.dll", "mciSendCommandA", 4, m_mciSendCommandA},
    // WINMM: logging-only, correct stdcall pop counts so the guest stack stays
    // balanced. MIDI and aux output belong to the audio task.
    {"WINMM.dll", "auxGetDevCapsA", 3, nullptr},
    {"WINMM.dll", "auxGetVolume", 2, nullptr},
    {"WINMM.dll", "auxSetVolume", 2, nullptr},
    // DirectX and third-party DLLs are owned by later tasks; these entries only
    // record the callee's pop count so an early call cannot unbalance the stack.
    {"DDRAW.dll", "DirectDrawCreate", 3, nullptr},
    {"DDRAW.dll", "DirectDrawEnumerateA", 2, nullptr},
    {"DINPUT.dll", "DirectInputCreateA", 4, nullptr},
    {"DSOUND.dll", "ord1", 3, nullptr}, // DirectSoundCreate

    // QMIXER and weanetr: the audio and network tasks own the behaviour, but a
    // callee-cleanup DLL must pop its arguments or the guest stack drifts. The
    // counts below are recovered, not guessed: each one is the `ret n` the
    // export actually executes in original/gog/QMixer.dll and
    // original/gog/WEANETR.dll, reached by a fall-through walk from the export
    // address. For weanetr every count independently matches the MSVC name
    // mangling (`QAE` = __thiscall, `this` in ECX, the rest on the stack), so
    // the two derivations agree on all 17 methods.
    {"QMIXER.dll", "QSWaveMixSetSpeakerPlacement", 2, nullptr},
    {"QMIXER.dll", "QSWaveMixSetSpeedOfSound", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixSetPanRate", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetListenerOrientation", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetListenerPosition", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixSetVolume", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetDistanceMapping", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetSourceCone", 6, nullptr},
    {"QMIXER.dll", "QSWaveMixSetFrequency", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetListenerVelocity", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixSetSourceVelocity", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetSourcePosition", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixSetPosition", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixRestartChannel", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixPauseChannel", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixStopChannel", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixConfigureChannel", 5, nullptr},
    {"QMIXER.dll", "QSWaveMixEnableChannel", 4, nullptr},
    {"QMIXER.dll", "QSWaveMixOpenWaveEx", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixFreeWave", 2, nullptr},
    {"QMIXER.dll", "QSWaveMixPlayEx", 6, nullptr},
    {"QMIXER.dll", "QSWaveMixCloseSession", 1, nullptr},
    {"QMIXER.dll", "QSWaveMixGetDirectSound", 2, nullptr},
    {"QMIXER.dll", "QSWaveMixInitEx", 1, nullptr},
    {"QMIXER.dll", "QSWaveMixActivate", 2, nullptr},
    {"QMIXER.dll", "QSWaveMixOpenChannel", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixSetOptions", 3, nullptr},
    {"QMIXER.dll", "QSWaveMixPump", 0, nullptr},
    {"weanetr.dll", "?SendData@MLDPlay@@QAEHKPAXKKPAK@Z", 5, nullptr},
    {"weanetr.dll", "?GetPlayerInfo@MLDPlay@@QAEHPAUMLDPLAY_PLAYERINFO@@@Z", 1, nullptr},
    {"weanetr.dll", "?EnumerateServices@MLDPlay@@QAEHP6GXPAXPAGPAU_GUID@@K0@Z0@Z", 2, nullptr},
    {"weanetr.dll",
     "?AreWeLobbied@MLDPlay@@QAEKP6GXKPAXKK0@ZPAU_GUID@@PAUMLDPLAY_LOBBYINFO@@PAKPAG5KK@Z", 8,
     nullptr},
    {"weanetr.dll", "?StartupNetwork@MLDPlay@@QAEHP6GXKPAXKK0@Z@Z", 1, nullptr},
    {"weanetr.dll", "?ShutdownNetwork@MLDPlay@@QAEHXZ", 0, nullptr},
    {"weanetr.dll", "?SendMSResults@MLDPlay@@QAEKPAD@Z", 1, nullptr},
    {"weanetr.dll", "?EnumerateNetworkMediums@MLDPlay@@QAEKP6GXPAGPAX@Z1@Z", 2, nullptr},
    {"weanetr.dll", "?GetCurrentMs@MLDPlay@@QAEKXZ", 0, nullptr},
    {"weanetr.dll", "?CreateSession@MLDPlay@@QAEHPAKPAG1PAXK@Z", 5, nullptr},
    {"weanetr.dll", "?EnumerateSessions@MLDPlay@@QAEHKP6GXPAUMLDPLAY_SESSIONDESC@@PAX@ZK1@Z", 4,
     nullptr},
    {"weanetr.dll", "?JoinSession@MLDPlay@@QAEHPAUMLDPLAY_SESSIONDESC@@PAKPAGPAX@Z", 4, nullptr},
    {"weanetr.dll", "?CreateNetworkAddress@MLDPlay@@QAEHPAXK0PAK@Z", 4, nullptr},
    {"weanetr.dll", "?EnableNewPlayers@MLDPlay@@QAEXH@Z", 1, nullptr},
    {"weanetr.dll", "?DestroySession@MLDPlay@@QAEHXZ", 0, nullptr},
    {"weanetr.dll", "?SetupConnection@MLDPlay@@QAEHPAXPAU_GUID@@0@Z", 3, nullptr},
    {"weanetr.dll", "?SendChat@MLDPlay@@QAEHKPAGKPAK@Z", 4, nullptr},
};
const size_t g_misc_shim_count = sizeof(g_misc_shims) / sizeof(g_misc_shims[0]);
