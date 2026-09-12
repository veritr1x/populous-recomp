#include "game_path.h"

#include "../mods/layout.h"
#include "../platform/os.h"
#include "../runtime/loader.h"

#include <stdio.h>
#include <stdlib.h>

static std::string saved_file() {
    return host_layout().profile_dir + "/game-path.txt";
}

static void mkdir_p(const std::string &path) {
    std::string acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < path.size())
            acc.push_back(path[i]);
    }
}

bool game_path_is_supported(const std::string &path, std::string *digest_out) {
    std::string digest = loader_hash_file(path.c_str());
    if (digest_out)
        *digest_out = digest;
    return !digest.empty() && digest == LOADER_EXPECTED_SHA256;
}

bool game_path_save(const std::string &path) {
    mkdir_p(host_layout().profile_dir);
    FILE *f = fopen(saved_file().c_str(), "wb");
    if (!f)
        return false;
    fputs(path.c_str(), f);
    fputc('\n', f);
    return fclose(f) == 0;
}

GamePath game_path_resolve(const char *flag) {
    GamePath g;
    if (flag && *flag) {
        g.exe = flag;
        g.source = GamePathSource::Flag;
        return g;
    }
    if (const char *env = getenv("POP_RECOMP_EXE"); env && *env) {
        g.exe = env;
        g.source = GamePathSource::Environment;
        return g;
    }
    const HostLayout &l = host_layout();
    if (l.developer) {
        std::string candidate = l.checkout_root + "/original/gog/D3DPopTB.exe";
        OsStat st;
        if (os_stat(candidate.c_str(), &st) == 0) {
            g.exe = candidate;
            g.source = GamePathSource::Checkout;
            return g;
        }
    }
    if (FILE *f = fopen(saved_file().c_str(), "rb")) {
        char line[4096] = {0};
        if (fgets(line, sizeof line, f)) {
            std::string path(line);
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            if (!path.empty() && game_path_is_supported(path, nullptr)) {
                g.exe = path;
                g.source = GamePathSource::Saved;
            }
        }
        fclose(f);
    }
    return g;
}
