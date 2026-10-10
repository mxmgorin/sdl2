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
   panel coordinates. */
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

/* Weights are in 32nds: the widest that keeps every 8888 channel product inside 16 bits. */
#define MIX_SHIFT   5
#define MIX_ONE     (1 << MIX_SHIFT)

/* A quarter of each colour channel, alpha left alone: subtracted, it darkens a pixel
   without unpacking it. */
#define SHADE_8888  0x003f3f3f
#define SHADE_565   0x39e7

/* Per-channel gains are 64ths, a byte each packed R, G, B from the bottom, so a gain can
   brighten as well as darken. */
#define GAIN_SHIFT  6
#define GAIN_ONE    (1 << GAIN_SHIFT)
#define GAIN_BITS   8
#define GAIN_MASK   ((1 << GAIN_BITS) - 1)
#define CHANNELS    3

#define PACK_GAIN(r, g, b) ((uint32_t)(r) | ((uint32_t)(g) << GAIN_BITS) | ((uint32_t)(b) << (2 * GAIN_BITS)))

/* Pixel layouts with a gain loop of their own, channel positions known at compile time. */
enum {
    LAYOUT_ANY,
    LAYOUT_8888,
    LAYOUT_565
};

/* Where each colour channel sits in a pixel, so a gain works on any format. */
typedef struct
{
    int shift[CHANNELS];
    uint32_t max[CHANNELS];
    /* Takes a channel to 8 bits in 256ths. */
    uint32_t to8[CHANNELS];
    uint32_t keep;
    int layout;
} Channels;

/* RetroArch's LCD3x light: a cell is 3x3 subcells, each lit in the channels listed. */
#define LCD_SUBCELLS 3

enum {
    LIT_R = 1,
    LIT_G = 2,
    LIT_B = 4,
    LIT_RGB = LIT_R | LIT_G | LIT_B
};

/* What a subcell keeps of the channels it does not light. */
#define LCD_UNLIT (GAIN_ONE / 5)

static const uint8_t lcd_light[LCD_SUBCELLS][LCD_SUBCELLS] = {
    { LIT_R, LIT_G, LIT_RGB },
    { LIT_R, LIT_G, LIT_B },
    { LIT_RGB, LIT_RGB, LIT_B }
};

/* A beam's brightness down a cell is kept in a table this fine, and averaged over this
   many points of a panel row. */
#define BEAM_RES  64
#define BEAM_TAPS 8

/* Scanline and CRT masks alternate magenta and green per panel column. */
#define MASK_PERIOD 2

/* Indexed with `x & (MASK_PERIOD - 1)`. */
SDL_COMPILE_TIME_ASSERT(mask_period_pow2, (MASK_PERIOD & (MASK_PERIOD - 1)) == 0);

/* Scanlines: a beam brightest mid-cell, through the mask, with a gain making up for both. */
#define SCAN_BEAM_FLOOR 0.35f
#define SCAN_BEAM_SIGMA 0.28f
#define SCAN_MASK_DIM   0.6f
#define SCAN_GAIN       1.3f

/* CRT: a beam that widens with the pixel's luma and falls deeper between rows, and a
   cell that bleeds into the next one over its last part. */
#define CRT_BEAM_FLOOR  0.1f
#define CRT_SIGMA_DARK  0.18f
#define CRT_SIGMA_LIT   0.38f
#define CRT_MASK_DIM    0.7f
#define CRT_GAIN        1.45f
#define CRT_BLEED_FROM  0.6f
#define CRT_BLEED_MAX   0.5f
#define LUMA_BITS       4
#define LUMA_LEVELS     (1 << LUMA_BITS)

/* Rec. 601 luma in 256ths. */
#define LUMA_R 77
#define LUMA_G 150
#define LUMA_B 29

