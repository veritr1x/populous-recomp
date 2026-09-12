/* settingsmenu.c - the settings and menu example.
 *
 * It declares its settings in mod.toml, registers one more through the API to
 * show the programmatic form, round-trips a value through settings_get and
 * settings_set, and puts a row on its own page whose callback opens that page
 * through the public contract.
 *
 * F10 belongs to the foundation, not to a mod: it opens the settings page and
 * the game never sees the key. A mod's menu entry is a row on its own page,
 * and open_settings_page is the only way a mod opens it. */
#include "pop_mod_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

POP_MOD_DECLARE_ABI();

#define UI_SCALE_WRITTEN 3 /* the manifest's default is 2 */

static const PopModApi *g_api;
static int64_t g_written = -1, g_readback = -1;
static int g_menu_registered;

/* The menu row's callback. It runs on the host thread, where the page lives. */
static void open_page(const PopModApi *api, void *user) {
    (void)user;
    api->open_settings_page(api, api->mod_id); /* the public contract */
}

PopModStatus pop_mod_init(const PopModApi *api) {
    PopSettingDesc desc;
    PopModStatus st;
    int64_t v = 0;
    char msg[256];

    g_api = api;

    /* mod.toml already declares ui_scale and show_hints. This one is declared
     * from code, which is the other half of the contract: a setting can come
     * from the manifest or from the mod, and both end up in the same store. */
    memset(&desc, 0, sizeof desc);
    desc.size = (uint32_t)sizeof desc;
    desc.key = "hud_rows";
    desc.label = "HUD rows";
    desc.kind = POP_SETTING_INT;
    desc.def = 3;
    desc.min = 1;
    desc.max = 8;
    st = api->register_setting(api, &desc);
    if (st != POP_OK) {
        snprintf(msg, sizeof msg, "settingsmenu: register_setting failed (%d)", (int)st);
        api->log(api, msg);
    }

    /* A round trip that moves the value. Writing back what was already there
     * would pass whether or not the write worked, so this writes something
     * else - UI_SCALE_WRITTEN, which is not the manifest's default - and reads
     * it back. Both numbers go into the exit file, where the test compares
     * them. Writing a constant keeps it idempotent: a second run over the same
     * profile writes the same value again. */
    if (api->settings_set(api, "ui_scale", UI_SCALE_WRITTEN) == POP_OK) {
        g_written = UI_SCALE_WRITTEN;
        if (api->settings_get(api, "ui_scale", &v) == POP_OK)
            g_readback = v;
        snprintf(msg, sizeof msg, "settingsmenu: ui_scale written %lld, read back %lld",
                 (long long)g_written, (long long)g_readback);
        api->log(api, msg);
    } else {
        api->log(api, "settingsmenu: ui_scale could not be written");
    }

    st = api->register_menu_item(api, "example.settingsmenu", "Settings example", open_page, 0);
    if (st == POP_OK) {
        g_menu_registered = 1;
        api->log(api, "settingsmenu: menu entry \"Settings example\" registered");
    } else {
        snprintf(msg, sizeof msg, "settingsmenu: register_menu_item failed (%d)", (int)st);
        api->log(api, msg);
    }

    /* Neither failure above is fatal to the mod: a host without a menu is
     * still a host this mod can run in, and a mod that refuses to load takes
     * its dependents down with it. */
    return POP_OK;
}

/* The API is LIVE here. A mod's context is revoked immediately after its own
 * pop_mod_exit returns, not before it is called, so an exit handler is the
 * last place a mod may read or write its settings - and a write from here
 * still reaches the profile on disk. The two values below are read now rather
 * than remembered from init, which is the point: if the API were dead this
 * file would say -1. */
PopModStatus pop_mod_exit(void) {
    const char *dir = getenv("POPM_PROFILE_DIR");
    char path[512];
    FILE *f;
    int64_t ui_now = -1, hud_now = -1;
    snprintf(path, sizeof path, "%s/example-settingsmenu.txt",
             dir && *dir ? dir : "build/recomp/profile");
    g_api->settings_get(g_api, "ui_scale", &ui_now);
    g_api->settings_get(g_api, "hud_rows", &hud_now);
    f = fopen(path, "wb");
    if (f) {
        fprintf(f, "ui_scale_written %lld\n", (long long)g_written);
        fprintf(f, "ui_scale_readback %lld\n", (long long)g_readback);
        fprintf(f, "ui_scale_at_exit %lld\n", (long long)ui_now);
        fprintf(f, "hud_rows %lld\n", (long long)hud_now);
        fprintf(f, "menu_registered %d\n", g_menu_registered);
        fclose(f);
    }
    return POP_OK;
}
