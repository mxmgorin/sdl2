// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MINI

#include <time.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>

#include "../../events/SDL_events_c.h"
#include "../SDL_sysvideo.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"

#include "SDL_image.h"
#include "SDL_version.h"
#include "SDL_syswm.h"
#include "SDL_loadso.h"
#include "SDL_events.h"
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_video_mini.h"
#include "SDL_event_mini.h"
#include "SDL_gles_mini.h"
#include "SDL_fb_mini.h"

/* A bezel is the only PNG here, so the decoder is vendored: SDL2_image would add libpng
   and libz to a shared library. */
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
/* Nothing here reads its failure string, and the string is what pulls thread-local
   storage into a library that is LD_PRELOADed. */
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC     SDL_malloc
#define STBI_REALLOC    SDL_realloc
#define STBI_FREE       SDL_free
#define STBI_ASSERT(x)  SDL_assert(x)
#define STB_IMAGE_IMPLEMENTATION
/* Upstream's own warnings, in a file that is not ours to fix. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "stb_image.h"
#pragma GCC diagnostic pop

GFX gfx = { 0 };

int FB_W = 0;
int FB_H = 0;
int FB_SIZE = 0;
int TMP_SIZE = 0;
SDL_Window *vid_win = NULL;

static int Mini_VideoInit(_THIS);
static int Mini_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static void Mini_VideoQuit(_THIS);

int Mini_InitGFX(void)
{
    debug("%s\n", __func__);
    MI_SYS_Init();
    MI_GFX_Open();

    gfx.fb_dev = open("/dev/fb0", O_RDWR);
    ioctl(gfx.fb_dev, FBIOGET_FSCREENINFO, &gfx.finfo);
    ioctl(gfx.fb_dev, FBIOGET_VSCREENINFO, &gfx.vinfo);
    gfx.vinfo.yoffset = 0;
    gfx.vinfo.yres_virtual = gfx.vinfo.yres * 2;
    ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);

    gfx.fb.phyAddr = gfx.finfo.smem_start;
    MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, FB_SIZE);
    MI_SYS_Mmap(gfx.fb.phyAddr, gfx.finfo.smem_len, &gfx.fb.virAddr, TRUE);
    memset(&gfx.hw.opt, 0, sizeof(gfx.hw.opt));

    MI_SYS_MMA_Alloc(NULL, TMP_SIZE, &gfx.tmp.phyAddr);
    MI_SYS_Mmap(gfx.tmp.phyAddr, TMP_SIZE, &gfx.tmp.virAddr, TRUE);

    MI_SYS_MMA_Alloc(NULL, TMP_SIZE, &gfx.overlay.phyAddr);
    MI_SYS_Mmap(gfx.overlay.phyAddr, TMP_SIZE, &gfx.overlay.virAddr, TRUE);
    return 0;
}

int Mini_QuitGFX(void)
{
    debug("%s\n", __func__);
    MI_SYS_Munmap(gfx.fb.virAddr, FB_SIZE);
    MI_SYS_Munmap(gfx.tmp.virAddr, TMP_SIZE);
    MI_SYS_MMA_Free(gfx.tmp.phyAddr);
    MI_SYS_Munmap(gfx.overlay.virAddr, TMP_SIZE);
    MI_SYS_MMA_Free(gfx.overlay.phyAddr);

    MI_GFX_Close();
    MI_SYS_Exit();
    return 0;
}

void GFX_Init(void)
{
    debug("%s\n", __func__);
    Mini_InitGFX();
}

void GFX_Quit(void)
{
    debug("%s\n", __func__);

    GFX_Clear();
    Mini_QuitGFX();
    gfx.vinfo.yoffset = 0;
    ioctl(gfx.fb_dev, FBIOPUT_VSCREENINFO, &gfx.vinfo);
    close(gfx.fb_dev);
    gfx.fb_dev = 0;
}

void GFX_Clear(void)
{
    debug("%s\n", __func__);
    MI_SYS_MemsetPa(gfx.fb.phyAddr, 0, FB_SIZE);
    MI_SYS_MemsetPa(gfx.tmp.phyAddr, 0, TMP_SIZE);
}

int GFX_Copy(const void *pixels, SDL_Rect srt, SDL_Rect drt, int pitch, int alpha, int rotate)
{
    MI_U16 u16Fence = 0;
    int rgb565 = (pitch / srt.w) == 2 ? 1 : 0;

    debug("%s, pixels=%p, rgb565=%d\n", __func__, pixels, rgb565);
    memcpy(gfx.tmp.virAddr, pixels, srt.h * pitch);

    gfx.hw.opt.u32GlobalSrcConstColor = 0;
    gfx.hw.opt.eRotate = rotate;
    gfx.hw.opt.eSrcDfbBldOp = E_MI_GFX_DFB_BLD_ONE;
    gfx.hw.opt.eDstDfbBldOp = 0;
    gfx.hw.opt.eDFBBlendFlag = 0;

    gfx.hw.src.rt.s32Xpos = srt.x;
    gfx.hw.src.rt.s32Ypos = srt.y;
    gfx.hw.src.rt.u32Width = srt.w;
    gfx.hw.src.rt.u32Height = srt.h;
    gfx.hw.src.surf.u32Width = srt.w;
    gfx.hw.src.surf.u32Height = srt.h;
    gfx.hw.src.surf.u32Stride = pitch;
    gfx.hw.src.surf.eColorFmt = rgb565 ? E_MI_GFX_FMT_RGB565 : E_MI_GFX_FMT_ARGB8888;
    gfx.hw.src.surf.phyAddr = gfx.tmp.phyAddr;

    gfx.hw.dst.rt.s32Xpos = drt.x;
    gfx.hw.dst.rt.s32Ypos = drt.y;
    gfx.hw.dst.rt.u32Width = drt.w;
    gfx.hw.dst.rt.u32Height = drt.h;
    gfx.hw.dst.surf.u32Width = FB_W;
    gfx.hw.dst.surf.u32Height = FB_H;
    gfx.hw.dst.surf.u32Stride = FB_W * FB_BPP;
    gfx.hw.dst.surf.eColorFmt = E_MI_GFX_FMT_ARGB8888;
    gfx.hw.dst.surf.phyAddr = gfx.fb.phyAddr + (FB_W * gfx.vinfo.yoffset * FB_BPP);

    MI_SYS_FlushInvCache(gfx.tmp.virAddr, pitch * srt.h);
    MI_GFX_BitBlit(&gfx.hw.src.surf, &gfx.hw.src.rt, &gfx.hw.dst.surf, &gfx.hw.dst.rt, &gfx.hw.opt, &u16Fence);
    MI_GFX_WaitAllDone(TRUE, u16Fence);
    return 0;
}

void GFX_Flip(void)
{
    debug("%s\n", __func__);
    ioctl(gfx.fb_dev, FBIOPAN_DISPLAY, &gfx.vinfo);
    gfx.vinfo.yoffset ^= FB_H;
}

void* GFX_CB(void)
{
    SDL_Rect srt = { 0, 0, vid_win->w, vid_win->h };
    SDL_Rect drt = { 0, 0, FB_W, FB_H };

    debug("%s src(%d,%d,%d,%d) dst(%d,%d,%d,%d)\n", __func__, srt.x, srt.y, srt.w, srt.h, drt.x, drt.y, drt.w, drt.h);
    GFX_Copy(gfx.tmp.virAddr, srt, drt, srt.w * FB_BPP, 0, E_MI_GFX_ROTATE_180);
    GFX_Flip();
    return gfx.tmp.virAddr;
}

static uint32_t *bezel_pix = NULL;
static int bezel_w = 0;
static int bezel_h = 0;
static char *bezel_file[BEZEL_MAX] = { NULL };
static int bezel_count = 0;
static int bezel_now = 0;

static void bezel_free(void)
{
    stbi_image_free(bezel_pix);
    bezel_pix = NULL;
    bezel_w = 0;
    bezel_h = 0;
}

/* One bezel is held decoded, not the whole folder: each is the panel's size in
   ARGB8888 and these devices have 128 MB. */