/* One destination column or row of a resample, and where it sits in the effect cells. */
typedef struct
{
    /* The source it reads: a byte offset for columns, a row for rows. */
    int at;
    /* How much of it lies on the last panel pixel's worth of its cell. */
    int edge;
    /* The LCD subcell it starts in, and how much of it lies on the next one. */
    int subcell;
    int next;
    /* The span it covers, in 16.16 cell units. */
    int from;
    int to;
    /* The next cell's source, as `at`, and how much of it bleeds in. */
    int bleed_at;
    int bleed;
} ScaleStep;

static ScaleStep *step_x = NULL;
static ScaleStep *step_y = NULL;
static int step_x_len = 0;
static int step_y_len = 0;

static uint32_t mix_8888(uint32_t a, uint32_t b, int w)
{
    const uint32_t lo = ((((a & 0x00ff00ff) * (MIX_ONE - w)) + ((b & 0x00ff00ff) * w)) >> MIX_SHIFT) & 0x00ff00ff;
    const uint32_t hi = (((((a >> 8) & 0x00ff00ff) * (MIX_ONE - w)) + (((b >> 8) & 0x00ff00ff) * w)) >> MIX_SHIFT) & 0x00ff00ff;

    return lo | (hi << 8);
}

static uint16_t mix_565(uint16_t a, uint16_t b, int w)
{
    /* Spreads G into the upper half, so each field has room for the product. */
    const uint32_t wa = ((uint32_t)a | ((uint32_t)a << 16)) & 0x07e0f81f;
    const uint32_t wb = ((uint32_t)b | ((uint32_t)b << 16)) & 0x07e0f81f;
    const uint32_t m = (((wa * (MIX_ONE - w)) + (wb * w)) >> MIX_SHIFT) & 0x07e0f81f;

    return (uint16_t)(m | (m >> 16));
}

static uint32_t mix_px(uint32_t a, uint32_t b, int w, int bpp)
{
    if (w == 0) {
        return a;
    }
    return (bpp == 4) ? mix_8888(a, b, w) : mix_565((uint16_t)a, (uint16_t)b, w);
}

static uint32_t read_px(const uint8_t *p, int bpp)
{
    return (bpp == 4) ? *(const uint32_t *)p : *(const uint16_t *)p;
}

static void write_px(uint8_t *p, uint32_t v, int bpp)
{
    if (bpp == 4) {
        *(uint32_t *)p = v;
    }
    else {
        *(uint16_t *)p = (uint16_t)v;
    }
}

static uint32_t shaded(uint32_t p, int bpp)
{
    return p - ((p >> 2) & ((bpp == 4) ? SHADE_8888 : SHADE_565));
}

/* Darkens a pixel by a quarter, scaled by `w`, the part of it that lies on the line. */
static uint32_t shade_by(uint32_t p, int w, int bpp)
{
    return (w == 0) ? p : mix_px(p, shaded(p, bpp), w, bpp);
}

static void channels_from_masks(Channels *ch, const uint32_t *mask)
{
    int c = 0;

    ch->keep = ~(mask[0] | mask[1] | mask[2]);
    for (c = 0; c < CHANNELS; c++) {
        ch->shift[c] = mask[c] ? __builtin_ctz(mask[c]) : 0;
        ch->max[c] = mask[c] >> ch->shift[c];
        ch->to8[c] = ch->max[c] ? ((0xff << 8) / ch->max[c]) : 0;
    }
    if ((mask[0] == 0x00ff0000) && (mask[1] == 0x0000ff00) && (mask[2] == 0x000000ff)) {
        ch->layout = LAYOUT_8888;
    }
    else if ((mask[0] == 0xf800) && (mask[1] == 0x07e0) && (mask[2] == 0x001f)) {
        ch->layout = LAYOUT_565;
    }
    else {
        ch->layout = LAYOUT_ANY;
    }
}

