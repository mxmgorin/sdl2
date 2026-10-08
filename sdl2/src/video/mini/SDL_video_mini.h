// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#ifndef __SDL_VIDEO_Mini_H__
#define __SDL_VIDEO_Mini_H__

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

#include <stdint.h>
#include <stdbool.h>
#include <linux/fb.h>

#include "mi_sys.h"
#include "mi_gfx.h"

#ifndef MAX_PATH
    #define MAX_PATH 128
#endif

#define DEF_FB_W 640
#define DEF_FB_H 480
/* A sanity bound on what `fbset` reports, so a garbled line cannot size the
   framebuffer allocations. */
#define MAX_FB_W 2048
#define MAX_FB_H 2048
#define FB_BPP   4

/* One PNG or a folder of them, drawn around the app's picture. */
#define BEZEL_ENV       "SDL_MINI_BEZEL"
/* A file holding the chosen bezel's name, so the choice outlives the run. */
#define BEZEL_SAVE_ENV  "SDL_MINI_BEZEL_SAVE"
/* A folder deeper than this is not a bezel pack. */
#define BEZEL_MAX       64

/* The pointer arrow's size and colours; black and white are the same bytes whichever
   way round the channels are. */
#define POINTER_W       9
#define POINTER_H       14
#define POINTER_OUTLINE 0xff000000
#define POINTER_FILL    0xffffffff
/* A cache flush starts on a page boundary. */
#define FB_FLUSH_ALIGN  4096

#if 0
    #define debug(...) printf(__VA_ARGS__)
#else
    #define debug(...) (void)0
#endif

typedef struct _GFX {
    int fb_dev;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    struct _DMA {
        void *virAddr;
        MI_PHY phyAddr;
    } fb, tmp, overlay;

    struct _HW {
        struct _BUF {
            MI_GFX_Surface_t surf;
            MI_GFX_Rect_t rt;
        } src, dst, overlay;
        MI_GFX_Opt_t opt;
    } hw;
} GFX;

void GFX_Clear(void);
void GFX_Flip(void);
int GFX_Copy(const void *pixels, SDL_Rect srcrect, SDL_Rect dstrect, int pitch, int alpha, int rotate);

/* Walks the bezel folder; 0 when there is nothing to walk, so the key it came from can
   still reach the app. */
int Mini_BezelStep(int step);
void Mini_DrawPointer(int px, int py, int mag, const SDL_Rect *clip);

#endif

