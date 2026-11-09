/*
 * The Controllers window -- the CoCo's two joysticks drawn like the Tandy ones:
 * an off-white base, a round gate with a black stick, red fire buttons (two on
 * a CoCo 3), a "X 31  Y 31" readout, and the console's RESET between them, in
 * a case of the machine's colour (CoCo 3 silver-grey, CoCo 2 cream). Under them,
 * chosen by the Keyboard check, the machine's keyboard. Both are drawn from
 * the session's layout tables (cocosession_controls_layout and
 * cocosession_keyboard_layout), so every frontend shows the same parts.
 *
 * The sticks are ANALOG: dragging the gate (or the ring round it) puts the
 * pointer where the knob is, as a deflection in -1000..1000 on each axis
 * (cocosession_stick_from_vector); a corner is a full diagonal. A red button
 * is held for as long as the mouse button is down, dragging off lets it go;
 * the right button anywhere on a joystick holds its fire button 1 and the
 * middle one fire button 2. RESET fires when the mouse is released over it.
 * "Hold stick" keeps the stick where it was dropped, as the Tandy ones do not
 * centre; pressing on the centre and letting go without moving centres it.
 * Hiding, a machine switch and Defaults always centre. A key on screen is a
 * real key press of the machine, sticky for SHIFT, CTRL and ALT.
 *
 * What the machine sees held -- host keyboard, gamepads and this window
 * together (cocosession_joystick_held, _buttons_held, _keyboard_held) -- is
 * painted in the accent colour (the app icon's green), the knob following the
 * merged deflection: the window doubles as an input tester for a new gamepad.
 *
 * Keys typed with the window active go through the main window
 * (coco_window_key): the whole keyboard belongs to the CoCo, so they behave the
 * same whichever window has the focus, and the window's buttons are not
 * focusable so they never eat a key.
 *
 * FIXED SIZE, FLOATING: the drawn parts have fixed pixel sizes and labels
 * ellipsize, so neither Map mode nor a long gamepad name resizes the window; it
 * changes size only when the Keyboard check or the machine does. It is not
 * resizable, so tiling compositors float it, and hidden at startup (Ctrl+Shift+K
 * or the View menu shows it). SINGLETON, hidden rather than destroyed, so a
 * remap survives closing it; its timers run only while it is on screen.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click a direction of a
 * gate, a fire button or RESET (or the Stop in Debugger button) to pick the
 * target, then press a keyboard key OR a gamepad button. While armed the same
 * gestures pick instead of pressing, every control shows what is bound to it,
 * and typed keys are not typed into the machine (only then: the CoCo owns the
 * keyboard); once a target is picked the other controls are dimmed. Rebinding
 * steals the key from whatever held it. Escape backs out one level.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "controllers_window.h"

#include "../window.h"
#include "../help/help_window.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

#define UNIT 36                 /* pixels per joystick unit (a fire button) */
#define KEY_UNIT 28             /* pixels per key unit */
#define CASE_PAD 18             /* the case around the joysticks */
#define HEADER_HGT 22           /* the dark strip with the machine's name */
#define ORIGIN_TOP (HEADER_HGT + CASE_PAD)
#define KEY_PAD 14              /* the case around the keyboard */
#define KNOB_TRAVEL 1.25        /* units: the gate's radius less the knob's */
#define KNOB_RADIUS 0.55
#define LIGHT_MILLI 250         /* a sector is lit past this deflection */
#define CENTRE_ZONE 0.15        /* Hold stick: this central part recentres */
#define MAX_CAPS 64
#define MAX_PADS 8
#define PAD_ROWS 2              /* pad rows the window reserves; more scroll */
#define PAD_ROW_HGT 34
#define PAD_GAP 4
#define STOP_TARGET COCO_TARGET_SYSACT(COCO_SYSACT_DEBUG_STOP)

#define COL_ACCENT COCOSESSION_ACCENT_RGB
#define COL_ACCENT_DARK 0x5E8A2C
#define COL_ACCENT_INK 0x14210A
#define COL_FIRE 0xC8281E
#define COL_FIRE_LIT 0xE5594C
#define COL_FIRE_RIM 0x7D140E
#define COL_BASE_TOP 0xDDD9CF
#define COL_BASE_BOT 0xC2BDB1
#define COL_BASE_EDGE 0x8D887C
#define COL_GATE_RING 0x8F8B80
#define COL_GATE_FLOOR 0xA9A598
#define COL_GATE_MARK 0x7A766B
#define COL_HINT 0x5D5A52
#define COL_SECTOR 0x9C988B
#define COL_RESET_CAP 0x3A3A3D
#define COL_RESET_SIDE 0x1F1F21
#define COL_RESET_BAR 0xD33A2F
#define COL_READOUT 0xB9B5A8
#define COL_READOUT_INK 0x3B3A36

/* what a machine's case and caps look like */
typedef struct {
    const char *title;
    guint32 case_top, case_bot, case_lit, case_edge;
    guint32 strip, strip_ink, caption_ink;
    guint32 cap, cap_side, cap_ink, cap_shift, cap_special, cap_func;
} Skin;

static const Skin SKIN_COCO3 = {
    "COLOR COMPUTER 3", 0xB4B5B8, 0x8E8F93, 0xD9DADC, 0x5E5F63,
    0x26272A, 0xC9CACC, 0x26272A,
    0x2E2F33, 0x1F2023, 0xECECEC, 0x9FA0A5, 0x3E3F45, 0x55565C
};
static const Skin SKIN_COCO2 = {
    "COLOR COMPUTER 2", 0xE6E1D3, 0xCFC9B8, 0xF4F1E8, 0x9B9484,
    0x3A3A3C, 0xE6E1D3, 0x3A3A3C,
    0x3B3A38, 0x262523, 0xF2EFE6, 0xA8A59B, 0x4A4844, 0x4A4844
};

enum { TXT_BOLD = 1, TXT_MONO = 2 };

typedef struct {
    double cenx, ceny, radius;
} Gate;

/* what the machine sees held, sampled by the held tick */
typedef struct {
    int xaxis[2], yaxis[2];
    unsigned buttons[2];
    int reset;
} Snap;

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the CocoWindow: typed keys go through it */
static cocosession *g_session;
static AdwWindowTitle *g_title;
static GtkWidget *g_controls_area, *g_keys_area, *g_root;

static int g_machine = -1;      /* what the layouts were fetched for */
static const coco_control *g_ctl;
static int g_ctl_count;
static float g_ctl_wide, g_ctl_high;
static const coco_keycap *g_caps;
static int g_cap_count;
static float g_cap_wide, g_cap_high;

static Snap g_snap;
static unsigned char g_cap_bits[MAX_CAPS];
static double g_knob[2][2];     /* the knob as drawn, eased toward the stick */

/* The control the mouse holds (a button or RESET), the cap it holds, the
 * stick it drags, the fire button the right/middle button holds. */
static int g_press_ctl = -1;
static int g_press_cap = -1;
static struct {
    gboolean active, moved, centre;
    int port;
    int held_target;            /* fire button held by a right/middle press */
} g_drag = { FALSE, FALSE, FALSE, 0, -1 };
/* Hold stick: where each stick was left. */
static gboolean g_holding[2];
static int g_hold_x[2], g_hold_y[2];

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button, *g_clear_button, *g_stop_button;
static GtkWidget *g_map_hint;
static GtkWidget *g_show_bindings, *g_keyboard_check, *g_hold_check;
static guint g_capture_timer;
static guint g_swallow_code;    /* the key Map took: its repeats and release too */
static guint g_held_timer;
static guint g_pad_timer;

static GtkWidget *g_pad_box;
static GtkWidget *g_pad_port[MAX_PADS];     /* each pad's "drives ..." */
static int g_pad_rows;
static unsigned g_pad_generation = (unsigned)-1;
static GtkCssProvider *g_css;

static void refresh_all(void);
static void set_map_state(int state);

/* ---- small helpers ---------------------------------------------------------- */

static const Skin *skin_now(void)
{
    return g_machine == COCOSESSION_MACHINE_COCO2 ? &SKIN_COCO2 : &SKIN_COCO3;
}