/* Scales one channel by its packed gain, saturating. */
SDL_FORCE_INLINE uint32_t gain_channel(uint32_t p, uint32_t gain, int c, int shift, uint32_t max)
{
    const uint32_t v = (((p >> shift) & max) * ((gain >> (c * GAIN_BITS)) & GAIN_MASK)) >> GAIN_SHIFT;

    return SDL_min(v, max) << shift;
}

/* Scales each colour channel by its packed gain, saturating; alpha is kept. */
SDL_FORCE_INLINE uint32_t gain_px(uint32_t p, uint32_t gain, const Channels *ch, int layout)
{
    if (layout == LAYOUT_8888) {
        return (p & ch->keep) | gain_channel(p, gain, 0, 16, 0xff) | gain_channel(p, gain, 1, 8, 0xff) | gain_channel(p, gain, 2, 0, 0xff);
    }
    if (layout == LAYOUT_565) {
        return gain_channel(p, gain, 0, 11, 0x1f) | gain_channel(p, gain, 1, 5, 0x3f) | gain_channel(p, gain, 2, 0, 0x1f);
    }
    return (p & ch->keep) | gain_channel(p, gain, 0, ch->shift[0], ch->max[0]) | gain_channel(p, gain, 1, ch->shift[1], ch->max[1]) | gain_channel(p, gain, 2, ch->shift[2], ch->max[2]);
}

/* Resamples a source row into `dst` with every pixel scaled by its gain, `gains[x & period]`;
   `layout` and `bpp` are constants wherever this is inlined, so each gets its own loop. */
SDL_FORCE_INLINE void gain_row_as(uint8_t *dst, const uint8_t *src, int dw, const uint32_t *gains, uint32_t period, const Channels *ch, int layout, int bpp)
{
    int x = 0;

    for (x = 0; x < dw; x++) {
        write_px(dst + ((size_t)x * bpp), gain_px(read_px(src + step_x[x].at, bpp), gains[x & period], ch, layout), bpp);
    }
}

static void gain_row(uint8_t *dst, const uint8_t *src, int dw, int bpp, const uint32_t *gains, uint32_t period, const Channels *ch)
{
    if ((ch->layout == LAYOUT_8888) && (bpp == 4)) {
        gain_row_as(dst, src, dw, gains, period, ch, LAYOUT_8888, 4);
    }
    else if ((ch->layout == LAYOUT_565) && (bpp == 2)) {
        gain_row_as(dst, src, dw, gains, period, ch, LAYOUT_565, 2);
    }
    else {
        gain_row_as(dst, src, dw, gains, period, ch, LAYOUT_ANY, bpp);
    }
}

/* Fills `table` with a beam's brightness down a cell, in GAIN_ONE units. */
static void beam_fill(int *table, float floor, float sigma)
{
    int i = 0;

    for (i = 0; i < BEAM_RES; i++) {
        const float d = (((float)i + 0.5f) / BEAM_RES) - 0.5f;
        const float b = floor + ((1.0f - floor) * SDL_expf(-(d * d) / (2.0f * sigma * sigma)));

        table[i] = (int)(b * GAIN_ONE);
    }
}

static const int *scan_beam(void)
{
    static int table[BEAM_RES];
    static SDL_bool ready = SDL_FALSE;

    if (!ready) {
        beam_fill(table, SCAN_BEAM_FLOOR, SCAN_BEAM_SIGMA);
        ready = SDL_TRUE;
    }
    return table;
}

/* Returns the CRT beam of each luma level, built on first use. */
static const int (*crt_beam(void))[BEAM_RES]
{
    static int table[LUMA_LEVELS][BEAM_RES];
    static SDL_bool ready = SDL_FALSE;

    if (!ready) {
        int l = 0;

        for (l = 0; l < LUMA_LEVELS; l++) {
            const float luma = ((float)l + 0.5f) / LUMA_LEVELS;

            beam_fill(table[l], CRT_BEAM_FLOOR, CRT_SIGMA_DARK + ((CRT_SIGMA_LIT - CRT_SIGMA_DARK) * luma));
        }
        ready = SDL_TRUE;
    }
    return (const int (*)[BEAM_RES])table;
}

