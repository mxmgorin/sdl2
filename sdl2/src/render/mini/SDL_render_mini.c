// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#include "../../SDL_internal.h"

#if SDL_VIDEO_RENDER_MINI

#include <unistd.h>
#include <stdbool.h>

#include "SDL_hints.h"
#include "../SDL_sysrender.h"
#include "../../video/mini/SDL_video_mini.h"
#include "../../video/mini/SDL_event_mini.h"

typedef struct Mini_TextureData {
    void *data;
    uint32_t w;
    uint32_t h;
    uint32_t fmt;
    uint32_t pitch;
} Mini_TextureData;

typedef struct {
    SDL_bool is_init;
} Mini_RenderData;

#define MAX_TEXTURE 100

struct MY_TEXTURE {
    int pitch;
    const void *pixels;
    SDL_Texture *texture;
};

extern int FB_W;
extern int FB_H;
extern SDL_Window *vid_win;

/* Where the last copy put the window, the rect the app asked for and the picture, in
   panel coordinates; the pointer is drawn against them. */
static struct {
    int x;
    int y;
    int scale;
    SDL_Rect want;
    SDL_Rect clip;
} panel_map = { 0, 0, 0, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };

static struct MY_TEXTURE mytex[MAX_TEXTURE] = {0};

static int update_texture(void *chk, void *new, const void *pixels, int pitch)
{
    int cc = 0;

    for (cc = 0; cc < MAX_TEXTURE; cc++) {
        if (mytex[cc].texture == chk) {
            mytex[cc].texture = new;
            mytex[cc].pixels = pixels;
            mytex[cc].pitch = pitch;
            return cc;
        }
    }
    return -1;
}

const void* get_pixels(void *chk)
{
    int cc = 0;

    for (cc = 0; cc < MAX_TEXTURE; cc++) {
        if (mytex[cc].texture == chk) {
            return mytex[cc].pixels;
        }
    }
    return NULL;
}

static int get_pitch(void *chk)
{
    int cc = 0;

    for (cc = 0; cc < MAX_TEXTURE; cc++) {
        if (mytex[cc].texture == chk) {
            return mytex[cc].pitch;
        }
    }
    return -1;
}

static void *scale_buf = NULL;
static size_t scale_len = 0;
static int *scale_map = NULL;
static int scale_map_len = 0;

/* A quarter of each colour channel, alpha left alone: subtracted, it darkens a pixel. */
#define SHADE_8888  0x003f3f3f
#define SHADE_565   0x39e7

static int *edge_cols = NULL;
static int edge_cols_len = 0;

static void shade(uint8_t *px, int bpp)
{
    if (bpp == 4) {
        uint32_t *p = (uint32_t *)px;

        *p -= (*p >> 2) & SHADE_8888;
    }
    else {
        uint16_t *p = (uint16_t *)px;

        *p -= (uint16_t)((*p >> 2) & SHADE_565);
    }
}

/* Cells the source is divided into for the effects, so a frame the app already scaled up
   by an integer still gets one line per cell; 0 means one cell per source pixel. */
static int effect_cells(void)
{
    static int cells = -1;

    if (cells < 0) {
        const char *env = SDL_getenv(EFFECT_CELLS_ENV);

        cells = env ? SDL_atoi(env) : 0;
        if (cells < 0) {
            cells = 0;
        }
    }
    return cells;
}

/* The cell a source coordinate falls in, `len` being the source extent on that axis. */
static int effect_cell(int coord, int len, int cells)
{
    return cells ? ((coord * cells) / len) : coord;
}

/* Darkens the last panel row of every cell row, and for the grid its last column too.
   `map` is the source byte offset each destination column reads. */