static void set_rgb(cairo_t *cairo, guint32 colour)
{
    cairo_set_source_rgb(cairo, ((colour >> 16) & 0xFF) / 255.0,
                         ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
}

static guint32 mix_rgb(guint32 one, guint32 two, double frac)
{
    guint32 out = 0;
    int shift;
    for (shift = 16; shift >= 0; shift -= 8) {
        const double chan_one = (one >> shift) & 0xFF, chan_two = (two >> shift) & 0xFF;
        out |= (guint32)(chan_one + (chan_two - chan_one) * frac + 0.5) << shift;
    }
    return out;
}

static void add_stop(cairo_pattern_t *pattern, double offset, guint32 colour)
{
    cairo_pattern_add_color_stop_rgb(pattern, offset, ((colour >> 16) & 0xFF) / 255.0,
                                     ((colour >> 8) & 0xFF) / 255.0, (colour & 0xFF) / 255.0);
}

static void round_rect(cairo_t *cairo, double left, double top, double wide, double high,
                       double rad)
{
    if (rad > wide / 2) rad = wide / 2;
    if (rad > high / 2) rad = high / 2;
    cairo_new_sub_path(cairo);
    cairo_arc(cairo, left + wide - rad, top + rad, rad, -G_PI / 2, 0);
    cairo_arc(cairo, left + wide - rad, top + high - rad, rad, 0, G_PI / 2);
    cairo_arc(cairo, left + rad, top + high - rad, rad, G_PI / 2, G_PI);
    cairo_arc(cairo, left + rad, top + rad, rad, G_PI, 3 * G_PI / 2);
    cairo_close_path(cairo);
}

/* Text centred in a box, in an absolute pixel size, ellipsized to the box. */
static void put_text(cairo_t *cairo, GtkWidget *widget, const char *text, double pixels,
                     int style, guint32 colour, double left, double top, double wide,
                     double high)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *font = pango_font_description_new();
    int txtw, txth;

    pango_font_description_set_family(font, style & TXT_MONO ? "Monospace" : "Sans");
    pango_font_description_set_weight(font, style & TXT_BOLD ? PANGO_WEIGHT_BOLD
                                                             : PANGO_WEIGHT_NORMAL);
    pango_font_description_set_absolute_size(font, pixels * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_width(layout, (int)(wide * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size(layout, &txtw, &txth);
    set_rgb(cairo, colour);
    cairo_move_to(cairo, left, top + (high - txth) / 2.0);
    pango_cairo_show_layout(cairo, layout);
    /* the move_to above is still the current point, and the next arc would
     * join it with a line */
    cairo_new_path(cairo);
    pango_font_description_free(font);
    g_object_unref(layout);
}

static gboolean showing_bindings(void)
{
    return g_map_state != -2 ||
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_show_bindings));
}

/* "controllers_show_bindings"; the older spellings are read when it is absent. */
static gboolean show_bindings_saved(void)
{
    static const char *const older[] = { "controllers_key_names", "controllers_bindings",
                                         "show_bindings" };
    int val = cocosession_get_int(g_session, "controllers_show_bindings", -1);
    size_t idx;
    for (idx = 0; val < 0 && idx < G_N_ELEMENTS(older); idx++)
        val = cocosession_get_int(g_session, older[idx], -1);
    return val > 0;
}

static gboolean hold_mode(void)
{
    return g_hold_check &&
           gtk_check_button_get_active(GTK_CHECK_BUTTON(g_hold_check));
}

/* A host key's name as a small legend: the long ones shortened to fit. */
static void key_legend(guint32 keysym, char *dst, int dstsz)
{
    char name[32];
    cocosession_keysym_name(keysym, name, sizeof name);
    if (g_str_has_prefix(name, "Keypad "))
        g_snprintf(dst, dstsz, "KP %s", name + 7);
    else if (!strcmp(name, "Backspace"))
        g_snprintf(dst, dstsz, "BkSp");
    else if (!strcmp(name, "Escape"))
        g_snprintf(dst, dstsz, "Esc");
    else if (!strcmp(name, "Page Up"))
        g_snprintf(dst, dstsz, "PgUp");
    else if (!strcmp(name, "Page Down"))
        g_snprintf(dst, dstsz, "PgDn");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

/* A pad button's name, shortened: "D-pad Left" -> "D-Left". */
static void pad_legend(int button, char *dst, int dstsz)
{
    const char *name = coco_pad_button_name(button);
    if (g_str_has_prefix(name, "D-pad "))
        g_snprintf(dst, dstsz, "D-%s", name + 6);
    else if (!strcmp(name, "Left Shoulder"))
        g_snprintf(dst, dstsz, "LB");
    else if (!strcmp(name, "Right Shoulder"))
        g_snprintf(dst, dstsz, "RB");
    else if (!strcmp(name, "Left Trigger"))
        g_snprintf(dst, dstsz, "LT");
    else if (!strcmp(name, "Right Trigger"))
        g_snprintf(dst, dstsz, "RT");
    else if (!strcmp(name, "Left Stick"))
        g_snprintf(dst, dstsz, "LS");
    else if (!strcmp(name, "Right Stick"))
        g_snprintf(dst, dstsz, "RS");
    else
        g_snprintf(dst, dstsz, "%s", name);
}

/* The binding legend for a target ("" when not shown): its key, else its pad
 * button, else a dash. */
static void bound_text(int target, char *dst, int dstsz)
{
    coco_binding bnd;
    dst[0] = '\0';
    if (!showing_bindings() || target < 0)
        return;
    bnd = cocosession_binding_get(g_session, target);
    if (bnd.keysym)
        key_legend(bnd.keysym, dst, dstsz);
    else if (bnd.button != COCO_PAD_BTN_NONE)
        pad_legend(bnd.button, dst, dstsz);
    else
        g_snprintf(dst, dstsz, "\xe2\x80\x94");
}

static gboolean pickable(const coco_control *ctl)
{
    return ctl->target >= 0 &&
           (ctl->kind == COCO_CTL_DIRECTION || ctl->kind == COCO_CTL_BUTTON ||
            ctl->kind == COCO_CTL_RESET);
}

static gboolean gate_of(int port, Gate *gate)
{
    int idx;
    for (idx = 0; idx < g_ctl_count; idx++) {
        const coco_control *ctl = &g_ctl[idx];
        if (ctl->kind == COCO_CTL_STICK && ctl->port == port) {
            gate->cenx = CASE_PAD + (ctl->left + ctl->wide / 2) * UNIT;
            gate->ceny = ORIGIN_TOP + (ctl->top + ctl->high / 2) * UNIT;
            gate->radius = ctl->wide / 2 * UNIT;
            return TRUE;
        }
    }
    return FALSE;
}

/* ---- the held state ------------------------------------------------------------ */

static void take_snap(Snap *snap)
{
    int port;
    memset(snap, 0, sizeof *snap);
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++) {
        cocosession_joystick_held(g_session, port, &snap->xaxis[port], &snap->yaxis[port]);
        snap->buttons[port] = cocosession_buttons_held(g_session, port);
    }
    snap->reset = cocosession_switch_held(g_session, COCO_SW_RESET) != 0;
}

static gboolean dir_lit(int port, int act)
{
    switch (act) {
    case COCO_ACT_RIGHT: return g_snap.xaxis[port] >= LIGHT_MILLI;
    case COCO_ACT_LEFT: return g_snap.xaxis[port] <= -LIGHT_MILLI;
    case COCO_ACT_DOWN: return g_snap.yaxis[port] >= LIGHT_MILLI;
    case COCO_ACT_UP: return g_snap.yaxis[port] <= -LIGHT_MILLI;
    default: return FALSE;
    }
}

/* The knob's eased state toward the merged stick. Returns whether it moved. */
static gboolean ease_knob(int port)
{
    double goal[2];
    gboolean moved = FALSE;
    double len;
    int axis;

    goal[0] = g_snap.xaxis[port] / 1000.0;
    goal[1] = g_snap.yaxis[port] / 1000.0;
    len = hypot(goal[0], goal[1]);
    if (len > 1.0) {                    /* a corner: drawn inside the gate */
        goal[0] /= len;
        goal[1] /= len;
    }
    for (axis = 0; axis < 2; axis++) {
        const double dist = goal[axis] - g_knob[port][axis];
        if (fabs(dist) < 0.004) {
            if (dist != 0) {
                g_knob[port][axis] = goal[axis];
                moved = TRUE;
            }
            continue;
        }
        g_knob[port][axis] += dist * 0.4;
        moved = TRUE;
    }
    return moved;
}

/* Samples what the machine sees and redraws what changed. */
static void refresh_held(void)
{
    Snap now;
    gboolean ctl_changed = FALSE, keys_changed = FALSE;
    int idx, port;

    take_snap(&now);
    if (memcmp(&now, &g_snap, sizeof now) != 0) {
        g_snap = now;
        ctl_changed = TRUE;
    }
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        if (ease_knob(port))
            ctl_changed = TRUE;
    for (idx = 0; idx < g_cap_count && idx < MAX_CAPS; idx++) {
        const unsigned char bits = (unsigned char)cocosession_keyboard_held(g_session, idx);
        if (bits != g_cap_bits[idx]) {
            g_cap_bits[idx] = bits;
            keys_changed = TRUE;
        }
    }
    if (ctl_changed && g_controls_area)
        gtk_widget_queue_draw(g_controls_area);
    if (keys_changed && g_keys_area)
        gtk_widget_queue_draw(g_keys_area);
}

/* ---- drawing: the case -------------------------------------------------------- */

static void paint_case(cairo_t *cairo, const Skin *skin, int wid, int hgt, gboolean header)
{
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, hgt);

    add_stop(grad, 0, skin->case_top);
    add_stop(grad, 1, skin->case_bot);
    round_rect(cairo, 0.5, 0.5, wid - 1, hgt - 1, 16);
    cairo_set_source(cairo, grad);
    cairo_fill(cairo);
    cairo_pattern_destroy(grad);
    if (header) {
        cairo_save(cairo);
        round_rect(cairo, 0.5, 0.5, wid - 1, hgt - 1, 16);
        cairo_clip(cairo);
        cairo_rectangle(cairo, 0, 0, wid, HEADER_HGT);
        set_rgb(cairo, skin->strip);
        cairo_fill(cairo);
        cairo_rectangle(cairo, 0, HEADER_HGT, wid, 2);
        set_rgb(cairo, COL_ACCENT);
        cairo_fill(cairo);
        cairo_restore(cairo);
    }
    round_rect(cairo, 0.5, 0.5, wid - 1, hgt - 1, 16);
    set_rgb(cairo, skin->case_edge);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
    /* the lit top edge of the moulding */
    if (!header) {
        cairo_move_to(cairo, 18, 2.5);
        cairo_line_to(cairo, wid - 18, 2.5);
        set_rgb(cairo, skin->case_lit);
        cairo_set_line_width(cairo, 1.5);
        cairo_stroke(cairo);
    }
}