/* Averages a beam over [s, e), in 16.16 cell units. */
static int beam_over(const int *table, int s, int e)
{
    int sum = 0;
    int k = 0;

    for (k = 0; k < BEAM_TAPS; k++) {
        const int at = s + (((2 * k + 1) * (e - s)) / (2 * BEAM_TAPS));

        sum += table[((at & 0xffff) * BEAM_RES) >> 16];
    }
    return sum / BEAM_TAPS;
}

/* Returns how many cells the source is divided into for the effects, so a frame the app
   already scaled up by an integer still gets one line per cell; 0 means one per pixel. */
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

/* Lays out one axis of `len` source pixels from `off` over `dst` panel pixels, in cells of
   `units` source pixels' worth. Pixels are nearest, so cells differ by a pixel at a
   fractional scale; the effect lines are placed by area instead, so they stay even. */
static void layout_axis(ScaleStep *steps, int off, int len, int dst, int units, int scale)
{
    /* One panel pixel in 16.16 cell units, so lines stay one pixel wide at any scale. */
    const int64_t px = ((int64_t)units << 16) / dst;
    int i = 0;

    for (i = 0; i < dst; i++) {
        const int64_t s = ((int64_t)i * units << 16) / dst;
        const int64_t e = ((int64_t)(i + 1) * units << 16) / dst;
        const int64_t bound = ((s >> 16) + 1) << 16;
        /* The same span in 16.16 subcell units. */
        const int64_t ss = s * LCD_SUBCELLS;
        const int64_t se = e * LCD_SUBCELLS;
        const int64_t sub_bound = ((ss >> 16) + 1) << 16;
        ScaleStep *st = &steps[i];

        st->at = (off + ((i * len) / dst)) * scale;
        /* The part of this pixel over the line closing the cell it starts in. */
        st->edge = (int)(((SDL_min(e, bound) - SDL_max(s, bound - px)) * MIX_ONE) / px);
        st->edge = SDL_max(st->edge, 0);

        /* A pixel wider than a subcell, in cells under 3 px, counts only the next one. */
        st->subcell = (int)((ss >> 16) % LCD_SUBCELLS);
        st->next = (se > sub_bound) ? (int)(((SDL_min(se, sub_bound + 0x10000) - sub_bound) * MIX_ONE) / (se - ss)) : 0;

        st->from = (int)s;
        st->to = (int)e;
        {
            const int64_t mid = ((s + e) >> 1) & 0xffff;
            const int64_t start = (int64_t)(CRT_BLEED_FROM * 0x10000);
            const int next_src = (int)SDL_min((((s >> 16) + 1) * len) / units, len - 1);

            st->bleed_at = (off + next_src) * scale;
            st->bleed = (mid > start) ? (int)(((mid - start) * (int64_t)(CRT_BLEED_MAX * MIX_ONE)) / (0x10000 - start)) : 0;
        }
    }
}

/* A row's LCD gains depend only on the subcell it starts in and how much of the next it
   covers, so each kind is worked out once per column layout and kept. */
#define LCD_ROW_KINDS (LCD_SUBCELLS * (MIX_ONE + 1))

static uint32_t *lcd_gains = NULL;
static size_t lcd_gains_len = 0;
static SDL_bool lcd_known[LCD_ROW_KINDS];
static int lcd_layout[4] = { 0 };

/* Forgets the kept rows when the columns are laid out differently. */
static int lcd_prepare(int dw, int off, int len, int units)
{
    const int layout[4] = { dw, off, len, units };
    const size_t need = (size_t)LCD_ROW_KINDS * dw;

    if (lcd_gains && (SDL_memcmp(layout, lcd_layout, sizeof(layout)) == 0)) {
        return 0;
    }
    if (need > lcd_gains_len) {
        uint32_t *buf = (uint32_t *)SDL_realloc(lcd_gains, need * sizeof(uint32_t));

        if (buf == NULL) {
            return -1;
        }
        lcd_gains = buf;
        lcd_gains_len = need;
    }
    SDL_memset(lcd_known, 0, sizeof(lcd_known));
    SDL_memcpy(lcd_layout, layout, sizeof(layout));
    return 0;
}

