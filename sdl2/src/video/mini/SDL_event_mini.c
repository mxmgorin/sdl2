// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MINI

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <linux/input.h>

#include "../../events/SDL_events_c.h"
#include "../../core/linux/SDL_evdev.h"
#include "../../thread/SDL_systhread.h"

#include "SDL_timer.h"
#include "SDL_video_mini.h"
#include "SDL_event_mini.h"

static int running = 0;
static SDL_Thread *thread = NULL;

uint8_t mykey[KEY_MAX][2] = { 0 };
/* The flags above are edges the pump clears; a pointer needs to know what is down. */
static uint8_t myheld[KEY_MAX] = { 0 };
static SDL_Scancode mymap[KEY_MAX] = { 0 };

extern SDL_Window *vid_win;

typedef enum {
    MOUSE_OFF = 0,
    MOUSE_TOGGLE,
    MOUSE_HOLD
} MouseMode;

static MouseMode mouse_mode = MOUSE_OFF;
static int mouse_speed = MOUSE_SPEED_MAX;
static int mouse_icon = MOUSE_ICON_MAG;
static SDL_Rect mouse_rect = { 0, 0, 0, 0 };
static int mouse_placed = 0;
/* The toggle's own latch, and what the previous pump resolved it to. */
static int mouse_live = 0;
static int mouse_prev = 0;
static float mouse_x = 0.0f;
static float mouse_y = 0.0f;
static Uint32 mouse_ms = 0;
static Uint32 mouse_push_ms = 0;
static uint8_t mouse_btn[2] = { 0 };

static void mouse_init(void)
{
    const char *env = SDL_getenv(MOUSE_MODE_ENV);
    SDL_Rect rect = { 0, 0, 0, 0 };
    int speed = 0;

    if ((env == NULL) || (env[0] == '\0') ||
        !SDL_strcasecmp(env, "0") || !SDL_strcasecmp(env, "off")) {
        return;
    }
    mouse_mode = !SDL_strcasecmp(env, "hold") ? MOUSE_HOLD : MOUSE_TOGGLE;

    env = SDL_getenv(MOUSE_SPEED_ENV);
    if (env != NULL) {
        speed = SDL_atoi(env);
        if ((speed >= MOUSE_SPEED_MIN) && (speed <= MOUSE_SPEED_TOP)) {
            mouse_speed = speed;
        }
    }

    /* Outside the app's picture there is nowhere to put the pointer. */
    env = SDL_getenv(MOUSE_RECT_ENV);
    if ((env != NULL) &&
        (SDL_sscanf(env, "%d,%d,%d,%d", &rect.x, &rect.y, &rect.w, &rect.h) == 4)) {
        mouse_rect = rect;
    }

    env = SDL_getenv(MOUSE_ICON_ENV);
    if (env != NULL) {
        int mag = SDL_atoi(env);

        if ((mag >= 0) && (mag <= MOUSE_ICON_TOP)) {
            mouse_icon = mag;
        }
    }
    /* Logged rather than debugged, as only a device shows what the pad does. */
    SDL_Log("Mini: pointer on L2 (%s), %d px/s, arrow x%d, inside %d,%d,%d,%d",
        (mouse_mode == MOUSE_HOLD) ? "hold" : "toggle", mouse_speed, mouse_icon,
        mouse_rect.x, mouse_rect.y, mouse_rect.w, mouse_rect.h);
}

/* The keys the pointer takes while it is live; the mode key stays the mode's own. */
static int mouse_pad_key(int code)
{
    switch (code) {
    case KEY_UP:
    case KEY_DOWN:
    case KEY_LEFT:
    case KEY_RIGHT:
    case MOUSE_LEFT_KEY:
    case MOUSE_RIGHT_KEY:
        return 1;
    default:
        return 0;
    }
}

/* What the app was told about the pad has to be untold across a mode change, or a
   direction held as the pointer takes the keys stays down for good. */
static void mouse_switch(int live)
{
    int c0 = 0;

    for (c0 = 0; c0 < KEY_MAX; c0++) {
        if (myheld[c0] && mymap[c0] && mouse_pad_key(c0)) {
            SDL_SendKeyboardKey(live ? SDL_RELEASED : SDL_PRESSED,
                SDL_GetScancodeFromKey(mymap[c0]));
        }
    }
}

