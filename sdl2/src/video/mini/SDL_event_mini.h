// LGPL-2.1 License
// (C) 2025 Steward Fu <steward.fu@gmail.com>

#ifndef __SDL_EVENT_MINI_H__
#define __SDL_EVENT_MINI_H__

#include <linux/input.h>

/* A port that wants the pad to drive a pointer asks for one: `toggle` switches it on
   L2, `hold` keeps it only while L2 is down. Unset or `0` leaves the pad alone. */
#define MOUSE_MODE_ENV      "SDL_MINI_MOUSE"
/* Top speed in panel pixels a second, and the rect the pointer may not leave. */
#define MOUSE_SPEED_ENV     "SDL_MINI_MOUSE_SPEED"
#define MOUSE_RECT_ENV      "SDL_MINI_MOUSE_RECT"
/* How much bigger than its nine pixels the arrow is drawn; `0` draws none. */
#define MOUSE_ICON_ENV      "SDL_MINI_MOUSE_ICON"
#define MOUSE_ICON_MAG      2
#define MOUSE_ICON_TOP      8

/* SELECT + LEFT/RIGHT walks a bezel folder. */
#define BEZEL_KEY_HOLD      KEY_RIGHTCTRL
#define BEZEL_KEY_PREV      KEY_LEFT
#define BEZEL_KEY_NEXT      KEY_RIGHT

/* SELECT + R1 steps the scaling mode. */
#define SCALE_KEY           KEY_T

/* SELECT + L1 steps the screen effect. */
#define EFFECT_KEY          KEY_E

/* L2 is the shoulder Miyoo firmwares leave free; A and B sit where a mouse's buttons
   would. */
#define MOUSE_MODE_KEY      KEY_TAB
#define MOUSE_LEFT_KEY      KEY_SPACE
#define MOUSE_RIGHT_KEY     KEY_LEFTCTRL

/* A push starts this slow, to aim with a tap, and reaches the top speed over the
   ramp. */
#define MOUSE_SPEED_MIN     60
#define MOUSE_SPEED_MAX     480
/* A sanity bound on what SDL_MINI_MOUSE_SPEED may ask for. */
#define MOUSE_SPEED_TOP     4096
#define MOUSE_RAMP_MS       400
/* Longer than this between pumps is a stall, not a push, and must not move anything. */
#define MOUSE_STEP_MAX_MS   100
/* 1/sqrt(2), so a diagonal push is no faster than a straight one. */
#define MOUSE_DIAGONAL      0.7071f

void Mini_EventInit(void);
void Mini_EventQuit(void);
void Mini_PumpEvents(_THIS);
/* Where the pointer is in window coordinates, and how much to magnify the arrow that
   marks it; 0 when there is none to draw. */
int Mini_PointerAt(int *x, int *y);

#endif