/* ---- drawing: the joysticks ------------------------------------------------------ */

static double sector_angle(int act)
{
    switch (act) {
    case COCO_ACT_RIGHT: return 0;
    case COCO_ACT_DOWN: return G_PI / 2;
    case COCO_ACT_LEFT: return G_PI;
    default: return -G_PI / 2;          /* UP */
    }
}

static void draw_button(cairo_t *cairo, GtkWidget *widget, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const int bit = ctl->target % COCO_ACT_PER_PORT - COCO_ACT_BUTTON1;
    const double cenx = CASE_PAD + (ctl->left + ctl->wide / 2) * UNIT;
    const double ceny = ORIGIN_TOP + (ctl->top + ctl->high / 2) * UNIT;
    const double rad = ctl->wide / 2 * UNIT;
    const gboolean lit = ((g_snap.buttons[ctl->port] >> bit) & 1) || g_press_ctl == idx;
    const gboolean picked = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean dim = g_map_state >= 0 && !picked;
    const gboolean glow = lit || picked;
    cairo_pattern_t *grad;
    char bound[40];

    if (dim) cairo_push_group(cairo);
    cairo_arc(cairo, cenx, ceny + 3, rad, 0, 2 * G_PI);
    set_rgb(cairo, glow ? COL_ACCENT_DARK : COL_FIRE_RIM);
    cairo_fill(cairo);
    grad = cairo_pattern_create_radial(cenx - rad * 0.3, ceny - rad * 0.35, rad * 0.1,
                                       cenx, ceny, rad);
    add_stop(grad, 0, glow ? 0xC9E6A0 : COL_FIRE_LIT);
    add_stop(grad, 1, glow ? COL_ACCENT : COL_FIRE);
    cairo_arc(cairo, cenx, ceny, rad, 0, 2 * G_PI);
    cairo_set_source(cairo, grad);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(grad);
    set_rgb(cairo, glow ? 0xFFFFFF : COL_FIRE_RIM);
    cairo_set_line_width(cairo, glow ? 2 : 1);
    cairo_stroke(cairo);

    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        /* beside the cap, toward the middle of the plate; never on it */
        const double side = bit == 0 ? 1 : -1;
        const double note_wide = 1.3 * UNIT;
        put_text(cairo, widget, bound, 9, TXT_BOLD, COL_READOUT_INK,
                 side > 0 ? cenx + rad + 4 : cenx - rad - 4 - note_wide, ceny - rad, note_wide, 2 * rad);
    }
    if (ctl->label) {
        /* the button's number, under the cap */
        put_text(cairo, widget, ctl->label, 11, TXT_BOLD, 0x3A3A3C, cenx - 8, ceny + rad + 3, 16, 16);
    }
    if (dim) {
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, 0.4);
    }
}

static void draw_gate(cairo_t *cairo, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const double cenx = CASE_PAD + (ctl->left + ctl->wide / 2) * UNIT;
    const double ceny = ORIGIN_TOP + (ctl->top + ctl->high / 2) * UNIT;
    const double rad = ctl->wide / 2 * UNIT;
    int tick;

    cairo_arc(cairo, cenx, ceny, rad, 0, 2 * G_PI);
    set_rgb(cairo, COL_GATE_RING);
    cairo_fill(cairo);
    cairo_arc(cairo, cenx, ceny, rad - 4, 0, 2 * G_PI);
    set_rgb(cairo, COL_GATE_FLOOR);
    cairo_fill(cairo);
    /* cross-hairs and the four diagonal ticks */
    set_rgb(cairo, COL_GATE_MARK);
    cairo_set_line_width(cairo, 1);
    cairo_move_to(cairo, cenx - rad + 6, ceny);
    cairo_line_to(cairo, cenx + rad - 6, ceny);
    cairo_move_to(cairo, cenx, ceny - rad + 6);
    cairo_line_to(cairo, cenx, ceny + rad - 6);
    cairo_stroke(cairo);
    for (tick = 0; tick < 4; tick++) {
        const double ang = G_PI / 4 + tick * G_PI / 2;
        cairo_move_to(cairo, cenx + cos(ang) * (rad - 12), ceny + sin(ang) * (rad - 12));
        cairo_line_to(cairo, cenx + cos(ang) * (rad - 5), ceny + sin(ang) * (rad - 5));
    }
    cairo_stroke(cairo);
}

/* One of the gate's four ring sectors: painted only to say it is held, picked
 * or (Show bindings / Map) what it is bound to. */
static void draw_sector(cairo_t *cairo, GtkWidget *widget, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const int act = ctl->target % COCO_ACT_PER_PORT;
    const gboolean lit = dir_lit(ctl->port, act);
    const gboolean picked = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean dim = g_map_state >= 0 && !picked;
    const gboolean glow = lit || picked;
    const double ang = sector_angle(act);
    const double span = G_PI / 4;
    Gate gate;
    double mid;
    char bound[40];

    if (!gate_of(ctl->port, &gate) || !(glow || showing_bindings()))
        return;
    if (dim) cairo_push_group(cairo);
    cairo_arc(cairo, gate.cenx, gate.ceny, gate.radius - 3, ang - span, ang + span);
    cairo_arc_negative(cairo, gate.cenx, gate.ceny, gate.radius / 2, ang + span, ang - span);
    cairo_close_path(cairo);
    set_rgb(cairo, glow ? COL_ACCENT : COL_SECTOR);
    cairo_fill_preserve(cairo);
    set_rgb(cairo, glow ? 0xFFFFFF : COL_GATE_MARK);
    cairo_set_line_width(cairo, glow ? 2 : 1);
    cairo_stroke(cairo);

    mid = gate.radius * 0.75;
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0]) {
        /* left and right: pulled in off the rim, a size smaller */
        const gboolean side = fabs(cos(ang)) > 0.5;
        const double legx = gate.cenx + cos(ang) * (side ? gate.radius * 0.64 : mid);
        put_text(cairo, widget, bound, side ? 7 : 8, TXT_BOLD, glow ? COL_ACCENT_INK : COL_READOUT_INK,
                 legx - 17, gate.ceny + sin(ang) * mid - 8, 34, 16);
    } else {
        /* an arrow pointing the way the stick pushes */
        const double dirx = cos(ang), diry = sin(ang);
        const double midx = gate.cenx + dirx * mid, midy = gate.ceny + diry * mid;
        cairo_move_to(cairo, midx + dirx * 6, midy + diry * 6);
        cairo_line_to(cairo, midx - dirx * 4 - diry * 6, midy - diry * 4 + dirx * 6);
        cairo_line_to(cairo, midx - dirx * 4 + diry * 6, midy - diry * 4 - dirx * 6);
        cairo_close_path(cairo);
        set_rgb(cairo, glow ? COL_ACCENT_INK : COL_HINT);
        cairo_fill(cairo);
    }
    if (dim) {
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, 0.4);
    }
}