static void apply_effect(uint8_t *out, int dw, int dh, int bpp, const SDL_Rect *src, const int *map)
{
    const int effect = Mini_EffectMode();
    const int cells = effect_cells();
    int cols = 0;
    int x = 0;
    int y = 0;
    int c0 = 0;

    if (effect == EFFECT_NONE) {
        return;
    }
    if (effect == EFFECT_GRID) {
        if (dw > edge_cols_len) {
            int *buf = (int *)SDL_realloc(edge_cols, (size_t)dw * sizeof(int));

            if (buf == NULL) {
                return;
            }
            edge_cols = buf;
            edge_cols_len = dw;
        }
        for (x = 0; x < dw; x++) {
            const int here = effect_cell((map[x] / bpp) - src->x, src->w, cells);
            const int next = (x == (dw - 1)) ? -1 : effect_cell((map[x + 1] / bpp) - src->x, src->w, cells);

            if (here != next) {
                edge_cols[cols++] = x;
            }
        }
    }

    for (y = 0; y < dh; y++) {
        uint8_t *row = out + ((size_t)y * dw * bpp);
        const int here = effect_cell((y * src->h) / dh, src->h, cells);
        const int next = (y == (dh - 1)) ? -1 : effect_cell(((y + 1) * src->h) / dh, src->h, cells);
        const int last_row = (here != next);

        if (last_row) {
            for (x = 0; x < dw; x++) {
                shade(row + ((size_t)x * bpp), bpp);
            }
            continue;
        }
        for (c0 = 0; c0 < cols; c0++) {
            shade(row + ((size_t)edge_cols[c0] * bpp), bpp);
        }
    }
}

static SDL_bool nearest_wanted(void)
{
    static int wanted = -1;

    if (wanted < 0) {
        const char *env = SDL_getenv("SDL_MINI_NEAREST");

        wanted = (env && (env[0] == '0')) ? 0 : 1;
    }
    return wanted ? SDL_TRUE : SDL_FALSE;
}

/* Resamples a copy nearest to its destination size, so MI_GFX, which filters every
   resize, has nothing left to scale. */
static const void *scale_nearest(SDL_Texture *texture, const void *pixels, const SDL_Rect *src, int dw, int dh, int *pitch)
{
    const int bpp = (texture->w > 0) ? (*pitch / texture->w) : 0;
    const uint8_t *in = (const uint8_t *)pixels;
    uint8_t *out = NULL;
    size_t need = 0;
    int x = 0;
    int y = 0;

    if (((bpp != 2) && (bpp != 4)) || (dw <= 0) || (dh <= 0) || (src->w <= 0) || (src->h <= 0)) {
        return NULL;
    }

    /* GFX_Copy stages the source in a panel-sized buffer. */
    if ((dw > FB_W) || (dh > FB_H)) {
        return NULL;
    }

    need = (size_t)dw * dh * bpp;
    if (need > scale_len) {
        void *buf = SDL_realloc(scale_buf, need);

        if (buf == NULL) {
            return NULL;
        }
        scale_buf = buf;
        scale_len = need;
    }

    if (dw > scale_map_len) {
        int *map = (int *)SDL_realloc(scale_map, (size_t)dw * sizeof(int));

        if (map == NULL) {
            return NULL;
        }
        scale_map = map;
        scale_map_len = dw;
    }

    /* The column each destination column reads, once per frame rather than per pixel. */
    for (x = 0; x < dw; x++) {
        scale_map[x] = (src->x + ((x * src->w) / dw)) * bpp;
    }

    out = (uint8_t *)scale_buf;
    for (y = 0; y < dh; y++) {
        const uint8_t *row = in + (size_t)(src->y + ((y * src->h) / dh)) * *pitch;
        uint8_t *dstrow = out + ((size_t)y * dw * bpp);

        if (bpp == 4) {
            for (x = 0; x < dw; x++) {
                ((uint32_t *)dstrow)[x] = *(const uint32_t *)(row + scale_map[x]);
            }
        }
        else {
            for (x = 0; x < dw; x++) {
                ((uint16_t *)dstrow)[x] = *(const uint16_t *)(row + scale_map[x]);
            }
        }
    }

    apply_effect(out, dw, dh, bpp, src, scale_map);
    *pitch = dw * bpp;
    return scale_buf;
}