/* The window bounds the pointer, narrowed to whatever rect a port named. It starts in
   the middle of that once; a later mode switch leaves it where it was. */
static void mouse_place(void)
{
    SDL_Rect win = { 0, 0, vid_win->w, vid_win->h };
    SDL_Rect fit = { 0, 0, 0, 0 };

    if (!SDL_IntersectRect(&mouse_rect, &win, &fit)) {
        fit = win;
    }
    mouse_rect = fit;

    if (!mouse_placed) {
        mouse_placed = 1;
        mouse_x = mouse_rect.x + (mouse_rect.w / 2.0f);
        mouse_y = mouse_rect.y + (mouse_rect.h / 2.0f);
    }
    mouse_ms = SDL_GetTicks();
    mouse_push_ms = 0;
    SDL_SendMouseMotion(vid_win, 0, 0, (int)mouse_x, (int)mouse_y);
}

/* A tap that began and ended between two pumps is still a click: the press edge
   stands in for the held state it no longer has. */
static void mouse_buttons(int live)
{
    static const struct {
        int key;
        Uint8 button;
    } btn[] = {
        { MOUSE_LEFT_KEY, SDL_BUTTON_LEFT },
        { MOUSE_RIGHT_KEY, SDL_BUTTON_RIGHT }
    };
    int c0 = 0;
    uint8_t down = 0;

    for (c0 = 0; c0 < (int)SDL_arraysize(btn); c0++) {
        down = live && (myheld[btn[c0].key] || mykey[btn[c0].key][1]);
        if (down != mouse_btn[c0]) {
            mouse_btn[c0] = down;
            SDL_SendMouseButton(vid_win, 0, down ? SDL_PRESSED : SDL_RELEASED,
                btn[c0].button);
        }
    }
}

/* Moved by elapsed time, not per pump, so its speed does not depend on how often the
   app asks for events. */
static void mouse_motion(void)
{
    Uint32 now = SDL_GetTicks();
    Uint32 step_ms = now - mouse_ms;
    Uint32 ramp_ms = 0;
    int dx = myheld[KEY_RIGHT] - myheld[KEY_LEFT];
    int dy = myheld[KEY_DOWN] - myheld[KEY_UP];
    float step = 0.0f;

    mouse_ms = now;
    if ((dx == 0) && (dy == 0)) {
        mouse_push_ms = 0;
        return;
    }
    if (mouse_push_ms == 0) {
        mouse_push_ms = now;
    }
    if (step_ms > MOUSE_STEP_MAX_MS) {
        step_ms = MOUSE_STEP_MAX_MS;
    }

    ramp_ms = now - mouse_push_ms;
    if (ramp_ms > MOUSE_RAMP_MS) {
        ramp_ms = MOUSE_RAMP_MS;
    }
    step = MOUSE_SPEED_MIN +
        ((mouse_speed - MOUSE_SPEED_MIN) * ((float)ramp_ms / MOUSE_RAMP_MS));
    step = (step * step_ms) / 1000.0f;
    if ((dx != 0) && (dy != 0)) {
        step *= MOUSE_DIAGONAL;
    }

    mouse_x += dx * step;
    mouse_y += dy * step;
    if (mouse_x < mouse_rect.x) {
        mouse_x = mouse_rect.x;
    }
    if (mouse_y < mouse_rect.y) {
        mouse_y = mouse_rect.y;
    }
    if (mouse_x > (mouse_rect.x + mouse_rect.w - 1)) {
        mouse_x = mouse_rect.x + mouse_rect.w - 1;
    }
    if (mouse_y > (mouse_rect.y + mouse_rect.h - 1)) {
        mouse_y = mouse_rect.y + mouse_rect.h - 1;
    }
    SDL_SendMouseMotion(vid_win, 0, 0, (int)mouse_x, (int)mouse_y);
}