/* The stick: a black knob at the merged deflection, ringed in the accent while
 * it is off centre. */
static void draw_knob(cairo_t *cairo, int port)
{
    Gate gate;
    double posx, posy, rad;
    cairo_pattern_t *grad;

    if (!gate_of(port, &gate))
        return;
    rad = KNOB_RADIUS * UNIT;
    posx = gate.cenx + g_knob[port][0] * KNOB_TRAVEL * UNIT;
    posy = gate.ceny + g_knob[port][1] * KNOB_TRAVEL * UNIT;
    /* the shaft's shadow on the gate floor */
    cairo_move_to(cairo, gate.cenx + 2, gate.ceny + 3);
    cairo_line_to(cairo, posx + 3, posy + 4);
    cairo_set_line_cap(cairo, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cairo, 7);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.25);
    cairo_stroke(cairo);
    /* the pivot */
    cairo_arc(cairo, gate.cenx, gate.ceny, 0.3 * UNIT, 0, 2 * G_PI);
    set_rgb(cairo, 0x1A1A1C);
    cairo_fill(cairo);
    if (g_snap.xaxis[port] != 0 || g_snap.yaxis[port] != 0) {
        cairo_arc(cairo, posx, posy, rad + 3, 0, 2 * G_PI);
        set_rgb(cairo, COL_ACCENT);
        cairo_fill(cairo);
    }
    grad = cairo_pattern_create_radial(posx - rad * 0.35, posy - rad * 0.35, rad * 0.1, posx,
                                       posy, rad);
    add_stop(grad, 0, 0x5A5A60);
    add_stop(grad, 0.7, 0x1A1A1C);
    add_stop(grad, 1, 0x0A0A0B);
    cairo_arc(cairo, posx, posy, rad, 0, 2 * G_PI);
    cairo_set_source(cairo, grad);
    cairo_fill(cairo);
    cairo_pattern_destroy(grad);
}

static void draw_readout(cairo_t *cairo, GtkWidget *widget, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const double left = CASE_PAD + ctl->left * UNIT, top = ORIGIN_TOP + ctl->top * UNIT;
    char text[32];

    round_rect(cairo, left, top, ctl->wide * UNIT, ctl->high * UNIT, 5);
    set_rgb(cairo, COL_READOUT);
    cairo_fill(cairo);
    g_snprintf(text, sizeof text, "X %2d  Y %2d", coco_axis_6bit(g_snap.xaxis[ctl->port]),
               coco_axis_6bit(g_snap.yaxis[ctl->port]));
    put_text(cairo, widget, text, 12, TXT_MONO | TXT_BOLD, COL_READOUT_INK, left, top,
             ctl->wide * UNIT, ctl->high * UNIT);
}

static void draw_reset(cairo_t *cairo, GtkWidget *widget, const Skin *skin, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const double left = CASE_PAD + ctl->left * UNIT, top = ORIGIN_TOP + ctl->top * UNIT;
    const double wide = ctl->wide * UNIT, high = ctl->high * UNIT;
    const gboolean lit = g_snap.reset || g_press_ctl == idx;
    const gboolean picked = g_map_state >= 0 && g_map_state == ctl->target;
    const gboolean dim = g_map_state >= 0 && !picked;
    const gboolean glow = lit || picked;
    char bound[40];

    if (dim) cairo_push_group(cairo);
    round_rect(cairo, left, top + 3, wide, high - 3, 6);
    set_rgb(cairo, glow ? COL_ACCENT_DARK : COL_RESET_SIDE);
    cairo_fill(cairo);
    round_rect(cairo, left, top, wide, high - 3, 6);
    set_rgb(cairo, glow ? COL_ACCENT : COL_RESET_CAP);
    cairo_fill_preserve(cairo);
    set_rgb(cairo, glow ? 0xFFFFFF : COL_RESET_SIDE);
    cairo_set_line_width(cairo, glow ? 2 : 1);
    cairo_stroke(cairo);
    if (!glow) {
        cairo_rectangle(cairo, left + 8, top + high - 10, wide - 16, 3);
        set_rgb(cairo, COL_RESET_BAR);
        cairo_fill(cairo);
    }
    bound_text(ctl->target, bound, sizeof bound);
    if (bound[0])
        put_text(cairo, widget, bound, 10, TXT_BOLD, glow ? COL_ACCENT_INK : 0xE0E0E0, left,
                 top, wide, high - 3);
    if (ctl->label)
        put_text(cairo, widget, ctl->label, 10, TXT_BOLD, skin->caption_ink, left - 10,
                 top + high + 1, wide + 20, 14);
    if (dim) {
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, 0.4);
    }
}

static void draw_base(cairo_t *cairo, int idx)
{
    const coco_control *ctl = &g_ctl[idx];
    const double left = CASE_PAD + ctl->left * UNIT, top = ORIGIN_TOP + ctl->top * UNIT;
    const double wide = ctl->wide * UNIT, high = ctl->high * UNIT;
    cairo_pattern_t *grad = cairo_pattern_create_linear(0, top, 0, top + high);

    round_rect(cairo, left + 2, top + 4, wide, high, 0.5 * UNIT);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.25);
    cairo_fill(cairo);
    add_stop(grad, 0, COL_BASE_TOP);
    add_stop(grad, 1, COL_BASE_BOT);
    round_rect(cairo, left, top, wide, high, 0.5 * UNIT);
    cairo_set_source(cairo, grad);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(grad);
    set_rgb(cairo, COL_BASE_EDGE);
    cairo_set_line_width(cairo, 1);
    cairo_stroke(cairo);
}

static void draw_controls(GtkDrawingArea *area, cairo_t *cairo, int wid, int hgt, gpointer data)
{
    GtkWidget *widget = GTK_WIDGET(area);
    const Skin *skin = skin_now();
    int idx;
    (void)data;

    paint_case(cairo, skin, wid, hgt, TRUE);
    put_text(cairo, widget, skin->title, 11, TXT_BOLD, skin->strip_ink, 0, 0, wid, HEADER_HGT);

    for (idx = 0; idx < g_ctl_count; idx++) {
        const coco_control *ctl = &g_ctl[idx];
        switch (ctl->kind) {
        case COCO_CTL_BASE:
            draw_base(cairo, idx);
            break;
        case COCO_CTL_CAPTION:
            put_text(cairo, widget, ctl->label, 12, TXT_BOLD, skin->caption_ink,
                     CASE_PAD + ctl->left * UNIT, ORIGIN_TOP + ctl->top * UNIT,
                     ctl->wide * UNIT, ctl->high * UNIT);
            break;
        case COCO_CTL_BUTTON:
            draw_button(cairo, widget, idx);
            break;
        case COCO_CTL_STICK:
            draw_gate(cairo, idx);
            break;
        case COCO_CTL_DIRECTION:
            draw_sector(cairo, widget, idx);
            break;
        case COCO_CTL_READOUT:
            draw_knob(cairo, ctl->port);
            draw_readout(cairo, widget, idx);
            break;
        case COCO_CTL_RESET:
            draw_reset(cairo, widget, skin, idx);
            break;
        default:
            break;
        }
    }
}

/* ---- drawing: the keyboard -------------------------------------------------------- */

static void draw_arrow(cairo_t *cairo, const char *label, double cenx, double ceny, guint32 colour)
{
    /* the caps carry UTF-8 arrows; drawn as triangles so no font needs them */
    double dirx = 0, diry = 0;
    if (!strcmp(label, "\xe2\x86\x91")) diry = -1;
    else if (!strcmp(label, "\xe2\x86\x93")) diry = 1;
    else if (!strcmp(label, "\xe2\x86\x90")) dirx = -1;
    else dirx = 1;
    cairo_move_to(cairo, cenx + dirx * 7, ceny + diry * 7);
    cairo_line_to(cairo, cenx - dirx * 5 - diry * 6, ceny - diry * 5 + dirx * 6);
    cairo_line_to(cairo, cenx - dirx * 5 + diry * 6, ceny - diry * 5 - dirx * 6);
    cairo_close_path(cairo);
    set_rgb(cairo, colour);
    cairo_fill(cairo);
}

