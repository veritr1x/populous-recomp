/* Guest projection centre, clip width and sky coverage. The vertical projection,
 * camera, perspective scale and simulation records remain guest-owned.
 * Addresses and WORD widths are curated from the pinned executable's asm. */
#include "pop_mod_api.h"
#include <math.h>
#include <string.h>

POP_MOD_DECLARE_ABI();
static const PopModApi *owner;
static uint32_t width_addr, half_addr, height_addr, origin_addr, hooks[4];

static void widen(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint16_t height, native_width;
    float aspect = api->host_aspect(api);
    (void)cpu;
    (void)inv;
    (void)user;
    if (!isfinite(aspect) || aspect <= 0.0f ||
        api->guest_read_u16(api, height_addr, &height) != POP_OK || !height || height > 32767 ||
        api->guest_read_u16(api, 0x0089c6cf, &native_width) != POP_OK || !native_width)
        return;
    if (aspect < 4.0f / 3.0f)
        aspect = 4.0f / 3.0f;
    uint32_t bits;
    float origin;
    if (api->guest_read_u32(api, origin_addr, &bits) != POP_OK)
        return;
    memcpy(&origin, &bits, sizeof origin);
    if (!isfinite(origin) || origin < 0 || origin > height)
        return;
    /* The canvas includes the sidebar. The projector's width excludes it:
     * 640 canvas - 100 HUD = 540 world; 852 - 100 = 752 at 16:9.
     * Preserve vertical FOV and publish exactly the domain used by input. */
    float canvas = floorf((float)height * aspect * 0.5f) * 2.0f;
    if (canvas > 32766 || canvas <= origin)
        return;
    // A selected widescreen mode can already be wider than the host. Keep
    // the guest projection intact instead of narrowing it to the host ratio.
    if (canvas <= native_width) {
        api->set_scene_domain(api, native_width, height);
        return;
    }
    uint16_t value = (uint16_t)((canvas - origin) * 0.5f);
    uint16_t domain = (uint16_t)(value * 2 + (uint16_t)origin);
    api->guest_write_u16(api, width_addr, (uint16_t)(value * 2));
    api->guest_write_u16(api, half_addr, value);
    api->set_scene_domain(api, domain, height);
}

/* Widen AFTER each guest projection reset, before any points are projected.
 * The pinned executable writes width/half only in 0041ebf0 (frame viewport)
 * and 0046e700 (land/effect view). Hook both so non-land views also retain the
 * correction. Terrain and entities then share the centre from their first
 * point, without taking a mod game-view snapshot for every point and cull.
 * Keep the reset bodies intact, including their original mesh-bound work. */

/* Sky producers enqueue TL vertices for the original canvas. Extend only the
 * copied sky records, never the source mesh, UI, or shared screen dimensions.
 * Return PCs and queue layout are verified in the pinned GOG disassembly.
 * Registration filters those PCs before snapshot creation; keep the local
 * check too because an earlier mod callback may have edited the stack. */