/* Returns a row kind's packed gains, each pixel weighted by the subcell area it covers. */
static const uint32_t *lcd_row_gains(int dw, const ScaleStep *sy)
{
    const int kind = (sy->subcell * (MIX_ONE + 1)) + sy->next;
    uint32_t *gains = lcd_gains + ((size_t)kind * dw);
    const int r0 = sy->subcell;
    const int r1 = (r0 + 1) % LCD_SUBCELLS;
    /* Each channel's gain down each subcell column over this row, in GAIN_ONE * MIX_ONE. */
    int lit[LCD_SUBCELLS][CHANNELS];
    int col = 0;
    int c = 0;
    int x = 0;

    if (lcd_known[kind]) {
        return gains;
    }
    for (col = 0; col < LCD_SUBCELLS; col++) {
        for (c = 0; c < CHANNELS; c++) {
            const int g0 = ((lcd_light[r0][col] >> c) & 1) ? GAIN_ONE : LCD_UNLIT;
            const int g1 = ((lcd_light[r1][col] >> c) & 1) ? GAIN_ONE : LCD_UNLIT;

            lit[col][c] = (g0 * (MIX_ONE - sy->next)) + (g1 * sy->next);
        }
    }
    for (x = 0; x < dw; x++) {
        const ScaleStep *sx = &step_x[x];
        const int *a = lit[sx->subcell];
        const int *b = lit[(sx->subcell + 1) % LCD_SUBCELLS];
        int g[CHANNELS];

        for (c = 0; c < CHANNELS; c++) {
            g[c] = ((a[c] * (MIX_ONE - sx->next)) + (b[c] * sx->next)) >> (2 * MIX_SHIFT);
        }
        gains[x] = PACK_GAIN(g[0], g[1], g[2]);
    }
    lcd_known[kind] = SDL_TRUE;
    return gains;
}

/* Every column, so `gains[x & GAINS_PER_COLUMN]` is `gains[x]`. */
#define GAINS_PER_COLUMN 0xffffffffu

/* Draws a row through the scanline beam and the mask. */
static void scan_row(uint8_t *dst, const uint8_t *src, int dw, int bpp, const ScaleStep *sy, const Channels *ch)
{
    const int bright = (int)((float)beam_over(scan_beam(), sy->from, sy->to) * SCAN_GAIN);
    const int dim = (int)((float)bright * SCAN_MASK_DIM);
    const uint32_t mask[MASK_PERIOD] = {
        PACK_GAIN(bright, dim, bright),
        PACK_GAIN(dim, bright, dim)
    };

    gain_row(dst, src, dw, bpp, mask, MASK_PERIOD - 1, ch);
}

/* Quantises a pixel's luma to LUMA_LEVELS steps. */
SDL_FORCE_INLINE int luma_level(uint32_t p, const Channels *ch, int layout)
{
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;

    if (layout == LAYOUT_8888) {
        r = (p >> 16) & 0xff;
        g = (p >> 8) & 0xff;
        b = p & 0xff;
    }
    else if (layout == LAYOUT_565) {
        r = ((p >> 11) & 0x1f) << 3;
        g = ((p >> 5) & 0x3f) << 2;
        b = (p & 0x1f) << 3;
    }
    else {
        r = (((p >> ch->shift[0]) & ch->max[0]) * ch->to8[0]) >> 8;
        g = (((p >> ch->shift[1]) & ch->max[1]) * ch->to8[1]) >> 8;
        b = (((p >> ch->shift[2]) & ch->max[2]) * ch->to8[2]) >> 8;
    }
    return (int)(((LUMA_R * r) + (LUMA_G * g) + (LUMA_B * b)) >> (16 - LUMA_BITS));
}