static void draw_cap(cairo_t *cairo, GtkWidget *widget, const Skin *skin, int idx)
{
    const coco_keycap *cap = &g_caps[idx];
    const int bits = idx < MAX_CAPS ? g_cap_bits[idx] : 0;
    const gboolean latched = (bits & 2) != 0;
    const gboolean lit = (bits & 3) != 0 || g_press_cap == idx;
    const double left = KEY_PAD + cap->left * KEY_UNIT, top = KEY_PAD + cap->top * KEY_UNIT;
    const double wide = cap->wide * KEY_UNIT, high = cap->high * KEY_UNIT;
    guint32 face = skin->cap, side = skin->cap_side, ink = skin->cap_ink;
    double pixels, labtop = top, labhigh = high - 3;
    const int chars = (int)g_utf8_strlen(cap->label, -1);

    if (cap->kind == COCO_KEYCAP_SPECIAL || cap->kind == COCO_KEYCAP_MODIFIER)
        face = skin->cap_special;
    else if (cap->kind == COCO_KEYCAP_FUNCTION)
        face = skin->cap_func;
    if (lit) {
        face = COL_ACCENT; side = COL_ACCENT_DARK; ink = COL_ACCENT_INK;
    }
    round_rect(cairo, left, top + 3, wide, high - 3, 5);
    set_rgb(cairo, side);
    cairo_fill(cairo);
    round_rect(cairo, left, top, wide, high - 3, 5);
    set_rgb(cairo, face);
    cairo_fill_preserve(cairo);
    set_rgb(cairo, lit ? 0xFFFFFF : mix_rgb(face, 0x000000, 0.45));
    cairo_set_line_width(cairo, lit ? 2 : 1);
    cairo_stroke(cairo);

    if (cap->kind == COCO_KEYCAP_ARROW) {
        draw_arrow(cairo, cap->label, left + wide / 2, top + (high - 3) / 2, ink);
    } else if (cap->kind != COCO_KEYCAP_SPACE) {
        if (cap->shifted) {
            put_text(cairo, widget, cap->shifted, 8, 0, lit ? ink : skin->cap_shift, left + 2,
                     top + 1, wide - 4, 10);
            labtop = top + 8;
            labhigh = high - 11;
        }
        pixels = chars <= 1 ? 13.0 : MAX(7.5, MIN(11.0, (wide - 6) / (chars * 0.66)));
        put_text(cairo, widget, cap->label, pixels, TXT_BOLD, ink, left, labtop, wide, labhigh);
    }
    if (latched) {
        cairo_arc(cairo, left + 6, top + 6, 3, 0, 2 * G_PI);
        set_rgb(cairo, 0xFFFFFF);
        cairo_fill_preserve(cairo);
        set_rgb(cairo, COL_ACCENT_DARK);
        cairo_set_line_width(cairo, 1);
        cairo_stroke(cairo);
    }
}

static void draw_keys(GtkDrawingArea *area, cairo_t *cairo, int wid, int hgt, gpointer data)
{
    GtkWidget *widget = GTK_WIDGET(area);
    const Skin *skin = skin_now();
    int idx;
    (void)data;

    paint_case(cairo, skin, wid, hgt, FALSE);
    for (idx = 0; idx < g_cap_count; idx++)
        draw_cap(cairo, widget, skin, idx);
}

/* ---- hit testing ---------------------------------------------------------------- */

static int control_at(double posx, double posy)
{
    return cocosession_controls_hit(g_machine, (float)((posx - CASE_PAD) / UNIT),
                                    (float)((posy - ORIGIN_TOP) / UNIT));
}

static int cap_at(double posx, double posy)
{
    int idx;
    for (idx = g_cap_count - 1; idx >= 0; idx--) {
        const coco_keycap *cap = &g_caps[idx];
        const double left = KEY_PAD + cap->left * KEY_UNIT, top = KEY_PAD + cap->top * KEY_UNIT;
        if (posx >= left && posx < left + cap->wide * KEY_UNIT && posy >= top &&
            posy < top + cap->high * KEY_UNIT)
            return idx;
    }
    return -1;
}

/* ---- pressing --------------------------------------------------------------------- */

static void run_sysaction(int sysact)
{
    if (g_parent)
        coco_window_run_sysaction(COCO_WINDOW(g_parent), sysact);
    else
        cocosession_sysaction(g_session, sysact);
}

/* Sets the stick the pointer is dragging to the pointer's place on the gate. */
static void stick_follow(double posx, double posy)
{
    Gate gate;
    int xaxis, yaxis;

    if (!gate_of(g_drag.port, &gate))
        return;
    cocosession_stick_from_vector((float)((posx - gate.cenx) / UNIT),
                                  (float)((posy - gate.ceny) / UNIT), (float)KNOB_TRAVEL,
                                  &xaxis, &yaxis);
    g_hold_x[g_drag.port] = xaxis;
    g_hold_y[g_drag.port] = yaxis;
    cocosession_pointer_set(g_session, g_drag.port, xaxis, yaxis, 0);
    refresh_held();
}

static void centre_stick(int port)
{
    g_holding[port] = FALSE;
    g_hold_x[port] = g_hold_y[port] = 0;
    cocosession_pointer_set(g_session, port, 0, 0, 0);
}

static void centre_sticks(void)
{
    int port;
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        centre_stick(port);
}

/* Lets go of the stick being dragged: it stays where it is in Hold stick mode,
 * unless the drag began on the centre and never moved. */
static void stick_release(void)
{
    if (!g_drag.active)
        return;
    g_drag.active = FALSE;
    if (hold_mode() && !(g_drag.centre && !g_drag.moved))
        g_holding[g_drag.port] = TRUE;
    else
        centre_stick(g_drag.port);
}

static void fire_release(void)
{
    if (g_drag.held_target >= 0)
        cocosession_press(g_session, g_drag.held_target, 0);
    g_drag.held_target = -1;
}

/* Lets go of the control the mouse holds. RESET fires on release, like a real
 * button: pressing and dragging off should not reset the machine. */
static void control_release(gboolean activate)
{
    const coco_control *ctl;

    if (g_press_ctl < 0)
        return;
    ctl = &g_ctl[g_press_ctl];
    g_press_ctl = -1;
    if (ctl->kind == COCO_CTL_RESET) {
        if (activate) {
            cocosession_press(g_session, ctl->target, 1);
            cocosession_press(g_session, ctl->target, 0);
        }
    } else {
        cocosession_press(g_session, ctl->target, 0);
    }
}

static void cap_release(void)
{
    if (g_press_cap < 0)
        return;
    cocosession_keyboard_press(g_session, g_press_cap, 0);
    g_press_cap = -1;
}

/* Everything the mouse holds in this window (not the main window's keys, not
 * a stick Hold stick keeps). */
static void mouse_release(void)
{
    control_release(FALSE);
    cap_release();
    stick_release();
    fire_release();
    refresh_held();
    if (g_controls_area) gtk_widget_queue_draw(g_controls_area);
    if (g_keys_area) gtk_widget_queue_draw(g_keys_area);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    cocosession_gamepad_capture_cancel(g_session);
}

static void say_bound(const char *what, const char *stolen)
{
    char msg[220];
    if (stolen[0])
        g_snprintf(msg, sizeof msg, "Bound %s (was %s)", what, stolen);
    else
        g_snprintf(msg, sizeof msg, "Bound %s", what);
    gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
}