static void sky_polygon(const PopModApi *api, pop_cpu_v1 *cpu, PopHookInvocation *inv, void *user) {
    uint32_t ret = 0, begin = 0, end = 0, queue = cpu->ecx;
    unsigned count = (unsigned)(uintptr_t)user;
    int sky = api->guest_read_u32(api, cpu->esp, &ret) == POP_OK &&
              (ret == 0x005176d7 || ret == 0x00517613);
    if (sky)
        api->guest_read_u32(api, queue + 0x20002a, &begin);
    if (api->call_next(api, inv, cpu) != POP_OK || !sky || !begin ||
        api->guest_read_u32(api, queue + 0x20002a, &end) != POP_OK ||
        end != begin + 0x20 + count * 0x20)
        return;
    float aspect = api->host_aspect(api);
    uint16_t height, canvas_width;
    uint32_t origin_bits;
    float origin;
    if (!isfinite(aspect) || aspect <= 0 ||
        api->guest_read_u16(api, height_addr, &height) != POP_OK || !height || height > 32767 ||
        api->guest_read_u16(api, 0x0089c6cf, &canvas_width) != POP_OK ||
        api->guest_read_u32(api, origin_addr, &origin_bits) != POP_OK)
        return;
    memcpy(&origin, &origin_bits, 4);
    if (!isfinite(origin) || origin < 0 || origin >= canvas_width)
        return;
    const int higher_mode = canvas_width > 1024 || height > 768;
    float domain = floorf(height * aspect * 0.5f) * 2.0f;
    if (domain > 32766 || (!higher_mode && domain <= canvas_width))
        return;
    if (domain < canvas_width)
        domain = canvas_width;
    // Match widen's integer half-width, including modes with an odd HUD origin.
    if (domain > canvas_width)
        domain = (uint16_t)((domain - origin) * 0.5f) * 2 + (uint16_t)origin;
    float vertical_scale = 1;
    uint16_t reference_height;
    if (higher_mode && api->guest_read_u16(api, 0x0088f036, &reference_height) == POP_OK &&
        reference_height)
        vertical_scale = (float)height / reference_height;
    /* screen_width (0089c6cf, WORD) is the original canvas used by 00517630,
     * unlike the already widened projection width. The HUD origin and canvas
     * both change at 800x600; a 480-only gate leaves the new band uncovered. */
    for (unsigned i = 0; i < count; ++i) {
        uint32_t addr = begin + 0x20 + i * 0x20, bits;
        float x;
        if (api->guest_read_u32(api, addr, &bits) != POP_OK)
            return;
        memcpy(&x, &bits, 4);
        x = origin + (x - origin) * (domain - origin) / (canvas_width - origin);
        memcpy(&bits, &x, 4);
        api->guest_write_u32(api, addr, bits);
        /* VCONFIG's sky band is in the camera record's reference pixels.
         * Scale the copied base/cloud vertices to the selected height, then
         * extend their lower edge below the horizon. This also runs at a
         * native widescreen mode, where horizontal widening is unnecessary. */
        if (vertical_scale != 1 && api->guest_read_u32(api, addr + 4, &bits) == POP_OK) {
            float y;
            memcpy(&y, &bits, 4);
            y *= vertical_scale;
            memcpy(&bits, &y, 4);
            api->guest_write_u32(api, addr + 4, bits);
        }
    }
    /* The 4:3 sky ends near the old horizon. Extend the lower boundary of
     * both the base quad and cloud mesh, preserving their edge UVs/colours
     * and blend flags. Stretching only the base exposes a grey band beneath
     * the clouds at ultrawide edges. The pinned mesh's lowest row is at
     * y=170..192, scaled by (8/3)*sky_height/480 (00517420, 0058f910). */
    if ((ret == 0x005176d7 || ret == 0x00517613) && end <= queue + 0x1ff82a) {
        uint32_t record[40] = {0}, vertex[4][8];
        for (unsigned i = 0; i < 8 + count * 8; ++i)
            if (api->guest_read_u32(api, begin + i * 4, &record[i]) != POP_OK)
                return;
        /* D3D's default repeat sampler blends UV 1 with the opposite edge.
         * The base sky is a complete image: map its 0..1 endpoints to texel
         * centres before extending its last row. Cloud UVs really repeat. */
        if (count == 4) {
            uint32_t tw = 0, th = 0;
            if (api->guest_read_u32(api, record[6] + 0x30, &tw) == POP_OK &&
                api->guest_read_u32(api, record[6] + 0x34, &th) == POP_OK && tw > 0 && th > 0 &&
                tw <= 4096 && th <= 4096) {
                for (unsigned i = 0; i < 4; ++i)
                    for (unsigned axis = 0; axis < 2; ++axis) {
                        unsigned at = 8 + i * 8 + 6 + axis;
                        float uv;
                        memcpy(&uv, &record[at], 4);
                        float size = axis ? th : tw;
                        uv = (uv * (size - 1) + 0.5f) / size;
                        memcpy(&record[at], &uv, 4);
                        api->guest_write_u32(api, begin + at * 4, record[at]);
                    }
            }
        }
        memcpy(vertex, &record[8], count * 32);

        unsigned edge[2] = {3, 2};
        if (count == 3) {
            uint32_t sky_height;
            if (api->guest_read_u32(api, 0x00a69174, &sky_height) != POP_OK || !sky_height ||
                sky_height > height)
                return;
            /* 00517290 scales mesh y by screen_height/480; 00517420 then
             * multiplies by sky_height/screen_height. The reference 480
             * therefore remains here even when the active mode is 800x600. */
            float threshold = floorf(170.0f * height / 480.0f) * (8.0f / 3.0f) * sky_height /
                                  height * vertical_scale -
                              0.01f;
            unsigned found = 0;
            for (unsigned i = 0; i < 3; ++i) {
                float y;
                memcpy(&y, &vertex[i][1], 4);
                if (y >= threshold) {
                    if (found == 2)
                        return;
                    edge[found++] = i;
                }
            }
            if (found != 2)
                return;
        }
        float x0, x1, y0, y1;
        memcpy(&x0, &vertex[edge[0]][0], 4);
        memcpy(&x1, &vertex[edge[1]][0], 4);
        memcpy(&y0, &vertex[edge[0]][1], 4);
        memcpy(&y1, &vertex[edge[1]][1], 4);
        if (!isfinite(y0) || !isfinite(y1) || y0 < 1 || y1 < 1 || y0 >= height || y1 >= height)
            return;
        if (x0 > x1) {
            unsigned swap = edge[0];
            edge[0] = edge[1];
            edge[1] = swap;
        }
        uint16_t polygons;
        if (api->guest_read_u16(api, queue + 0x1c, &polygons) != POP_OK || polygons == 65535)
            return;
        record[0] = 0x0058f518;
        record[7] = 0; // quad vtable and constructor field
        memcpy(&record[8], vertex[edge[0]], 32);
        memcpy(&record[16], vertex[edge[1]], 32);
        memcpy(&record[24], vertex[edge[1]], 32);
        memcpy(&record[32], vertex[edge[0]], 32);
        float lower = height;
        memcpy(&record[25], &lower, 4);
        memcpy(&record[33], &lower, 4);
        for (unsigned i = 0; i < 40; ++i)
            if (api->guest_write_u32(api, end + i * 4, record[i]) != POP_OK)
                return;
        api->guest_write_u32(api, queue + 0x20002a, end + 0xa0);
        api->guest_write_u16(api, queue + 0x1c, polygons + 1);
    }
}