/* Where a copy of `src` asked for at `want` lands in the scaling mode, within `area`,
   the window on the panel. */
static SDL_Rect scaled_rect(const SDL_Rect *want, const SDL_Rect *src, const SDL_Rect *area)
{
    SDL_Rect out = *want;
    int k = 0;

    switch (Mini_ScaleMode()) {
    case SCALE_FIT:
        break;
    case SCALE_INTEGER:
        if ((src->w > 0) && (src->h > 0)) {
            k = SDL_min(area->w / src->w, area->h / src->h);
        }
        if (k > 0) {
            out.w = src->w * k;
            out.h = src->h * k;
            out.x = area->x + ((area->w - out.w) / 2);
            out.y = area->y + ((area->h - out.h) / 2);
        }
        break;
    case SCALE_STRETCH:
        out = *area;
        break;
    }
    return out;
}

static void Mini_WindowEvent(SDL_Renderer *renderer, const SDL_WindowEvent *event)
{
    debug("%s\n", __func__);
}

static int Mini_CreateTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    Mini_TextureData *t = (Mini_TextureData *)SDL_calloc(1, sizeof(Mini_TextureData));

    debug("%s, texture=%p\n", __func__, texture);
    if (!t) {
        debug("%s, failed to create texture\n", __func__);
        return SDL_OutOfMemory();
    }

    t->w = texture->w;
    t->h = texture->h;
    t->fmt = texture->format;
    t->pitch = t->w * SDL_BYTESPERPIXEL(t->fmt);
    t->data = SDL_calloc(1, t->h * t->pitch);
    if (!t->data) {
        debug("%s, failed to create texture data\n", __func__);
        SDL_free(t);
        return SDL_OutOfMemory();
    }

    texture->driverdata = t;
    update_texture(NULL, texture, NULL, 0);
    return 0;
}

static int Mini_LockTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, void **pixels, int *pitch)
{
    Mini_TextureData *t = (Mini_TextureData *)texture->driverdata;

    debug("%s\n", __func__);
    *pixels = t->data;
    *pitch = t->pitch;
    return 0;
}

static int Mini_UpdateTexture(SDL_Renderer *renderer, SDL_Texture *texture, const SDL_Rect *rect, const void *pixels, int pitch)
{
    debug("%s, texture=%p, pixels=%p\n", __func__, texture, pixels);
    update_texture(texture, texture, pixels, pitch);
    return 0;
}

static void Mini_UnlockTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    SDL_Rect rect = {0};
    Mini_TextureData *t = (Mini_TextureData *)texture->driverdata;

    debug("%s\n", __func__);
    rect.x = 0;
    rect.y = 0;
    rect.w = texture->w;
    rect.h = texture->h;
    Mini_UpdateTexture(renderer, texture, &rect, t->data, t->pitch);
}

static void Mini_SetTextureScaleMode(SDL_Renderer *renderer, SDL_Texture *texture, SDL_ScaleMode scaleMode)
{
    debug("%s\n", __func__);
}