static int bezel_load(const char *path)
{
    void *file = NULL;
    size_t len = 0;
    stbi_uc *rgba = NULL;
    int w = 0;
    int h = 0;
    int comp = 0;
    int c0 = 0;

    /* Logged rather than debugged: a missing bezel looks the same however it failed. */
    bezel_free();
    file = SDL_LoadFile(path, &len);
    if (file == NULL) {
        SDL_Log("Mini: bezel %s cannot be read", path);
        return -1;
    }

    rgba = stbi_load_from_memory(file, (int)len, &w, &h, &comp, 4);
    SDL_free(file);
    if (rgba == NULL) {
        SDL_Log("Mini: bezel %s is not a PNG this can decode", path);
        return -1;
    }
    if ((w > FB_W) || (h > FB_H)) {
        SDL_Log("Mini: bezel %s is %dx%d, bigger than the panel's %dx%d", path, w, h,
            FB_W, FB_H);
        stbi_image_free(rgba);
        return -1;
    }
    if ((w < FB_W) || (h < FB_H)) {
        SDL_Log("Mini: bezel %s is %dx%d on a %dx%d panel, centred with black around it",
            path, w, h, FB_W, FB_H);
    }

    /* stb hands back R,G,B,A; what the blitter calls ARGB8888 is B,G,R,A in memory. */
    for (c0 = 0; c0 < (w * h * 4); c0 += 4) {
        stbi_uc r = rgba[c0];

        rgba[c0] = rgba[c0 + 2];
        rgba[c0 + 2] = r;
    }

    bezel_pix = (uint32_t *)rgba;
    bezel_w = w;
    bezel_h = h;
    return 0;
}