/* A CRT row's bled pixels and their luma depend only on the source row, so they are
   worked out once for the run of panel rows that share it. */
static uint32_t *crt_px = NULL;
static uint8_t *crt_luma = NULL;
static int crt_len = 0;
static const uint8_t *crt_src = NULL;

/* Readies a frame of CRT rows; nothing from the last frame is kept. */
static int crt_prepare(int dw)
{
    if (dw > crt_len) {
        uint32_t *px = (uint32_t *)SDL_realloc(crt_px, (size_t)dw * sizeof(uint32_t));
        uint8_t *luma = NULL;

        if (px == NULL) {
            return -1;
        }
        crt_px = px;
        luma = (uint8_t *)SDL_realloc(crt_luma, (size_t)dw);
        if (luma == NULL) {
            return -1;
        }
        crt_luma = luma;
        crt_len = dw;
    }
    crt_src = NULL;
    return 0;
}

SDL_FORCE_INLINE void crt_bleed_as(const uint8_t *src, int dw, const Channels *ch, int layout, int bpp)
{
    int x = 0;

    for (x = 0; x < dw; x++) {
        const ScaleStep *sx = &step_x[x];
        const uint32_t p = mix_px(read_px(src + sx->at, bpp), read_px(src + sx->bleed_at, bpp), sx->bleed, bpp);

        crt_px[x] = p;
        crt_luma[x] = (uint8_t)luma_level(p, ch, layout);
    }
}

SDL_FORCE_INLINE void crt_row_as(uint8_t *dst, const uint8_t *src, int dw, const uint32_t (*gains)[MASK_PERIOD], const Channels *ch, int layout, int bpp)
{
    int x = 0;

    if (src != crt_src) {
        crt_bleed_as(src, dw, ch, layout, bpp);
        crt_src = src;
    }
    for (x = 0; x < dw; x++) {
        write_px(dst + ((size_t)x * bpp), gain_px(crt_px[x], gains[crt_luma[x]][x & (MASK_PERIOD - 1)], ch, layout), bpp);
    }
}

/* Draws a row through the CRT beam of each pixel's luma and the mask. */
static void crt_row(uint8_t *dst, const uint8_t *src, int dw, int bpp, const ScaleStep *sy, const Channels *ch)
{
    const int (*beams)[BEAM_RES] = crt_beam();
    uint32_t gains[LUMA_LEVELS][MASK_PERIOD];
    int l = 0;

    for (l = 0; l < LUMA_LEVELS; l++) {
        const int bright = (int)((float)beam_over(beams[l], sy->from, sy->to) * CRT_GAIN);
        const int dim = (int)((float)bright * CRT_MASK_DIM);

        gains[l][0] = PACK_GAIN(bright, dim, bright);
        gains[l][1] = PACK_GAIN(dim, bright, dim);
    }
    if ((ch->layout == LAYOUT_8888) && (bpp == 4)) {
        crt_row_as(dst, src, dw, (const uint32_t (*)[MASK_PERIOD])gains, ch, LAYOUT_8888, 4);
    }
    else if ((ch->layout == LAYOUT_565) && (bpp == 2)) {
        crt_row_as(dst, src, dw, (const uint32_t (*)[MASK_PERIOD])gains, ch, LAYOUT_565, 2);
    }
    else {
        crt_row_as(dst, src, dw, (const uint32_t (*)[MASK_PERIOD])gains, ch, LAYOUT_ANY, bpp);
    }
}