PopModStatus pop_mod_init(const PopModApi *api) {
    PopModStatus status;
    if (!api || api->size < offsetof(PopModApi, hook_install_ex) + sizeof(api->hook_install_ex) ||
        (!api->host_aspect || !api->set_scene_domain || !api->hook_install_ex))
        return POP_E_ABI;
    if (api->symbol(api, "screen_width_2", &width_addr) != POP_OK ||
        api->symbol(api, "screen_width_2_half", &half_addr) != POP_OK ||
        api->symbol(api, "display_origin_x", &origin_addr) != POP_OK ||
        api->symbol(api, "screen_height_2", &height_addr) != POP_OK)
        return POP_E_NOSYMBOL;
    owner = api;
    memset(hooks, 0, sizeof hooks);
    /* These callbacks use direct guest/host services, never the entity/tribe
     * snapshot. Opt out explicitly while keeping nested mod views isolated. */
    status = api->hook_install_ex(api, 0x0041ebf0, 0, widen, POP_HOOK_AFTER, POP_HOOK_NO_GAME_VIEW,
                                  0, &hooks[0]);
    if (status != POP_OK)
        return status;
    status = api->hook_install_ex(api, 0x0046e700, 0, widen, POP_HOOK_AFTER, POP_HOOK_NO_GAME_VIEW,
                                  0, &hooks[1]);
    if (status != POP_OK) {
        api->hook_remove(api, hooks[0]);
        hooks[0] = 0;
    }
    if (status == POP_OK)
        status = api->hook_install_ex(api, 0x0047d980, 0x005176d7, sky_polygon, POP_HOOK_WRAP,
                                      POP_HOOK_NO_GAME_VIEW, (void *)(uintptr_t)4, &hooks[2]);
    if (status == POP_OK)
        status = api->hook_install_ex(api, 0x0047d8a0, 0x00517613, sky_polygon, POP_HOOK_WRAP,
                                      POP_HOOK_NO_GAME_VIEW, (void *)(uintptr_t)3, &hooks[3]);
    if (status != POP_OK) {
        for (unsigned i = 0; i < 4; ++i)
            if (hooks[i]) {
                api->hook_remove(api, hooks[i]);
                hooks[i] = 0;
            }
    }
    return status;
}

PopModStatus pop_mod_exit(void) {
    PopModStatus result = POP_OK;
    for (unsigned i = 0; i != 4; ++i) {
        if (!hooks[i])
            continue;
        PopModStatus status = owner->hook_remove(owner, hooks[i]);
        if (status != POP_OK)
            result = status;
        hooks[i] = 0;
    }
    return result;
}