/* Into both framebuffer halves, as only the app's rect is ever redrawn. Centred at its
   own size: MI_GFX filters a resize. */
static void bezel_draw(void)
{
    SDL_Rect srt = { 0, 0, bezel_w, bezel_h };
    SDL_Rect drt = { (FB_W - bezel_w) / 2, (FB_H - bezel_h) / 2, bezel_w, bezel_h };
    int c0 = 0;

    if (bezel_pix == NULL) {
        return;
    }
    for (c0 = 0; c0 < 2; c0++) {
        GFX_Copy(bezel_pix, srt, drt, bezel_w * FB_BPP, 0, E_MI_GFX_ROTATE_180);
        /* The other buffer; twice puts the offset back where GFX_Flip expects it. */
        gfx.vinfo.yoffset ^= FB_H;
    }
}

static int bezel_name(const char *name)
{
    const char *dot = SDL_strrchr(name, '.');

    return (dot != NULL) && !SDL_strcasecmp(dot, ".png");
}

static int bezel_sort(const void *a, const void *b)
{
    return SDL_strcmp(*(const char **)a, *(const char **)b);
}

static const char *bezel_base(const char *path)
{
    const char *slash = SDL_strrchr(path, '/');

    return (slash != NULL) ? (slash + 1) : path;
}

/* The index of the bezel named in SDL_MINI_BEZEL_SAVE, matched by name, or the first
   when there is none. */
static int bezel_saved(void)
{
    const char *save = SDL_getenv(BEZEL_SAVE_ENV);
    char name[MAX_PATH] = { 0 };
    size_t len = 0;
    FILE *fp = NULL;
    int c0 = 0;

    if ((save == NULL) || ((fp = fopen(save, "r")) == NULL)) {
        return 0;
    }
    if (fgets(name, sizeof(name), fp) == NULL) {
        name[0] = '\0';
    }
    fclose(fp);
    len = SDL_strlen(name);
    while ((len > 0) && ((name[len - 1] == '\n') || (name[len - 1] == '\r'))) {
        name[--len] = '\0';
    }
    for (c0 = 0; c0 < bezel_count; c0++) {
        if (!SDL_strcmp(bezel_base(bezel_file[c0]), name)) {
            return c0;
        }
    }
    return 0;
}

static void bezel_save(void)
{
    const char *save = SDL_getenv(BEZEL_SAVE_ENV);
    FILE *fp = NULL;

    if ((save == NULL) || (save[0] == '\0')) {
        return;
    }
    fp = fopen(save, "w");
    if (fp == NULL) {
        SDL_Log("Mini: bezel choice cannot be saved to %s", save);
        return;
    }
    fprintf(fp, "%s\n", bezel_base(bezel_file[bezel_now]));
    fclose(fp);
}

