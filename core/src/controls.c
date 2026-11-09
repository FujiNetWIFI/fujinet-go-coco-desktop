/*
 * controls -- the on-screen joysticks: one layout for every toolkit, its hit
 * test, and the stick's gesture maths.
 *
 * A CoCo has two analog joysticks (cocosession.h): port 0 is the RIGHT one,
 * port 1 the LEFT, each a square base plate with a round gate and a stick, a
 * red fire button on its top (the CoCo 3's with a second). The window draws
 * the LEFT joystick on the left, the RIGHT one on the right, and the RESET
 * button centred above them:
 *
 *   CAPTION   "Left joystick"
 *   BASE      the plate, drawn behind everything on it (never pressed)
 *   BUTTON    a fire button, on the plate's top corners
 *   STICK     the gate, a disc: drag = the analog position
 *   DIRECTION the gate's outer ring in four 90-degree sectors (RIGHT, UP,
 *             LEFT, DOWN): a toolkit paints the sector of the gate between half
 *             its radius and all of it, and a Map pick or the binding hint
 *             goes on it; the rectangle is only the sector's bounding box
 *   READOUT   a text box under the gate: "X 31  Y 31"
 *
 * Units: a fire button's diameter is 1.0. The extents are the controls'
 * bounding box; a toolkit adds its own margin. The geometry is the only thing
 * here: colours are the frontends'. cocosession_controls_hit tests the exact
 * shapes (the gate and the buttons are discs, a DIRECTION an annular sector,
 * the rest rectangles) and returns the topmost control, so a DIRECTION wins
 * over the gate under it and a button over the plate.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <pthread.h>
#include <stddef.h>

#include "cocosession.h"

#define PI_F 3.14159265358979f

/* ---- geometry (layout units) ------------------------------------------------ */

#define BASE_WIDE   5.6f
#define BASE_HIGH   5.9f
#define BLOCK_TOP   2.3f           /* below the RESET button and its legend */
#define RIGHT_LEFT  6.4f           /* the right joystick's left edge */
#define GATE_D      3.6f
#define GATE_R      (GATE_D / 2)
#define TOTAL_WIDE  12.0f
#define TOTAL_HIGH  9.0f

#define COUNT_COCO3 21             /* RESET + 2 * (caption, base, 2 buttons, gate, 4 sectors, readout) */
#define COUNT_COCO2 19             /* one button each */

/* The exact shape of the round parts, parallel to the layout. */
typedef struct {
    float cenx, ceny;              /* the disc's centre */
    float rad0, rad1;              /* the ring (sectors); a disc is 0..rad1 */
    int sector;                    /* sectors: 0 RIGHT, 1 UP, 2 LEFT, 3 DOWN; -1 not one */
    int round;                     /* a disc or a ring; else the rectangle */
} shape;

typedef struct {
    coco_control list[COUNT_COCO3];
    shape shapes[COUNT_COCO3];
    int count;
    float wide, high;
} layout;

static layout g_coco3, g_coco2;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* The four sectors in order round the gate from RIGHT, counter-clockwise on
 * screen. */
static const int k_sector_acts[4] = { COCO_ACT_RIGHT, COCO_ACT_UP, COCO_ACT_LEFT, COCO_ACT_DOWN };
static const char *const k_sector_names[4] = { "RIGHT", "UP", "LEFT", "DOWN" };

static void add(layout *lay, int kind, const char *label, int port, int target,
                float left, float top, float wide, float high)
{
    coco_control *ctl = &lay->list[lay->count];

    ctl->kind = kind;
    ctl->label = label;
    ctl->legend = NULL;
    ctl->port = port;
    ctl->target = target;
    ctl->left = left;
    ctl->top = top;
    ctl->wide = wide;
    ctl->high = high;
    lay->shapes[lay->count].round = 0;
    lay->shapes[lay->count].sector = -1;
    lay->count++;
}

static void round_shape(layout *lay, float cenx, float ceny, float rad0, float rad1, int sector)
{
    shape *shp = &lay->shapes[lay->count - 1];

    shp->cenx = cenx;
    shp->ceny = ceny;
    shp->rad0 = rad0;
    shp->rad1 = rad1;
    shp->sector = sector;
    shp->round = 1;
}

/* The bounding box of the ring sector `sector` round (cenx, ceny). */
static void sector_box(float cenx, float ceny, float rad0, float rad1, int sector,
                       float *left, float *top, float *wide, float *high)
{
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    const float start = ((float)sector * 90.0f - 45.0f) * PI_F / 180.0f;
    int step, edge;

    for (step = 0; step <= 16; step++) {
        const float ang = start + (float)step * (90.0f * PI_F / 180.0f) / 16.0f;

        for (edge = 0; edge < 2; edge++) {
            const float rad = edge ? rad1 : rad0;
            const float posx = cenx + rad * cosf(ang), posy = ceny - rad * sinf(ang);

            if (posx < minx) minx = posx;
            if (posx > maxx) maxx = posx;
            if (posy < miny) miny = posy;
            if (posy > maxy) maxy = posy;
        }
    }
    *left = minx;
    *top = miny;
    *wide = maxx - minx;
    *high = maxy - miny;
}