static int Mini_SetRenderTarget(SDL_Renderer *renderer, SDL_Texture *texture)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_QueueSetViewport(SDL_Renderer *renderer, SDL_RenderCommand *cmd)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_QueueDrawPoints(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FPoint *points, int count)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_QueueGeometry(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
    const float *xy, int xy_stride, const SDL_Color *color, int color_stride, const float *uv, int uv_stride,
    int num_vertices, const void *indices, int num_indices, int size_indices,
    float scale_x, float scale_y)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_QueueFillRects(SDL_Renderer *renderer, SDL_RenderCommand *cmd, const SDL_FRect *rects, int count)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_QueueCopy(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture, const SDL_Rect *srcrect, const SDL_FRect *dstrect)
{
    int pitch = 0;
    const void *pixels = get_pixels(texture);
    SDL_Rect dst = { 0 };
    SDL_Rect src = {srcrect->x, srcrect->y, srcrect->w, srcrect->h};
    SDL_Rect area = { 0 };

    int c0 = FB_W / vid_win->w;
    int c1 = FB_H / vid_win->h;
    float scale = c0 > c1 ? c1 : c0;

    panel_map.x = (FB_W - (vid_win->w * scale)) / 2;
    panel_map.y = (FB_H - (vid_win->h * scale)) / 2;
    panel_map.scale = (int)scale;
    panel_map.want.x = panel_map.x + (dstrect->x * scale);
    panel_map.want.y = panel_map.y + (dstrect->y * scale);
    panel_map.want.w = dstrect->w * scale;
    panel_map.want.h = dstrect->h * scale;
    area.x = panel_map.x;
    area.y = panel_map.y;
    area.w = vid_win->w * scale;
    area.h = vid_win->h * scale;
    panel_map.clip = scaled_rect(&panel_map.want, &src, &area);

    /* The panel is mounted upside down: the blit rotates, so the rect is mirrored on
       both axes. */
    dst.w = panel_map.clip.w;
    dst.h = panel_map.clip.h;
    dst.x = FB_W - (panel_map.clip.x + panel_map.clip.w);
    dst.y = FB_H - (panel_map.clip.y + panel_map.clip.h);

    pitch = get_pitch(texture);
    if ((pitch == 0) || (pixels == NULL)) {
        debug("%s, failed to get pitch or pixels (%d, %p)\n", __func__, pitch, pixels);
        return 0;
    }

    /* A 1:1 copy still goes through the scaler when an effect is on, since that is where
       the effect is drawn. */
    if (nearest_wanted() &&
        ((dst.w != src.w) || (dst.h != src.h) || (Mini_EffectMode() != EFFECT_NONE))) {
        const void *scaled = scale_nearest(texture, pixels, &src, dst.w, dst.h, &pitch);

        if (scaled != NULL) {
            pixels = scaled;
            src.x = 0;
            src.y = 0;
            src.w = dst.w;
            src.h = dst.h;
        }
    }

    debug("%s, texture=%p, src:%d,%d,%d,%d, dst:%d,%d,%d,%d, scale=%.2f, pitch=%d, pixels=%p\n", 
        __func__, texture, src.x, src.y, src.w, src.h, dst.x, dst.y, dst.w, dst.h, scale, pitch, pixels);
    GFX_Copy(pixels, src, dst, pitch, 0, E_MI_GFX_ROTATE_180);
    return 0;
}

static int Mini_QueueCopyEx(SDL_Renderer *renderer, SDL_RenderCommand *cmd, SDL_Texture *texture,
    const SDL_Rect *srcrect, const SDL_FRect *dstrect, const double angle, const SDL_FPoint *center, const SDL_RendererFlip flip)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_RunCommandQueue(SDL_Renderer *renderer, SDL_RenderCommand *cmd, void *vertices, size_t vertsize)
{
    debug("%s\n", __func__);
    return 0;
}

static int Mini_RenderReadPixels(SDL_Renderer *renderer, const SDL_Rect *rect, Uint32 pixel_format, void *pixels, int pitch)
{
    debug("%s\n", __func__);
    return SDL_Unsupported();
}

static void Mini_RenderPresent(SDL_Renderer *renderer)
{
    int mx = 0;
    int my = 0;
    int mag = Mini_PointerAt(&mx, &my);

    debug("%s\n", __func__);
    /* Over the picture, at the spot matching the app's pointer, and only inside it: the
       app redraws that much every frame, which erases the arrow. */
    if ((mag > 0) && (panel_map.scale > 0) && (panel_map.want.w > 0) && (panel_map.want.h > 0)) {
        int px = panel_map.x + (mx * panel_map.scale) - panel_map.want.x;
        int py = panel_map.y + (my * panel_map.scale) - panel_map.want.y;

        Mini_DrawPointer(panel_map.clip.x + ((px * panel_map.clip.w) / panel_map.want.w),
            panel_map.clip.y + ((py * panel_map.clip.h) / panel_map.want.h),
            mag * panel_map.scale, &panel_map.clip);
    }
    GFX_Flip();
}