/* Whether the pointer holds the pad this pump. */
static int mouse_pump(void)
{
    int live = 0;

    if ((mouse_mode == MOUSE_OFF) || (vid_win == NULL)) {
        return 0;
    }

    if (mouse_mode == MOUSE_HOLD) {
        mouse_live = myheld[MOUSE_MODE_KEY];
    } else if (mykey[MOUSE_MODE_KEY][1]) {
        mouse_live = !mouse_live;
    }

    live = mouse_live;
    if (live != mouse_prev) {
        mouse_prev = live;
        mouse_switch(live);
        if (live) {
            mouse_place();
        }
    }
    mouse_buttons(live);
    if (live) {
        mouse_motion();
    }
    return live;
}

int Mini_PointerAt(int *x, int *y)
{
    if (!mouse_prev || (mouse_icon < 1)) {
        return 0;
    }
    *x = (int)mouse_x;
    *y = (int)mouse_y;
    return mouse_icon;
}

/* SELECT + LEFT/RIGHT walks the bezels and SELECT + R1 steps the scaling mode. A key is
   only swallowed if it did something, so a port without bezels or modes keeps it. */
static void select_hotkeys(void)
{
    if (!myheld[BEZEL_KEY_HOLD]) {
        return;
    }
    if (mykey[SCALE_KEY][1] && Mini_ScaleStep()) {
        mykey[SCALE_KEY][1] = 0;
    }
    if (mykey[BEZEL_KEY_PREV][1] && Mini_BezelStep(-1)) {
        mykey[BEZEL_KEY_PREV][1] = 0;
    }
    if (mykey[BEZEL_KEY_NEXT][1] && Mini_BezelStep(1)) {
        mykey[BEZEL_KEY_NEXT][1] = 0;
    }
}

int Mini_InputHandler(void *data)
{
    int fd = -1;
    struct input_event ev = {0};

    debug("%s++\n", __func__);
    fd = open("/dev/input/event0", O_RDONLY);
    if (fd < 0){
        debug("%s, failed to open input device\n", __func__);
        return 0;
    }

    running = 1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    while (running) {
        if (read(fd, &ev, sizeof(struct input_event)) > 0) {
            /* Value 2 is autorepeat and the pair has no third slot: writing it lands
               on the next code's release flag. */
            if ((ev.type == EV_KEY) && (ev.code < KEY_MAX) && (ev.value < 2)) {
                mykey[ev.code][ev.value] = 1;
                myheld[ev.code] = (ev.value != 0);
                debug("%s, code:%d, value:%d\n", __func__, ev.code, ev.value);
            }
        }
        usleep(1000);
    }
    
    if (fd > 0) {
        close(fd);
        fd = -1;
    }
    debug("%s--\n", __func__);
    return 0;
}