static gboolean capture_tick(gpointer data)
{
    int button;
    (void)data;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (cocosession_gamepad_capture_poll(g_session, &button)) {
        char stolen[128];
        cocosession_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        g_capture_timer = 0;
        set_map_state(-1);     /* stay armed: remapping several in a row is normal */
        say_bound(coco_pad_button_name(button), stolen);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "coco-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Drag a stick, hold a button, tap a key. Map rebinds a control.");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "coco-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint),
                           "Click a direction, a fire button or RESET to remap it");
    } else {
        char msg[160];
        mouse_release();
        g_snprintf(msg, sizeof msg, "Press a key or pad button for %s. Esc cancels.",
                   coco_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        cocosession_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_all();
}

static void on_controls_begin(GtkGestureDrag *gesture, double posx, double posy, gpointer data)
{
    const int button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    const int hit = control_at(posx, posy);
    const coco_control *ctl = hit >= 0 ? &g_ctl[hit] : NULL;
    (void)data;

    if (!ctl)
        return;
    if (g_map_state != -2) {
        /* armed: picks (or changes) the target */
        if (button == GDK_BUTTON_PRIMARY && pickable(ctl))
            set_map_state(ctl->target);
        return;
    }
    mouse_release();
    if (button == GDK_BUTTON_SECONDARY || button == GDK_BUTTON_MIDDLE) {
        if (ctl->port >= 0) {
            g_drag.held_target = COCO_TARGET_PORT(
                ctl->port, button == GDK_BUTTON_MIDDLE ? COCO_ACT_BUTTON2 : COCO_ACT_BUTTON1);
            cocosession_press(g_session, g_drag.held_target, 1);
        }
    } else if (ctl->kind == COCO_CTL_STICK || ctl->kind == COCO_CTL_DIRECTION) {
        Gate gate;
        g_drag.active = TRUE;
        g_drag.moved = FALSE;
        g_drag.port = ctl->port;
        g_drag.centre = FALSE;
        if (gate_of(ctl->port, &gate) && hold_mode())
            g_drag.centre = hypot(posx - gate.cenx, posy - gate.ceny) <
                            CENTRE_ZONE * KNOB_TRAVEL * UNIT;
        if (!g_drag.centre)
            stick_follow(posx, posy);
    } else if (ctl->kind == COCO_CTL_BUTTON || ctl->kind == COCO_CTL_RESET) {
        g_press_ctl = hit;
        if (ctl->kind == COCO_CTL_BUTTON)
            cocosession_press(g_session, ctl->target, 1);
    }
    refresh_held();
    gtk_widget_queue_draw(g_controls_area);
}

static void on_controls_update(GtkGestureDrag *gesture, double offx, double offy, gpointer data)
{
    double startx, starty;
    (void)data;

    gtk_gesture_drag_get_start_point(gesture, &startx, &starty);
    if (g_drag.active) {
        if (hypot(offx, offy) > 3)
            g_drag.moved = TRUE;
        if (g_drag.moved || !g_drag.centre)
            stick_follow(startx + offx, starty + offy);
    } else if (g_press_ctl >= 0 && control_at(startx + offx, starty + offy) != g_press_ctl) {
        /* dragging off the control lets it go */
        control_release(FALSE);
        gtk_widget_queue_draw(g_controls_area);
    }
}

static void on_controls_end(GtkGestureDrag *gesture, double offx, double offy, gpointer data)
{
    double startx, starty;
    (void)data;

    gtk_gesture_drag_get_start_point(gesture, &startx, &starty);
    stick_release();
    fire_release();
    control_release(control_at(startx + offx, starty + offy) == g_press_ctl);
    refresh_held();
    gtk_widget_queue_draw(g_controls_area);
}

static void on_controls_cancel(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    mouse_release();
}

static void on_keys_begin(GtkGestureDrag *gesture, double posx, double posy, gpointer data)
{
    const int hit = cap_at(posx, posy);
    (void)gesture; (void)data;

    if (hit < 0 || g_map_state != -2)
        return;                 /* the keyboard is inert in Map mode */
    if (cocosession_machine(g_session) != g_machine)
        return;                 /* the layout is about to be rebuilt */
    cap_release();
    g_press_cap = hit;
    cocosession_keyboard_press(g_session, hit, 1);
    refresh_held();
    gtk_widget_queue_draw(g_keys_area);
}

static void on_keys_update(GtkGestureDrag *gesture, double offx, double offy, gpointer data)
{
    double startx, starty;
    (void)data;

    gtk_gesture_drag_get_start_point(gesture, &startx, &starty);
    if (g_press_cap >= 0 && cap_at(startx + offx, starty + offy) != g_press_cap) {
        /* dragging off the cap lets it go */
        cap_release();
        refresh_held();
        gtk_widget_queue_draw(g_keys_area);
    }
}

static void on_keys_end(GtkGestureDrag *gesture, double offx, double offy, gpointer data)
{
    (void)gesture; (void)offx; (void)offy; (void)data;
    cap_release();
    refresh_held();
    gtk_widget_queue_draw(g_keys_area);
}

static void on_keys_cancel(GtkGesture *gesture, GdkEventSequence *seq, gpointer data)
{
    (void)gesture; (void)seq; (void)data;
    cap_release();
    refresh_held();
}

static gboolean on_controls_tooltip(GtkWidget *widget, int posx, int posy, gboolean kbd,
                                    GtkTooltip *tip, gpointer data)
{
    const int hit = kbd ? -1 : control_at(posx, posy);
    char key[32], text[260];
    coco_binding bnd;
    (void)widget; (void)data;

    if (hit < 0 || g_ctl[hit].target < 0)
        return FALSE;
    bnd = cocosession_binding_get(g_session, g_ctl[hit].target);
    cocosession_keysym_name(bnd.keysym, key, sizeof key);
    g_snprintf(text, sizeof text, "%s\nKey: %s    Gamepad: %s",
               coco_target_name(g_ctl[hit].target), key[0] ? key : "none",
               bnd.button != COCO_PAD_BTN_NONE ? coco_pad_button_name(bnd.button) : "none");
    gtk_tooltip_set_text(tip, text);
    return TRUE;
}

static gboolean on_keys_tooltip(GtkWidget *widget, int posx, int posy, gboolean kbd,
                                GtkTooltip *tip, gpointer data)
{
    const int hit = kbd ? -1 : cap_at(posx, posy);
    (void)widget; (void)data;

    if (hit < 0 || g_caps[hit].kind == COCO_KEYCAP_ARROW)
        return FALSE;
    gtk_tooltip_set_text(tip, g_caps[hit].kind == COCO_KEYCAP_MODIFIER
                                  ? "Sticky: tap it, then the key" : g_caps[hit].label);
    return TRUE;
}

static GtkWidget *area_new(GtkDrawingAreaDrawFunc draw, GCallback begin, GCallback update,
                           GCallback end, GCallback cancel, GCallback tooltip, int buttons)
{
    GtkWidget *area = gtk_drawing_area_new();
    GtkGesture *drag = gtk_gesture_drag_new();

    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area), draw, NULL, NULL);
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER);
    /* Not focusable: clicking must not steal the focus from typing. */
    gtk_widget_set_focusable(area, FALSE);
    gtk_widget_set_has_tooltip(area, TRUE);
    g_signal_connect(area, "query-tooltip", tooltip, NULL);
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), buttons);
    g_signal_connect(drag, "drag-begin", begin, NULL);
    g_signal_connect(drag, "drag-update", update, NULL);
    g_signal_connect(drag, "drag-end", end, NULL);
    g_signal_connect(drag, "cancel", cancel, NULL);
    gtk_widget_add_controller(area, GTK_EVENT_CONTROLLER(drag));
    return area;
}

/* ---- building --------------------------------------------------------------------- */

static void size_areas(void)
{
    const int ctl_wide = (int)(g_ctl_wide * UNIT) + 2 * CASE_PAD;
    const int key_wide = (int)(g_cap_wide * KEY_UNIT) + 2 * KEY_PAD;
    const gboolean keys = gtk_check_button_get_active(GTK_CHECK_BUTTON(g_keyboard_check));

    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_controls_area), ctl_wide);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_controls_area),
                                        (int)(g_ctl_high * UNIT) + ORIGIN_TOP + CASE_PAD);
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(g_keys_area), key_wide);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(g_keys_area),
                                        (int)(g_cap_high * KEY_UNIT) + 2 * KEY_PAD);
    gtk_widget_set_visible(g_keys_area, keys);
    /* the rows below take the wider of the two */
    gtk_widget_set_size_request(g_root, keys ? MAX(ctl_wide, key_wide) : ctl_wide, -1);
}

/* (Re)fetch the layouts for the machine running now. Whatever the mouse holds
 * and the keyboard latches go with the old parts. */
static void rebuild(void)
{
    mouse_release();
    centre_sticks();
    cocosession_keyboard_release(g_session);
    g_machine = cocosession_machine(g_session);
    g_ctl = cocosession_controls_layout(g_machine, &g_ctl_count, &g_ctl_wide, &g_ctl_high);
    g_caps = cocosession_keyboard_layout(g_machine, &g_cap_count, &g_cap_wide, &g_cap_high);
    if (g_cap_count > MAX_CAPS)
        g_cap_count = MAX_CAPS;
    g_press_ctl = g_press_cap = -1;
    memset(g_cap_bits, 0, sizeof g_cap_bits);
    memset(g_knob, 0, sizeof g_knob);
    memset(&g_snap, 0, sizeof g_snap);
    adw_window_title_set_subtitle(g_title, cocosession_machine_name(g_machine));
    size_areas();
    refresh_held();
    refresh_all();
}