/* Reads SDL_MINI_BEZEL, one PNG or a folder of them, in name order. */
static void bezel_init(void)
{
    const char *env = SDL_getenv(BEZEL_ENV);
    char path[MAX_PATH * 4] = { 0 };
    struct dirent *ent = NULL;
    DIR *dir = NULL;

    if ((env == NULL) || (env[0] == '\0')) {
        return;
    }

    dir = opendir(env);
    if (dir == NULL) {
        bezel_file[bezel_count++] = SDL_strdup(env);
    } else {
        while (((ent = readdir(dir)) != NULL) && (bezel_count < BEZEL_MAX)) {
            if (!bezel_name(ent->d_name)) {
                continue;
            }
            SDL_snprintf(path, sizeof(path), "%s/%s", env, ent->d_name);
            bezel_file[bezel_count++] = SDL_strdup(path);
        }
        closedir(dir);
        SDL_qsort(bezel_file, bezel_count, sizeof(bezel_file[0]), bezel_sort);
    }

    SDL_Log("Mini: %d bezel(s) at %s", bezel_count, env);
    bezel_now = bezel_saved();
    if ((bezel_count > 0) && (bezel_load(bezel_file[bezel_now]) == 0)) {
        bezel_draw();
        SDL_Log("Mini: bezel %s drawn", bezel_file[bezel_now]);
    }
}

static void bezel_quit(void)
{
    int c0 = 0;

    for (c0 = 0; c0 < bezel_count; c0++) {
        SDL_free(bezel_file[c0]);
        bezel_file[c0] = NULL;
    }
    bezel_count = 0;
    bezel_now = 0;
    bezel_free();
}

int Mini_BezelStep(int step)
{
    if (bezel_count < 2) {
        return 0;
    }
    bezel_now = (bezel_now + step + bezel_count) % bezel_count;
    if (bezel_load(bezel_file[bezel_now]) == 0) {
        bezel_draw();
        bezel_save();
        SDL_Log("Mini: bezel %d of %d, %s", bezel_now + 1, bezel_count,
            bezel_file[bezel_now]);
    }
    return 1;
}

/* An arrow with its tip at the top left: `X` is the outline, `.` the fill, a space is
   not drawn at all -- the blitter here does no alpha. */
static const char *pointer_art[POINTER_H] = {
    "X        ",
    "XX       ",
    "X.X      ",
    "X..X     ",
    "X...X    ",
    "X....X   ",
    "X.....X  ",
    "X......X ",
    "X.......X",
    "X....XXXX",
    "X..X.X   ",
    "X.X X.X  ",
    "XX   X.X ",
    "X     XX "
};

/* Draws the arrow straight into the framebuffer, never erased: only inside `clip`,
   which the app rewrites every frame. */
void Mini_DrawPointer(int px, int py, int mag, const SDL_Rect *clip)
{
    uint32_t *fb = (uint32_t *)gfx.fb.virAddr;
    int top = FB_H;
    int bottom = -1;
    int ax = 0;
    int ay = 0;

    if ((fb == NULL) || (mag < 1)) {
        return;
    }
    fb += FB_W * gfx.vinfo.yoffset;

    for (ay = 0; ay < (POINTER_H * mag); ay++) {
        for (ax = 0; ax < (POINTER_W * mag); ax++) {
            char ink = pointer_art[ay / mag][ax / mag];
            int sx = px + ax;
            int sy = py + ay;
            int fx = FB_W - 1 - sx;
            int fy = FB_H - 1 - sy;

            if ((ink == ' ') ||
                (sx < clip->x) || (sx >= (clip->x + clip->w)) ||
                (sy < clip->y) || (sy >= (clip->y + clip->h)) ||
                (fx < 0) || (fx >= FB_W) || (fy < 0) || (fy >= FB_H)) {
                continue;
            }
            /* The panel is mounted upside down, so every pixel goes to the opposite
               corner of the framebuffer. */
            fb[(fy * FB_W) + fx] = (ink == 'X') ? POINTER_OUTLINE : POINTER_FILL;
            if (fy < top) {
                top = fy;
            }
            if (fy > bottom) {
                bottom = fy;
            }
        }
    }

    if (bottom >= top) {
        /* The mapping is cached, so written rows are flushed before the panel reads
           them, from a page boundary. */
        uintptr_t from = (uintptr_t)&fb[top * FB_W] & ~(uintptr_t)(FB_FLUSH_ALIGN - 1);
        uintptr_t to = (uintptr_t)&fb[(bottom + 1) * FB_W];

        MI_SYS_FlushInvCache((void *)from, to - from);
    }
}

static void Mini_DeleteDevice(SDL_VideoDevice *device)
{
    debug("%s\n", __func__);
    SDL_free(device);
}

void Mini_DestroyWindow(_THIS, SDL_Window *window)
{
    debug("%s\n", __func__);
    GFX_Quit();
}