static void Mini_DestroyTexture(SDL_Renderer *renderer, SDL_Texture *texture)
{
    Mini_TextureData *t = (Mini_TextureData *)texture->driverdata;

    debug("%s\n", __func__);
    if (t) {
        update_texture(texture, NULL, NULL, 0);
        if (t->data) {
            SDL_free(t->data);
        }
        SDL_free(t);
        texture->driverdata = NULL;
    }
}

static void Mini_DestroyRenderer(SDL_Renderer *renderer)
{
    Mini_RenderData *data = (Mini_RenderData *)renderer->driverdata;

    debug("%s\n", __func__);
    if (data) {
        if (!data->is_init) {
            return;
        }

        data->is_init = SDL_FALSE;
        SDL_free(data);
    }
    SDL_free(renderer);
}

static int Mini_SetVSync(SDL_Renderer *renderer, const int vsync)
{
    debug("%s\n", __func__);
    return 0;
}

SDL_Renderer *Mini_CreateRenderer(SDL_Window *window, Uint32 flags)
{
    SDL_Renderer *renderer = NULL;
    Mini_RenderData *data = NULL;

    debug("%s\n", __func__);
    renderer = (SDL_Renderer *) SDL_calloc(1, sizeof(SDL_Renderer));
    if (!renderer) {
        debug("%s, failed to create render\n", __func__);
        SDL_OutOfMemory();
        return NULL;
    }

    data = (Mini_RenderData *) SDL_calloc(1, sizeof(Mini_RenderData));
    if (!data) {
        debug("%s, failed to create render data\n", __func__);
        Mini_DestroyRenderer(renderer);
        SDL_OutOfMemory();
        return NULL;
    }

    renderer->WindowEvent = Mini_WindowEvent;
    renderer->CreateTexture = Mini_CreateTexture;
    renderer->UpdateTexture = Mini_UpdateTexture;
    renderer->LockTexture = Mini_LockTexture;
    renderer->UnlockTexture = Mini_UnlockTexture;
    renderer->SetTextureScaleMode = Mini_SetTextureScaleMode;
    renderer->SetRenderTarget = Mini_SetRenderTarget;
    renderer->QueueSetViewport = Mini_QueueSetViewport;
    renderer->QueueSetDrawColor = Mini_QueueSetViewport;
    renderer->QueueDrawPoints = Mini_QueueDrawPoints;
    renderer->QueueDrawLines = Mini_QueueDrawPoints;
    renderer->QueueGeometry = Mini_QueueGeometry;
    renderer->QueueFillRects = Mini_QueueFillRects;
    renderer->QueueCopy = Mini_QueueCopy;
    renderer->QueueCopyEx = Mini_QueueCopyEx;
    renderer->RunCommandQueue = Mini_RunCommandQueue;
    renderer->RenderReadPixels = Mini_RenderReadPixels;
    renderer->RenderPresent = Mini_RenderPresent;
    renderer->DestroyTexture = Mini_DestroyTexture;
    renderer->DestroyRenderer = Mini_DestroyRenderer;
    renderer->SetVSync = Mini_SetVSync;
    renderer->info = Mini_RenderDriver.info;
    renderer->info.flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
    /* From the panel at runtime: a window that fills it needs a texture that size. */
    renderer->info.max_texture_width = FB_W;
    renderer->info.max_texture_height = FB_H;
    renderer->driverdata = data;
    renderer->window = window;

    if(data->is_init != SDL_FALSE) {
        return 0;
    }
    data->is_init = SDL_TRUE;
    return renderer;
}

SDL_RenderDriver Mini_RenderDriver = {
    .CreateRenderer = Mini_CreateRenderer,
    .info = {
        .name = "Miyoo Mini",
        .flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE,
        .num_texture_formats = 2,
        .texture_formats = {
            [0] = SDL_PIXELFORMAT_RGB565,
            [1] = SDL_PIXELFORMAT_ARGB8888,
        },
        .max_texture_width = 640,
        .max_texture_height = 480,
    }
};

#endif

