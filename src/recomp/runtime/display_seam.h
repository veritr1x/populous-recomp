#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal, unpinned host/mod seam. No AppKit or frame pointers cross it. */
int32_t host_display_anchor(uint64_t id, int8_t h, int8_t v, int clear);
uint32_t host_display_elements(uint64_t *ids, uint32_t max);
float host_display_aspect(void);
uint64_t host_display_epoch(void);
int host_display_offer_mode(int w, int h, int bpp);
void host_display_request_window(int mode); /* queued; 0 windowed, 1 borderless, 2 fullscreen */
int host_display_take_window(void);         /* main thread drains; -1 when unchanged */
void mods_display_transition(uint64_t epoch, int screen_class);
int mods_display_classic(void);
int mods_display_scale(void);
int mods_display_wide(void);
void mods_display_scene_domain(int w, int h);
int mods_display_scene_width(int guest_w, int guest_h);
int mods_display_fps(void);       /* 0 original, otherwise requested new-frame limit */
int mods_display_overlay(void);   /* 0 off, 1 counters, 2 frame-time graph */
int mods_display_textures(void);  /* 0 original, 1 HD pack */
int mods_display_filtering(void); /* 0 original, 1 trilinear, 2 4x, 3 8x, 4 16x */
#ifdef __cplusplus
}
#endif