int Mini_CreateWindow(_THIS, SDL_Window *window)
{
    vid_win = window;
    SDL_SetMouseFocus(window);
    glUpdateBufferSettings(GFX_CB);
    debug("%s, win=%p, w=%d, h=%d\n", __func__, window, window->w, window->h);
    return 0;
}

int Mini_CreateWindowFrom(_THIS, SDL_Window *window, const void *data)
{
    debug("%s\n", __func__);
    return SDL_Unsupported();
}

static SDL_VideoDevice *Mini_CreateDevice(int devindex)
{
    SDL_VideoDevice *device = NULL;
    SDL_GLDriverData *gldata = NULL;

    debug("%s\n", __func__);

    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }

    device->is_dummy = SDL_TRUE;
    device->VideoInit = Mini_VideoInit;
    device->VideoQuit = Mini_VideoQuit;
    device->SetDisplayMode = Mini_SetDisplayMode;
    device->PumpEvents = Mini_PumpEvents;
    device->CreateSDLWindow = Mini_CreateWindow;
    device->CreateSDLWindowFrom = Mini_CreateWindowFrom;
    device->CreateWindowFramebuffer = Mini_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = Mini_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = Mini_DestroyWindowFramebuffer;
    device->DestroyWindow = Mini_DestroyWindow;

    device->GL_LoadLibrary = glLoadLibrary;
    device->GL_GetProcAddress = glGetProcAddress;
    device->GL_CreateContext = glCreateContext;
    device->GL_SetSwapInterval = glSetSwapInterval;
    device->GL_SwapWindow = glSwapWindow;
    device->GL_MakeCurrent = glMakeCurrent;
    device->GL_DeleteContext = glDeleteContext;
    device->GL_UnloadLibrary = glUnloadLibrary;
    
    gldata = (SDL_GLDriverData*)SDL_calloc(1, sizeof(SDL_GLDriverData));
    if (gldata == NULL) {
        SDL_OutOfMemory();
        SDL_free(device);
        return NULL;
    }

    device->gl_data = gldata;
    device->free = Mini_DeleteDevice;
    return device;
}

VideoBootStrap Mini_VideoDriver = { "Mini", "Miyoo Mini Video Driver", Mini_CreateDevice };

int Mini_VideoInit(_THIS)
{
    FILE *fd = NULL;
    char buf[MAX_PATH] = {0};
    SDL_DisplayMode mode = {0};
    SDL_VideoDisplay display = {0};

    debug("%s\n", __func__);

    FB_W = DEF_FB_W;
    FB_H = DEF_FB_H;
    fd = popen("fbset | grep \"mode \"", "r");
    if (fd) {
        int w = 0;
        int h = 0;

        /* `mode "752x560-60"`, the Flip's panel; the Mini's own says 640x480. */
        if (fgets(buf, sizeof(buf), fd) && (sscanf(buf, " mode \"%dx%d", &w, &h) == 2) &&
            (w >= DEF_FB_W) && (h >= DEF_FB_H) && (w <= MAX_FB_W) && (h <= MAX_FB_H)) {
            FB_W = w;
            FB_H = h;
        }
        pclose(fd);
    }
    FB_SIZE = (FB_W * FB_H * FB_BPP * 2);
    TMP_SIZE = (FB_W * FB_H * FB_BPP);

    /* The panel itself, so SDL_GetDesktopDisplayMode can answer with it. */
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = FB_W;
    mode.h = FB_H;
    mode.refresh_rate = 60;
    display.desktop_mode = mode;
    display.current_mode = mode;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 640;
    mode.h = 480;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = 640;
    mode.h = 480;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 800;
    mode.h = 480;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = 800;
    mode.h = 480;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);
    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 800;
    mode.h = 600;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = 800;
    mode.h = 600;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 320;
    mode.h = 240;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = 320;
    mode.h = 240;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_RGB565;
    mode.w = 480;
    mode.h = 272;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);

    SDL_zero(mode);
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = 480;
    mode.h = 272;
    mode.refresh_rate = 60;
    SDL_AddDisplayMode(&display, &mode);
    SDL_AddVideoDisplay(&display, SDL_FALSE);

    debug("%s, screen=%dx%d\n", __func__, FB_W, FB_H);
    GFX_Init();
    bezel_init();
    Mini_EventInit();
    return 0;
}

static int Mini_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
    debug("%s\n", __func__);
    return 0;
}

void Mini_VideoQuit(_THIS)
{
    debug("%s\n", __func__);
    Mini_EventQuit();
    bezel_quit();
}

#endif