static void joystick(layout *lay, int port, float left0, float top0, const char *caption,
                     int second_button)
{
    const float cenx = left0 + 2.8f, ceny = top0 + 4.1f;
    int sector;

    add(lay, COCO_CTL_CAPTION, caption, port, -1, left0, top0, BASE_WIDE, 0.7f);
    add(lay, COCO_CTL_BASE, NULL, port, -1, left0, top0 + 0.8f, BASE_WIDE, BASE_HIGH);

    add(lay, COCO_CTL_BUTTON, "1", port, COCO_TARGET_PORT(port, COCO_ACT_BUTTON1),
        left0 + 0.5f, top0 + 1.3f, 1.0f, 1.0f);
    round_shape(lay, left0 + 1.0f, top0 + 1.8f, 0.0f, 0.5f, -1);
    if (second_button) {
        add(lay, COCO_CTL_BUTTON, "2", port, COCO_TARGET_PORT(port, COCO_ACT_BUTTON2),
            left0 + 4.1f, top0 + 1.3f, 1.0f, 1.0f);
        round_shape(lay, left0 + 4.6f, top0 + 1.8f, 0.0f, 0.5f, -1);
    }

    add(lay, COCO_CTL_STICK, "Stick", port, -1, left0 + 1.0f, top0 + 2.3f, GATE_D, GATE_D);
    round_shape(lay, cenx, ceny, 0.0f, GATE_R, -1);

    for (sector = 0; sector < 4; sector++) {
        float left, top, wide, high;

        sector_box(cenx, ceny, GATE_R / 2, GATE_R, sector, &left, &top, &wide, &high);
        add(lay, COCO_CTL_DIRECTION, k_sector_names[sector], port,
            COCO_TARGET_PORT(port, k_sector_acts[sector]), left, top, wide, high);
        round_shape(lay, cenx, ceny, GATE_R / 2, GATE_R, sector);
    }

    add(lay, COCO_CTL_READOUT, NULL, port, -1, left0 + 1.0f, top0 + 6.0f, GATE_D, 0.6f);
}

static void build_one(layout *lay, int second_button)
{
    lay->count = 0;
    /* RESET centred over both joysticks, its label drawn just below it */
    add(lay, COCO_CTL_RESET, "RESET", -1, COCO_TARGET_SWITCH(COCO_SW_RESET), 5.2f, 0.0f, 1.6f, 1.0f);
    joystick(lay, 1, 0.0f, BLOCK_TOP, "Left joystick", second_button);
    joystick(lay, 0, RIGHT_LEFT, BLOCK_TOP, "Right joystick", second_button);
    lay->wide = TOTAL_WIDE;
    lay->high = TOTAL_HIGH;
}

static void build(void)
{
    build_one(&g_coco3, 1);
    build_one(&g_coco2, 0);
}

static const layout *layout_for(int machine)
{
    pthread_once(&g_once, build);
    return machine == COCOSESSION_MACHINE_COCO2 ? &g_coco2 : &g_coco3;
}

const coco_control *cocosession_controls_layout(int machine, int *count, float *wide, float *high)
{
    const layout *lay = layout_for(machine);

    if (count) *count = lay->count;
    if (wide) *wide = lay->wide;
    if (high) *high = lay->high;
    return lay->list;
}

/* The sector (0 = RIGHT, counter-clockwise on screen) a vector lies in. */
static int sector_of(float delx, float dely)
{
    float deg = atan2f(-dely, delx) * 180.0f / PI_F;      /* +y is down */

    if (deg < 0) deg += 360.0f;
    return (int)floorf((deg + 45.0f) / 90.0f) & 3;
}

int cocosession_controls_hit(int machine, float posx, float posy)
{
    const layout *lay = layout_for(machine);
    int idx;

    for (idx = lay->count - 1; idx >= 0; idx--) {
        const coco_control *ctl = &lay->list[idx];
        const shape *shp = &lay->shapes[idx];

        if (shp->round) {
            const float delx = posx - shp->cenx, dely = posy - shp->ceny;
            const float dist = sqrtf(delx * delx + dely * dely);

            if (dist > shp->rad1 || dist < shp->rad0)
                continue;
            if (shp->sector >= 0 && sector_of(delx, dely) != shp->sector)
                continue;
            return idx;
        }
        if (posx >= ctl->left && posx < ctl->left + ctl->wide
            && posy >= ctl->top && posy < ctl->top + ctl->high)
            return idx;
    }
    return -1;
}

static int milli_of(float offset, float radius)
{
    float val = offset / radius * 1000.0f;

    if (val != val) return 0;                  /* NaN */
    if (val < -1000.0f) val = -1000.0f;
    if (val > 1000.0f) val = 1000.0f;
    if (val > -40.0f && val < 40.0f) return 0;
    return (int)lroundf(val);
}

void cocosession_stick_from_vector(float delx, float dely, float radius, int *xaxis, int *yaxis)
{
    if (radius <= 0.0f) {
        *xaxis = *yaxis = 0;
        return;
    }
    *xaxis = milli_of(delx, radius);
    *yaxis = milli_of(dely, radius);
}