static void refresh_all(void)
{
    if (g_controls_area)
        gtk_widget_queue_draw(g_controls_area);
    if (g_keys_area) {
        /* inert and dimmed while Map is on */
        gtk_widget_queue_draw(g_keys_area);
        gtk_widget_set_sensitive(g_keys_area, g_map_state == -2);
        gtk_widget_set_opacity(g_keys_area, g_map_state == -2 ? 1.0 : 0.4);
    }
    if (g_clear_button)
        gtk_widget_set_sensitive(g_clear_button, g_map_state >= 0);
    if (g_stop_button) {
        char bound[40], text[96];
        const gboolean picked = g_map_state >= 0 && g_map_state == STOP_TARGET;
        bound_text(STOP_TARGET, bound, sizeof bound);
        if (bound[0])
            g_snprintf(text, sizeof text, "Stop in Debugger  \xc2\xb7  %s", bound);
        else
            g_snprintf(text, sizeof text, "Stop in Debugger");
        gtk_button_set_label(GTK_BUTTON(g_stop_button), text);
        if (picked)
            gtk_widget_add_css_class(g_stop_button, "coco-accent");
        else
            gtk_widget_remove_css_class(g_stop_button, "coco-accent");
        gtk_widget_set_opacity(g_stop_button, g_map_state >= 0 && !picked ? 0.4 : 1.0);
    }
}

/* What the machine sees held, painted in the accent. */
static gboolean held_tick(gpointer data)
{
    int port;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    /* Hold stick: the stick stays where it was left */
    for (port = 0; port < COCOSESSION_JOYSTICK_PORTS; port++)
        if (g_holding[port])
            cocosession_pointer_set(g_session, port, g_hold_x[port], g_hold_y[port], 0);
    refresh_held();
    return G_SOURCE_CONTINUE;
}

/* ---- gamepads ------------------------------------------------------------- */

static void refresh_pad_ports(void)
{
    int idx;
    for (idx = 0; idx < g_pad_rows; idx++) {
        const int effective = cocosession_gamepad_effective_port(g_session, idx);
        char text[40];
        if (effective == 0) g_snprintf(text, sizeof text, "drives the right joystick");
        else if (effective > 0) g_snprintf(text, sizeof text, "drives the left joystick");
        else g_snprintf(text, sizeof text, "drives nothing");
        if (strcmp(gtk_label_get_text(GTK_LABEL(g_pad_port[idx])), text) != 0)
            gtk_label_set_text(GTK_LABEL(g_pad_port[idx]), text);
    }
}

static void on_assign_changed(GObject *object, GParamSpec *pspec, gpointer data)
{
    const int idx = GPOINTER_TO_INT(data);
    const guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
    (void)pspec;
    /* 0 = automatic (connection order), 1 = the right joystick, 2 = the left */
    cocosession_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
    refresh_pad_ports();
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name, the joystick it drives and the one it is set to. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Right joystick", "Left joystick", NULL };
    GtkWidget *child;
    int count, idx;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);
    g_pad_rows = 0;

    count = cocosession_gamepad_count(g_session);
    if (count == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0);
        gtk_widget_set_size_request(none, -1, PAD_ROW_HGT);
        gtk_widget_set_tooltip_text(none, "Plug one in: it is picked up as it appears");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (idx = 0; idx < count && idx < MAX_PADS; idx++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *drop, *eff;
        char text[128];
        const int assign = cocosession_gamepad_assignment(g_session, idx);

        cocosession_gamepad_name(g_session, idx, text, sizeof text);
        gtk_widget_set_size_request(row, -1, PAD_ROW_HGT);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(name), 1);
        gtk_widget_set_tooltip_text(name, text);
        eff = gtk_label_new("");
        gtk_widget_add_css_class(eff, "dim-label");
        drop = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop),
                                   assign < 0 || assign > 1 ? 0 : (guint)assign + 1);
        gtk_widget_set_focus_on_click(drop, FALSE);
        gtk_widget_set_valign(drop, GTK_ALIGN_CENTER);
        g_signal_connect(drop, "notify::selected", G_CALLBACK(on_assign_changed),
                         GINT_TO_POINTER(idx));
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), drop);
        gtk_box_append(GTK_BOX(g_pad_box), row);
        g_pad_port[g_pad_rows++] = eff;
    }
    refresh_pad_ports();
}

/* Hot-plugged pads, auto-assignment changes, and the machine changed in
 * Preferences. */
static gboolean pad_tick(gpointer data)
{
    unsigned gen;
    (void)data;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    gen = cocosession_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    } else {
        refresh_pad_ports();
    }
    if (cocosession_machine(g_session) != g_machine)
        rebuild();
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean modifier_keyval(guint keyval)
{
    return (keyval >= GDK_KEY_Shift_L && keyval <= GDK_KEY_Hyper_R) ||
           keyval == GDK_KEY_ISO_Level3_Shift || keyval == GDK_KEY_Mode_switch;
}

static gboolean on_key(guint keyval, guint code, GdkModifierType state, gboolean down)
{
    /* the keys the window and the app keep, which Map never takes */
    const gboolean app_key = (keyval >= GDK_KEY_F10 && keyval <= GDK_KEY_F12) ||
                             ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK));
    /* an open drop-down (a gamepad's joystick) has the arrows and Return */
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
    if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
        return FALSE;

    if (g_swallow_code && code == g_swallow_code) {
        if (!down) g_swallow_code = 0;
        return TRUE;
    }
    /* Map arms and waits: typing is not typed into the machine. A release
     * still goes through, so a key pressed before Map was armed is let go. */
    if (g_map_state != -2 && down && !app_key) {
        guint32 bindable;
        char stolen[128], name[32];
        if (keyval == GDK_KEY_Escape) {
            g_swallow_code = code;
            set_map_state(g_map_state >= 0 ? -1 : -2);
            return TRUE;
        }
        if (g_map_state < 0 || modifier_keyval(keyval))
            return TRUE;          /* armed, no target yet: typing is not typed */
        bindable = cocosession_bindable_key(keyval, gdk_keyval_to_unicode(keyval));
        if (!bindable) {
            gtk_label_set_text(GTK_LABEL(g_map_hint), "That key cannot be bound");
            return TRUE;
        }
        cocosession_binding_set_key(g_session, g_map_state, bindable, stolen, sizeof stolen);
        cocosession_keysym_name(bindable, name, sizeof name);
        g_swallow_code = code;
        set_map_state(-1);        /* stay armed: remapping several in a row is normal */
        say_bound(name, stolen);
        return TRUE;
    }
    /* Otherwise exactly what the main window does with it. */
    if (!g_parent)
        return FALSE;
    return coco_window_key(COCO_WINDOW(g_parent), keyval, state, down);
}

static gboolean on_key_pressed(GtkEventControllerKey *controller, guint keyval, guint code,
                               GdkModifierType state, gpointer data)
{
    (void)controller; (void)data;
    return on_key(keyval, code, state, TRUE);
}

static void on_key_released(GtkEventControllerKey *controller, guint keyval, guint code,
                            GdkModifierType state, gpointer data)
{
    (void)controller; (void)data;
    on_key(keyval, code, state, FALSE);
}

/* Keys typed here went through the main window, so a key held into a focus
 * change must be let go there too (and the mouse's, here). */
static void on_focus_leave(GtkEventControllerFocus *controller, gpointer data)
{
    (void)controller; (void)data;
    mouse_release();
    if (g_parent)
        coco_window_release_keys(COCO_WINDOW(g_parent));
    g_swallow_code = 0;     /* its release went elsewhere */
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    cocosession_bindings_reset(g_session);
    mouse_release();
    centre_sticks();
    cocosession_keyboard_release(g_session);
    gtk_label_set_text(GTK_LABEL(g_map_hint), "Bindings back to their defaults");
    refresh_held();
    refresh_all();
}

/* Takes the picked control's key and gamepad button away. */
static void on_clear_clicked(GtkButton *button, gpointer data)
{
    char stolen[16];
    (void)button; (void)data;
    if (g_map_state < 0)
        return;
    cocosession_binding_set_key(g_session, g_map_state, 0, stolen, sizeof stolen);
    cocosession_binding_set_button(g_session, g_map_state, COCO_PAD_BTN_NONE, stolen,
                                   sizeof stolen);
    gtk_label_set_text(GTK_LABEL(g_map_hint),
                       "Cleared. Press a key or a gamepad button to bind it again");
    refresh_all();
}

static void on_stop_clicked(GtkButton *button, gpointer data)
{
    (void)button; (void)data;
    if (g_map_state != -2)
        set_map_state(STOP_TARGET);
    else
        run_sysaction(COCO_SYSACT_DEBUG_STOP);
}

static void on_show_bindings(GtkCheckButton *button, gpointer data)
{
    (void)data;
    cocosession_set_int(g_session, "controllers_show_bindings",
                        gtk_check_button_get_active(button) ? 1 : 0);
    refresh_all();
}

static void on_keyboard_toggled(GtkCheckButton *button, gpointer data)
{
    (void)data;
    cocosession_set_int(g_session, "controllers_keyboard",
                        gtk_check_button_get_active(button) ? 1 : 0);
    if (!gtk_check_button_get_active(button)) {
        cap_release();
        cocosession_keyboard_release(g_session);
    }
    size_areas();
    refresh_held();
    refresh_all();
}