void Mini_EventInit(void)
{
    debug("%s\n", __func__);

     mymap[KEY_0] = SDLK_0;
     mymap[KEY_1] = SDLK_1;
     mymap[KEY_2] = SDLK_2;
     mymap[KEY_3] = SDLK_3;
     mymap[KEY_4] = SDLK_4;
     mymap[KEY_5] = SDLK_5;
     mymap[KEY_6] = SDLK_6;
     mymap[KEY_7] = SDLK_7;
     mymap[KEY_8] = SDLK_8;
     mymap[KEY_9] = SDLK_9;

     mymap[KEY_A] = SDLK_a;
     mymap[KEY_B] = SDLK_b;
     mymap[KEY_C] = SDLK_c;
     mymap[KEY_D] = SDLK_d;
     mymap[KEY_E] = SDLK_e;
     mymap[KEY_F] = SDLK_f;
     mymap[KEY_G] = SDLK_g;
     mymap[KEY_H] = SDLK_h;
     mymap[KEY_I] = SDLK_i;
     mymap[KEY_J] = SDLK_j;
     mymap[KEY_K] = SDLK_k;
     mymap[KEY_L] = SDLK_l;
     mymap[KEY_M] = SDLK_m;
     mymap[KEY_N] = SDLK_n;
     mymap[KEY_O] = SDLK_o;
     mymap[KEY_P] = SDLK_p;
     mymap[KEY_Q] = SDLK_q;
     mymap[KEY_R] = SDLK_r;
     mymap[KEY_S] = SDLK_s;
     mymap[KEY_T] = SDLK_t;
     mymap[KEY_U] = SDLK_u;
     mymap[KEY_V] = SDLK_v;
     mymap[KEY_W] = SDLK_w;
     mymap[KEY_X] = SDLK_x;
     mymap[KEY_Y] = SDLK_y;
     mymap[KEY_Z] = SDLK_z;

     mymap[KEY_UP] = SDLK_UP;
     mymap[KEY_DOWN] = SDLK_DOWN;
     mymap[KEY_LEFT] = SDLK_LEFT;
     mymap[KEY_RIGHT] = SDLK_RIGHT;
     
     mymap[KEY_ESC] = SDLK_ESCAPE;
     mymap[KEY_SPACE] = SDLK_SPACE;
     mymap[KEY_CAPSLOCK] = SDLK_CAPSLOCK;
     mymap[KEY_BACKSPACE] = SDLK_BACKSPACE;
     mymap[KEY_TAB] = SDLK_TAB;
     mymap[KEY_GRAVE] = SDLK_BACKQUOTE;
     mymap[KEY_COMMA] = SDLK_COMMA;
     mymap[KEY_DOT] = SDLK_PERIOD;
     mymap[KEY_APOSTROPHE] = SDLK_QUOTEDBL;
     mymap[KEY_LEFTBRACE] = SDLK_LEFTBRACKET;
     mymap[KEY_RIGHTBRACE] = SDLK_RIGHTBRACKET;
     mymap[KEY_ENTER] = SDLK_RETURN;
     mymap[KEY_MINUS] = SDLK_MINUS;
     mymap[KEY_EQUAL] = SDLK_EQUALS;
     mymap[KEY_SLASH] = SDLK_SLASH;
     mymap[KEY_BACKSLASH] = SDLK_BACKSLASH;

     mymap[KEY_F1] = SDLK_F1;
     mymap[KEY_F2] = SDLK_F2;
     mymap[KEY_F3] = SDLK_F3;
     mymap[KEY_F4] = SDLK_F4;
     mymap[KEY_F5] = SDLK_F5;

    mymap[KEY_RIGHTSHIFT] = SDLK_RSHIFT;
    mymap[KEY_LEFTSHIFT] = SDLK_LSHIFT;
    mymap[KEY_RIGHTCTRL] = SDLK_RCTRL;
    mymap[KEY_LEFTCTRL] = SDLK_LCTRL;
    mymap[KEY_RIGHTALT] = SDLK_RALT;
    mymap[KEY_LEFTALT] = SDLK_LALT;

    mouse_init();

    if ((thread = SDL_CreateThreadInternal(Mini_InputHandler, "Mini_InputHandler", 4096, NULL)) == NULL) {
        debug("%s, failed to create input thread\n", __func__);
    }
}

void Mini_EventQuit(void)
{
    debug("%s\n", __func__);
    running = 0;
    SDL_WaitThread(thread, NULL);
}

void Mini_PumpEvents(_THIS)
{
    int c0 = 0;
    int mouse = mouse_pump();

    select_hotkeys();
    for (c0 = 0; c0 < KEY_MAX; c0++) {
        if ((mouse_mode != MOUSE_OFF) &&
            ((c0 == MOUSE_MODE_KEY) || (mouse && mouse_pad_key(c0)))) {
            mykey[c0][0] = 0;
            mykey[c0][1] = 0;
            continue;
        }
        if (mykey[c0][1] && mymap[c0]) {
            mykey[c0][1] = 0;
            debug("%s, key pressed: %d\n", __func__, mymap[c0]);
            SDL_SendKeyboardKey(SDL_PRESSED, SDL_GetScancodeFromKey(mymap[c0]));
        }
        if (mykey[c0][0] && mymap[c0]) {
            mykey[c0][0] = 0;
            debug("%s, key released: %d\n", __func__, mymap[c0]);
            SDL_SendKeyboardKey(SDL_RELEASED, SDL_GetScancodeFromKey(mymap[c0]));
        }
    }
}

#endif

