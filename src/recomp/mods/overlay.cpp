// Asset overlay: profile, reverse mod load order, then the original game.
// Mutations never fall through; deleting a shadow reveals the lower file.
#include "mods_internal.h"
#include "../runtime/mods_seam.h"
#include "../runtime/win32.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <dirent.h>
#include <filesystem>
#include <limits>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

namespace {
namespace fs = std::filesystem;
struct RegistryLock {
    RegistryLock() {
        sched_registry_lock();
    }
    ~RegistryLock() {
        sched_registry_unlock();
    }
};
struct Layer {
    uint32_t id, owner;
    std::string dir;
};
std::vector<Layer> &layers() {
    static std::vector<Layer> v;
    return v;
}
std::string &profile() {
    static std::string s;
    return s;
}
uint32_t next_layer = 1;
bool sealed = false, in_init = false;
uint32_t init_owner = 0;
std::thread::id init_thread;

std::string lower(std::string s) {
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}
bool split(const std::string &rel, std::vector<std::string> *out) {
    if (!rel.empty() && rel[0] == '/')
        return false;
    std::string cur;
    for (char c : rel + "/") {
        if (c == '\\' || c == ':' || c == '\0')
            return false;
        if (c != '/') {
            cur += c;
            continue;
        }
        if (cur == ".." || cur == ".")
            return false;
        if (!cur.empty())
            out->push_back(cur);
        cur.clear();
    }
    return true;
}
bool mkdirs(const std::string &path) {
    if (path.empty())
        return false;
    std::error_code ec;
    fs::create_directories(path, ec);
    return !ec && fs::is_directory(path, ec) && !ec;
}

// Mutating walks reject symlinks beneath the configured root: a profile link
// must never turn a guest write or delete into a mutation of a lower tier.
bool resolve_in(const std::string &root, const std::string &rel, bool create, bool mutation,
                std::string *out) {
    if (root.empty())
        return false;
    std::vector<std::string> comps;
    if (!split(rel, &comps))
        return false;
    std::string host = root;
    struct stat st;
    if (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    for (size_t i = 0; i < comps.size(); ++i) {
        std::string name = comps[i];
        // Always use the directory's spelling, even on case-insensitive hosts.
        bool found = false;
        if (DIR *d = opendir(host.c_str())) {
            while (dirent *e = readdir(d)) {
                if (lower(e->d_name) == lower(name)) {
                    name = e->d_name;
                    found = true;
                    break;
                }
            }
            closedir(d);
        } else
            return false;
        host += "/" + name;
        if (!found) {
            if (!create)
                return false;
            if (i + 1 == comps.size()) {
                *out = host;
                return true;
            }
            if (mkdir(host.c_str(), 0755) != 0)
                return false;
        }
        if (lstat(host.c_str(), &st) != 0)
            return false;
        if (mutation && S_ISLNK(st.st_mode))
            return false;
        if (i + 1 < comps.size() && (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
            return false;
    }
    *out = host;
    return true;
}
std::vector<std::string> roots_snapshot() {
    RegistryLock lock;
    std::vector<std::string> roots{profile()};
    for (size_t i = layers().size(); i-- > 0;)
        roots.push_back(layers()[i].dir);
    roots.push_back(win32_game_dir());
    return roots;
}

// Publish only a complete copy. A failed read/write/close leaves no partial
// profile shadow hiding the still-good original.
bool copy_up(const std::string &from, const std::string &to) {
    struct stat st;
    if (stat(from.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    FILE *in = fopen(from.c_str(), "rb");
    if (!in)
        return false;
    std::string temp = to + ".pop-copy-XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0) {
        fclose(in);
        return false;
    }
    FILE *dest = fdopen(fd, "wb");
    if (!dest) {
        close(fd);
        unlink(temp.c_str());
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) != 0) {
        if (fwrite(buf, 1, n, dest) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in))
        ok = false;
    if (fclose(in) != 0)
        ok = false;
    if (fclose(dest) != 0)
        ok = false;
    if (ok)
        ok = rename(temp.c_str(), to.c_str()) == 0;
    if (!ok)
        unlink(temp.c_str());
    return ok;
}
int resolver(const char *relative, int op, char *out, size_t len) {
    std::string host;
    if (!relative || !out || !mods_cpp_overlay_resolve(relative, op, &host) || host.size() >= len)
        return 0;
    memcpy(out, host.c_str(), host.size() + 1);
    return 1;
}
void lister(const char *dir, void (*emit)(void *, const char *, const char *), void *ctx) {
    if (!dir || !emit)
        return;
    std::vector<std::pair<std::string, std::string>> found;
    mods_cpp_overlay_list(dir, &found);
    for (const auto &kv : found)
        emit(ctx, kv.first.c_str(), kv.second.c_str());
}
} // namespace

void mods_overlay_reset() {
    std::string dir;
    {
        RegistryLock lock;
        layers().clear();
        sealed = false;
        in_init = false;
        init_owner = 0;
        init_thread = {};
        if (profile().empty()) {
            const char *env = getenv("POPM_PROFILE_DIR");
            profile() = env && *env ? env : "build/recomp/profile";
        }
        dir = profile();
    }
    mkdirs(dir);
    win32_set_file_ops(resolver, lister);
    win32_invalidate_dir_cache();
}
void mods_overlay_set_profile_dir(const char *dir) {
    std::string value = dir ? dir : "";
    mkdirs(value);
    {
        RegistryLock lock;
        profile() = value;
    }
    win32_invalidate_dir_cache();
}
const char *mods_overlay_profile_dir() {
    thread_local std::string value;
    RegistryLock lock;
    value = profile();
    return value.c_str();
}
void mods_overlay_begin_init(uint32_t owner) {
    RegistryLock lock;
    in_init = true;
    init_owner = owner;
    init_thread = std::this_thread::get_id();
}
void mods_overlay_end_init() {
    RegistryLock lock;
    in_init = false;
    init_owner = 0;
    init_thread = {};
}
PopModStatus mods_overlay_push(uint32_t owner, const char *dir, uint32_t *out_id) {
    try {
        {
            RegistryLock lock;
            if (sealed || !in_init || owner != init_owner ||
                init_thread != std::this_thread::get_id())
                return POP_E_STATE;
        }
        if (!dir || !*dir)
            return POP_E_INVAL;
        std::string validated_dir(dir);
        struct stat st;
        if (stat(validated_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
            return POP_E_NOTFOUND;
        // Directory validation can block; recheck the window before publishing.
        RegistryLock lock;
        if (sealed || !in_init || owner != init_owner || init_thread != std::this_thread::get_id())
            return POP_E_STATE;
        if (!next_layer)
            return POP_E_LIMIT;
        const uint32_t id = next_layer;
        layers().push_back({id, owner, std::move(validated_dir)});
        ++next_layer; // Never reuse an opaque handle, including after reset.
        if (out_id)
            *out_id = id;
    } catch (const std::bad_alloc &) {
        return POP_E_NOMEM;
    }
    win32_invalidate_dir_cache();
    return POP_OK;
}
void mods_overlay_remove_all(uint32_t owner) {
    {
        RegistryLock lock;
        auto &v = layers();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [owner](const Layer &l) { return l.owner == owner; }),
                v.end());
    }
    win32_invalidate_dir_cache();
}
void mods_overlay_seal() {
    RegistryLock lock;
    sealed = true;
}
bool mods_overlay_sealed() {
    RegistryLock lock;
    return sealed;
}
uint32_t mods_overlay_layer_count() {
    RegistryLock lock;
    return (uint32_t)layers().size();
}

bool mods_cpp_overlay_resolve(const std::string &rel, int op, std::string *out) {
    if (!out)
        return false;
    out->clear();
    const auto roots = roots_snapshot();
    if (op == WIN32_FILE_READ || op == WIN32_FILE_LIST) {
        for (const auto &root : roots)
            if (resolve_in(root, rel, false, false, out))
                return true;
        return false;
    }
    if (rel.empty())
        return false; // The profile root itself is not an asset.
    if (op == WIN32_FILE_DELETE || op == WIN32_FILE_RENAME_SRC)
        return resolve_in(roots[0], rel, false, true, out);
    if (op != WIN32_FILE_WRITE && op != WIN32_FILE_RENAME_DST)
        return false;
    std::string target;
    if (!resolve_in(roots[0], rel, true, true, &target))
        return false;
    struct stat st;
    if (op == WIN32_FILE_WRITE && lstat(target.c_str(), &st) != 0) {
        for (size_t i = 1; i < roots.size(); ++i) {
            std::string source;
            if (!resolve_in(roots[i], rel, false, false, &source))
                continue;
            if (!copy_up(source, target))
                return false;
            break;
        }
    }
    *out = target;
    return true;
}
void mods_cpp_overlay_list(const std::string &rel,
                           std::vector<std::pair<std::string, std::string>> *out) {
    if (!out)
        return;
    std::unordered_set<std::string> seen;
    for (const auto &root : roots_snapshot()) {
        std::string host;
        if (!resolve_in(root, rel, false, false, &host))
            continue;
        DIR *d = opendir(host.c_str());
        if (!d)
            continue;
        while (dirent *e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            if (seen.insert(lower(e->d_name)).second)
                out->push_back({e->d_name, host + "/" + e->d_name});
        }
        closedir(d);
    }
}
void mods_fill_overlay_api(PopModApi *api) {
    if (!api)
        return;
    api->overlay_push = [](const PopModApi *a, const char *dir, uint32_t *id) -> PopModStatus {
        return a ? mods_overlay_push(a->mod_index, dir, id) : POP_E_INVAL;
    };
}