static void on_hold_toggled(GtkCheckButton *button, gpointer data)
{
    (void)data;
    cocosession_set_int(g_session, "joystick_hold", gtk_check_button_get_active(button) ? 1 : 0);
    if (!gtk_check_button_get_active(button)) {
        /* a stick left standing goes back to the centre */
        if (!g_drag.active)
            centre_sticks();
    }
    refresh_held();
}

static void hide_window(void)
{
    set_map_state(-2);
    mouse_release();
    centre_sticks();
    cocosession_keyboard_release(g_session);
    refresh_held();
    g_swallow_code = 0;
    gtk_widget_set_visible(g_window, FALSE);
}

static gboolean on_close(GtkWindow *window, gpointer data)
{
    (void)window; (void)data;
    hide_window();
    return TRUE;
}

/* The refresh timers run only while the window is on screen: closing only
 * hides it, and a hidden window has nobody to update. */
static void on_window_map(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (!g_held_timer) g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer) g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_held_timer = g_pad_timer = 0;
}

static void on_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; (void)data;
    if (g_capture_timer) g_source_remove(g_capture_timer);
    if (g_held_timer) g_source_remove(g_held_timer);
    if (g_pad_timer) g_source_remove(g_pad_timer);
    g_capture_timer = g_held_timer = g_pad_timer = 0;
    g_window = NULL;
    g_controls_area = g_keys_area = g_root = g_pad_box = NULL;
    g_map_button = g_clear_button = g_stop_button = g_map_hint = NULL;
    g_show_bindings = g_keyboard_check = g_hold_check = NULL;
    g_pad_rows = 0;
    g_machine = -1;
    g_map_state = -2;
}

static void install_css(void)
{
    char css[512];
    if (g_css)
        return;
    g_snprintf(css, sizeof css,
               ".coco-accent { background: #%06x; color: #%06x; }\n"
               ".coco-accent:hover { background: shade(#%06x, 1.12); }\n",
               COL_ACCENT, COL_ACCENT_INK, COL_ACCENT);
    g_css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(g_css, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(g_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static GtkWidget *plain_button(const char *label, GCallback handler)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gtk_widget_set_focus_on_click(button, FALSE);
    gtk_widget_set_focusable(button, FALSE);
    g_signal_connect(button, "clicked", handler, NULL);
    return button;
}

static GtkWidget *plain_check(const char *label, gboolean active, GCallback handler)
{
    GtkWidget *check = gtk_check_button_new_with_label(label);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), active);
    gtk_widget_set_focus_on_click(check, FALSE);
    gtk_widget_set_focusable(check, FALSE);
    g_signal_connect(check, "toggled", handler, NULL);
    return check;
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *toolbar, *header, *padscroll, *padtitle, *maprow, *checkrow;
    GtkEventController *keys, *focus;

    install_css();
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Controllers");
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
        /* Same app-id as the main window, so the shell groups and icons it. */
        gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    }
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    /* min == max: Wayland compositors float it */
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);
    g_signal_connect(g_window, "destroy", G_CALLBACK(on_destroy), NULL);

    g_controls_area = area_new(draw_controls, G_CALLBACK(on_controls_begin),
                               G_CALLBACK(on_controls_update), G_CALLBACK(on_controls_end),
                               G_CALLBACK(on_controls_cancel), G_CALLBACK(on_controls_tooltip), 0);
    g_keys_area = area_new(draw_keys, G_CALLBACK(on_keys_begin), G_CALLBACK(on_keys_update),
                           G_CALLBACK(on_keys_end), G_CALLBACK(on_keys_cancel),
                           G_CALLBACK(on_keys_tooltip), GDK_BUTTON_PRIMARY);

    g_root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(g_root, 12);
    gtk_widget_set_margin_bottom(g_root, 12);
    gtk_widget_set_margin_start(g_root, 12);
    gtk_widget_set_margin_end(g_root, 12);
    gtk_box_append(GTK_BOX(g_root), g_controls_area);
    gtk_box_append(GTK_BOX(g_root), g_keys_area);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = plain_button("Map", G_CALLBACK(on_map_clicked));
    gtk_widget_set_size_request(g_map_button, 72, -1);
    g_clear_button = plain_button("Clear", G_CALLBACK(on_clear_clicked));
    gtk_widget_set_tooltip_text(g_clear_button, "Take the picked control's key and gamepad button away");
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), g_clear_button);
    gtk_box_append(GTK_BOX(maprow), plain_button("Defaults", G_CALLBACK(on_defaults_clicked)));
    g_stop_button = plain_button("Stop in Debugger", G_CALLBACK(on_stop_clicked));
    gtk_widget_set_hexpand(g_stop_button, TRUE);
    gtk_widget_set_halign(g_stop_button, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(maprow), g_stop_button);
    gtk_box_append(GTK_BOX(g_root), maprow);

    checkrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    g_show_bindings = plain_check("Show bindings", show_bindings_saved(),
                                  G_CALLBACK(on_show_bindings));
    g_keyboard_check = plain_check("Keyboard",
        cocosession_get_int(g_session, "controllers_keyboard", 0) != 0,
        G_CALLBACK(on_keyboard_toggled));
    gtk_widget_set_tooltip_text(g_keyboard_check, "Show the machine's keyboard under the joysticks");
    g_hold_check = plain_check("Hold stick",
        cocosession_get_int(g_session, "joystick_hold", 0) != 0, G_CALLBACK(on_hold_toggled));
    gtk_widget_set_tooltip_text(g_hold_check,
        "Leave the stick where you drop it, as the Tandy ones do (click the centre to recentre)");
    gtk_box_append(GTK_BOX(checkrow), g_show_bindings);
    gtk_box_append(GTK_BOX(checkrow), g_keyboard_check);
    gtk_box_append(GTK_BOX(checkrow), g_hold_check);
    gtk_box_append(GTK_BOX(g_root), checkrow);

    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    /* a long hint wraps onto two fixed lines: it must neither widen nor
     * resize the window */
    gtk_label_set_wrap(GTK_LABEL(g_map_hint), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(g_map_hint), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(g_map_hint), 2);
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(g_map_hint), 1);
    {
        int linewide = 0, linehigh = 0;
        PangoLayout *layout = gtk_widget_create_pango_layout(g_map_hint, "A\nA");
        pango_layout_get_pixel_size(layout, &linewide, &linehigh);
        g_object_unref(layout);
        gtk_widget_set_size_request(g_map_hint, -1, linehigh);
    }
    gtk_widget_set_valign(g_map_hint, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(g_root), g_map_hint);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_label_set_xalign(GTK_LABEL(padtitle), 0);
    gtk_box_append(GTK_BOX(g_root), padtitle);
    /* the pad rows: a fixed-height area, so the window never resizes */
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, PAD_GAP);
    padscroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(padscroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(padscroll), FALSE);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_HGT + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(padscroll),
        PAD_ROWS * PAD_ROW_HGT + (PAD_ROWS - 1) * PAD_GAP);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(padscroll), g_pad_box);
    gtk_box_append(GTK_BOX(g_root), padscroll);

    header = adw_header_bar_new();
    g_title = ADW_WINDOW_TITLE(adw_window_title_new("Controllers", ""));
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), GTK_WIDGET(g_title));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), coco_help_button("controllers"));
    coco_help_set_context(GTK_WINDOW(g_window), "controllers");
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), g_root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    g_machine = -1;
    set_map_state(-2);
    rebuild();
    g_pad_generation = (unsigned)-1;
    refresh_pads();
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
}

static void ensure_window(GtkWindow *parent, cocosession *session)
{
    g_session = session;
    if (parent)
        g_parent = parent;
    if (!g_window)
        build_window(g_parent);
}

static void present_window(void)
{
    if (cocosession_machine(g_session) != g_machine)
        rebuild();
    g_pad_generation = cocosession_gamepad_generation(g_session);
    refresh_pads();
    refresh_held();
    gtk_window_present(GTK_WINDOW(g_window));
}

void coco_controllers_show(GtkWindow *parent, cocosession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        gtk_window_present(GTK_WINDOW(g_window));
    else
        present_window();
}

void coco_controllers_toggle(GtkWindow *parent, cocosession *session)
{
    ensure_window(parent, session);
    if (gtk_widget_get_visible(g_window))
        hide_window();
    else
        present_window();
}

void coco_controllers_hide(void)
{
    if (g_window && gtk_widget_get_visible(g_window))
        hide_window();
}

gboolean coco_controllers_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