static int ensure_steps(ScaleStep **steps, int *have, int want)
{
    if (want > *have) {
        ScaleStep *buf = (ScaleStep *)SDL_realloc(*steps, (size_t)want * sizeof(ScaleStep));

        if (buf == NULL) {
            return -1;
        }
        *steps = buf;
        *have = want;
    }
    return 0;
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
   resize, has nothing left to scale. The screen effect is drawn here too. */
static const void *scale_copy(SDL_Texture *texture, const void *pixels, const SDL_Rect *src, int dw, int dh, int *pitch)
{
    const int bpp = (texture->w > 0) ? (*pitch / texture->w) : 0;
    const int effect = Mini_EffectMode();
    const int cells = effect_cells();
    const uint8_t *in = (const uint8_t *)pixels;
    uint32_t chan[CHANNELS] = { 0 };
    Channels ch;
    uint32_t alpha = 0;
    uint8_t *out = NULL;
    size_t need = 0;
    int bits = 0;
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
    if ((ensure_steps(&step_x, &step_x_len, dw) < 0) || (ensure_steps(&step_y, &step_y_len, dh) < 0)) {
        return NULL;
    }
    if (!SDL_PixelFormatEnumToMasks(texture->format, &bits, &chan[0], &chan[1], &chan[2], &alpha)) {
        return NULL;
    }

    /* Laid out once per frame, shared by every pixel. */
    layout_axis(step_x, src->x, src->w, dw, cells ? cells : src->w, bpp);
    layout_axis(step_y, src->y, src->h, dh, cells ? cells : src->h, 1);
    channels_from_masks(&ch, chan);
    if ((effect == EFFECT_LCD) && (lcd_prepare(dw, src->x, src->w, cells ? cells : src->w) < 0)) {
        return NULL;
    }
    if ((effect == EFFECT_CRT) && (crt_prepare(dw) < 0)) {
        return NULL;
    }

    out = (uint8_t *)scale_buf;
    for (y = 0; y < dh; y++) {
        const ScaleStep *sy = &step_y[y];
        const uint8_t *row = in + ((size_t)sy->at * *pitch);
        uint8_t *dstrow = out + ((size_t)y * dw * bpp);

        /* These resample and draw in one pass. */
        if (effect == EFFECT_LCD) {
            gain_row(dstrow, row, dw, bpp, lcd_row_gains(dw, sy), GAINS_PER_COLUMN, &ch);
            continue;
        }
        if (effect == EFFECT_SCANLINES) {
            scan_row(dstrow, row, dw, bpp, sy, &ch);
            continue;
        }
        if (effect == EFFECT_CRT) {
            crt_row(dstrow, row, dw, bpp, sy, &ch);
            continue;
        }

        if (bpp == 4) {
            for (x = 0; x < dw; x++) {
                ((uint32_t *)dstrow)[x] = *(const uint32_t *)(row + step_x[x].at);
            }
        }
        else {
            for (x = 0; x < dw; x++) {
                ((uint16_t *)dstrow)[x] = *(const uint16_t *)(row + step_x[x].at);
            }
        }

        if (effect != EFFECT_GRID) {
            continue;
        }
        /* The grid leaves most pixels alone, so only its lines are touched. */
        if (sy->edge != 0) {
            for (x = 0; x < dw; x++) {
                uint8_t *px = dstrow + ((size_t)x * bpp);

                write_px(px, shade_by(read_px(px, bpp), SDL_max(sy->edge, step_x[x].edge), bpp), bpp);
            }
        }
        else {
            for (x = 0; x < dw; x++) {
                if (step_x[x].edge != 0) {
                    uint8_t *px = dstrow + ((size_t)x * bpp);

                    write_px(px, shade_by(read_px(px, bpp), step_x[x].edge, bpp), bpp);
                }
            }
        }
    }

    *pitch = dw * bpp;
    return scale_buf;
}

/* Places a copy of `src` asked for at `want` by the scaling mode, within `area`, the
   window on the panel; returns where it lands. */
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
        const void *scaled = scale_copy(texture, pixels, &src, dst.w, dst.h, &pitch);

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

