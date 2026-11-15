/*
 * The debugger's Video tab (GTK4/cairo).
 *
 * Everything that decides what to show lives in the core (cocovidtab.h): this file is
 * widgets. The picture is drawn from the controller's cached buffer (0xFFRRGGBB, drawn as
 * CAIRO_FORMAT_RGB24 with a nearest filter), so a mouse move redraws only the marks. Only the
 * section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <string.h>

#include "cocovidcap.h"
#include "cocovidtab.h"

#define NSWATCH 16          /* a GIME palette has 16 slots; the VDG shows 12 colours */
#define MAX_OVERLAYS 16

typedef struct {
    cocovid_tab *tab;
    GtkDropDown *view, *zoom;
    GtkWidget *brk_top, *brk_pix;
    GtkCheckButton *overlay_box[MAX_OVERLAYS];
    int nov;
    GtkLabel *status, *note;
    GtkDrawingArea *pic;
    GtkWidget *pic_scroll;
    int vp_w, vp_h;
    GtkNotebook *sections;
    GtkLabel *pixel_text, *pixel_note;
    GtkTextView *regs_text, *lines_text;
    char *regs_sig, *lines_sig;
    GtkDrawingArea *swatch_box[NSWATCH];
    GtkLabel *swatch_lbl[NSWATCH];
    GtkLabel *timing_text;
    GtkProgressBar *beam;
    gboolean updating, primed, was_mapped;
} Vid;

static GtkWidget *mono_label(void)
{
    GtkWidget *label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_add_css_class(label, "monospace");
    return label;
}

static GtkWidget *padded_box(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    return box;
}

static void set_rgb(cairo_t *context, uint32_t rgb)
{
    cairo_set_source_rgb(context, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
}

/* ---- the picture ---------------------------------------------------------- */

static void draw_message(cairo_t *context, int width, int height, const char *msg)
{
    cairo_text_extents_t ext;
    cairo_set_source_rgb(context, 0.65, 0.65, 0.68);
    cairo_select_font_face(context, "sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(context, 14);
    cairo_text_extents(context, msg, &ext);
    cairo_move_to(context, (width - ext.width) / 2 - ext.x_bearing, (height - ext.height) / 2 - ext.y_bearing);
    cairo_show_text(context, msg);
}

/* A pixel can be one device pixel wide: outline it from outside, dark under light. */
static void draw_mark(cairo_t *context, const int result[4], double red, double green, double blue)
{
    cairo_set_line_width(context, 3);
    cairo_set_source_rgba(context, 0, 0, 0, 0.85);
    cairo_rectangle(context, result[0] - 1.5, result[1] - 1.5, result[2] + 3, result[3] + 3);
    cairo_stroke(context);
    cairo_set_line_width(context, 1);
    cairo_set_source_rgb(context, red, green, blue);
    cairo_rectangle(context, result[0] - 1.5, result[1] - 1.5, result[2] + 3, result[3] + 3);
    cairo_stroke(context);
}

static void draw_pic(GtkDrawingArea *area, cairo_t *context, int width, int height, gpointer user_data)
{
    Vid *video = user_data;
    int picture_wide, picture_high, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *pixels;
    (void)area;
    cairo_set_source_rgb(context, 0.07, 0.07, 0.09);
    cairo_paint(context);
    pixels = cocovid_tab_picture(video->tab, &picture_wide, &picture_high);
    if (!pixels) {
        const char *why = cocovid_tab_why(video->tab);
        draw_message(context, width, height, why && *why ? why : "Nothing to show");
        return;
    }
    {
        cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)pixels, CAIRO_FORMAT_RGB24,
                                                                 picture_wide, picture_high, picture_wide * 4);
        cairo_set_source_surface(context, surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(context), CAIRO_FILTER_NEAREST);
        cairo_paint(context);
        cairo_surface_destroy(surface);
    }
    marks = cocovid_tab_marks(video->tab, hover, pin);
    if (marks & 1) draw_mark(context, hover, 1, 1, 1);
    if (marks & 2) draw_mark(context, pin, 1.0, 0.85, 0.1);
}

static void update_pixel_section(Vid *video)
{
    const cocovid_pixel *pixel = cocovid_tab_pixel(video->tab);
    const gboolean can = cocovid_tab_can_break(video->tab) != 0;
    gtk_widget_set_sensitive(video->brk_top, can);
    gtk_widget_set_sensitive(video->brk_pix, can);
    gtk_label_set_text(video->pixel_text, pixel ? pixel->text
        : "Move over the picture to inspect a pixel.\nClick to pin it (click again to unpin);\n"
          "double-click to break on writes to it.");
    {   /* the swatch the inspected pixel uses is outlined */
        int index;
        for (index = 0; index < NSWATCH; index++)
            gtk_widget_queue_draw(GTK_WIDGET(video->swatch_box[index]));
    }
}

static void do_break_pixel(Vid *video)
{
    const cocovid_pixel *pixel = cocovid_tab_pixel(video->tab);
    int breakpoint_id = cocovid_tab_break_on_pixel(video->tab);
    char buf[160];
    if (breakpoint_id >= 0 && pixel)
        g_snprintf(buf, sizeof buf, "Break on writes to $%04X (the byte behind the pixel)", pixel->addr);
    else
        g_snprintf(buf, sizeof buf, "Nothing to break on (inspect a pixel drawn from console RAM)");
    gtk_label_set_text(video->note, buf);
    gtk_label_set_text(video->pixel_note, buf);
}

static void on_motion(GtkEventControllerMotion *controller, double position_x, double position_y, gpointer user_data)
{
    Vid *video = user_data;
    (void)controller;
    if (cocovid_tab_hover(video->tab, (int)position_x, (int)position_y)) {
        gtk_widget_queue_draw(GTK_WIDGET(video->pic));
        update_pixel_section(video);
    }
}

static void on_leave(GtkEventControllerMotion *controller, gpointer user_data)
{
    Vid *video = user_data;
    (void)controller;
    if (cocovid_tab_hover(video->tab, -1, -1)) {
        gtk_widget_queue_draw(GTK_WIDGET(video->pic));
        update_pixel_section(video);
    }
}

static void on_click(GtkGestureClick *gesture, int count, double position_x, double position_y, gpointer user_data)
{
    Vid *video = user_data;
    (void)gesture;
    if (count == 1) {
        cocovid_tab_pin(video->tab, (int)position_x, (int)position_y);
    } else if (count == 2) {
        /* the first click of the pair toggled the pin; the second restores it */
        cocovid_tab_hover(video->tab, (int)position_x, (int)position_y);
        cocovid_tab_pin(video->tab, (int)position_x, (int)position_y);
        do_break_pixel(video);
    }
    gtk_widget_queue_draw(GTK_WIDGET(video->pic));
    update_pixel_section(video);
}

/* ---- text tables ------------------------------------------------------------- */

/* Replace a text view's contents when they (or the highlighted rows) changed;
 * the scroll position stays put. hl[i] != 0 highlights line i. */
static void put_table(GtkTextView *view_widget, char **sigp, GString *text, const GString *highlight)
{
    GString *sig = g_string_new(NULL);
    g_string_append_len(sig, highlight->str, (gssize)highlight->len);
    g_string_append_c(sig, '|');
    g_string_append_len(sig, text->str, (gssize)text->len);
    if (!*sigp || strcmp(*sigp, sig->str)) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(view_widget);
        GtkAdjustment *adj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(view_widget));
        double pos = adj ? gtk_adjustment_get_value(adj) : 0;
        char *clean = g_utf8_make_valid(text->str, -1);
        gsize index;
        gtk_text_buffer_set_text(buffer, clean, -1);
        g_free(clean);
        for (index = 0; index < highlight->len; index++) {
            if (highlight->str[index] == '1') {
                GtkTextIter iter_start, iter_end;
                gtk_text_buffer_get_iter_at_line(buffer, &iter_start, (int)index);
                iter_end = iter_start;
                gtk_text_iter_forward_to_line_end(&iter_end);
                gtk_text_buffer_apply_tag_by_name(buffer, "changed", &iter_start, &iter_end);
            }
        }
        if (adj) gtk_adjustment_set_value(adj, pos);
        g_free(*sigp);
        *sigp = g_strdup(sig->str);
    }
    g_string_free(sig, TRUE);
}

static void fill_regs(Vid *video)
{
    const cocovid_reg *regs;
    int count = cocovid_tab_regs(video->tab, &regs), index;
    GString *text = g_string_new(NULL), *highlight = g_string_new(NULL);
    g_string_append(text, "Register   Value  Meaning\n");
    g_string_append_c(highlight, '0');
    for (index = 0; index < count; index++) {
        g_string_append_printf(text, "%-10s %-6s %s\n", regs[index].name, regs[index].value, regs[index].meaning);
        g_string_append_c(highlight, regs[index].age < COCOVID_REG_FADE ? '1' : '0');
    }
    if (!count) g_string_append(text, "The machine is not running.\n");
    put_table(video->regs_text, &video->regs_sig, text, highlight);
    g_string_free(text, TRUE);
    g_string_free(highlight, TRUE);
}

static void fill_lines(Vid *video)
{
    const cocovid_line_row *rows;
    int count = cocovid_tab_lines(video->tab, &rows), index;
    GString *text = g_string_new(NULL), *highlight = g_string_new(NULL);
    g_string_append(text, "Line  Chip  Addr    Bytes Mode                              First bytes                Notes\n");
    g_string_append_c(highlight, '0');
    for (index = 0; index < count; index++) {
        int slot;
        g_string_append_printf(text, "%4d%c %4d  ", rows[index].row, rows[index].fresh ? ' ' : '~', rows[index].chip_line);
        if (rows[index].valid) {
            g_string_append_printf(text, "%06X  %4d  %-34.34s ", rows[index].addr, rows[index].nbytes, rows[index].mode);
            for (slot = 0; slot < rows[index].nbytes && slot < 8; slot++)
                g_string_append_printf(text, "%02X ", rows[index].data[slot]);
            for (; slot < 8; slot++)
                g_string_append(text, "   ");
        } else {
            g_string_append(text, "------  ----  (no record)                        ");
            g_string_append(text, "                        ");
        }
        g_string_append_printf(text, " %s%s%s%s%s",
                               rows[index].changed_mask & COCOVID_LC_ADDR ? "addr " : "",
                               rows[index].changed_mask & COCOVID_LC_MODE ? "mode " : "",
                               rows[index].changed_mask & COCOVID_LC_PALETTE ? "palette " : "",
                               rows[index].changed_mask & COCOVID_LC_SIZE ? "size " : "",
                               rows[index].changed_mask & COCOVID_LC_MID ? "mid-line " : "");
        g_string_append_c(text, '\n');
        g_string_append_c(highlight, rows[index].changed_mask != 0 ? '1' : '0');
    }
    if (!count) g_string_append(text, "The machine is not running.\n");
    put_table(video->lines_text, &video->lines_sig, text, highlight);
    g_string_free(text, TRUE);
    g_string_free(highlight, TRUE);
}

/* ---- colours ------------------------------------------------------------------ */

static void draw_swatch(GtkDrawingArea *area, cairo_t *context, int width, int height, gpointer user_data)
{
    Vid *video = user_data;
    const cocovid_swatch *swatches;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "idx"));
    (void)width; (void)height;
    if (cocovid_tab_swatches(video->tab, &swatches) <= index) return;
    set_rgb(context, swatches[index].rgb);
    cairo_rectangle(context, 0, 0, 22, 22);
    cairo_fill(context);
    cairo_set_source_rgba(context, 0.5, 0.5, 0.5, 0.8);
    cairo_set_line_width(context, 1);
    cairo_rectangle(context, 0.5, 0.5, 21, 21);
    cairo_stroke(context);
    if (swatches[index].inspected) {      /* the inspected pixel uses this colour */
        cairo_set_source_rgb(context, 1, 0.85, 0.1);
        cairo_set_line_width(context, 2);
        cairo_rectangle(context, 1, 1, 20, 20);
        cairo_stroke(context);
    }
}

static void fill_colours(Vid *video)
{
    const cocovid_swatch *swatches;
    int count = cocovid_tab_swatches(video->tab, &swatches), index;
    for (index = 0; index < NSWATCH; index++) {
        char buf[160];
        if (index < count)
            g_snprintf(buf, sizeof buf, "%-8s #%06X  %8ld px  %s", swatches[index].name,
                       swatches[index].rgb & 0xffffff, swatches[index].uses, swatches[index].detail);
        else
            buf[0] = '\0';
        gtk_label_set_text(video->swatch_lbl[index], buf);
        gtk_widget_set_visible(GTK_WIDGET(video->swatch_box[index]), index < count);
        gtk_widget_queue_draw(GTK_WIDGET(video->swatch_box[index]));
    }
}

static void fill_timing(Vid *video)
{
    const char *const *lines;
    int count = cocovid_tab_timing(video->tab, &lines), index;
    GString *text = g_string_new(NULL);
    for (index = 0; index < count; index++) {
        const char *tab = strchr(lines[index], '\t');
        if (tab) g_string_append_printf(text, "%-14.*s %s\n", (int)(tab - lines[index]), lines[index], tab + 1);
        else g_string_append_printf(text, "%s\n", lines[index]);
    }
    gtk_label_set_text(video->timing_text, text->len ? text->str : "The machine is not running.");
    g_string_free(text, TRUE);
    gtk_progress_bar_set_fraction(video->beam, cocovid_tab_beam_permille(video->tab) / 1000.0);
}

/* ---- refresh ------------------------------------------------------------------- */

static void fill_section(Vid *video, int sec)
{
    switch (sec) {
    case COCOVID_S_REGISTERS: fill_regs(video); break;
    case COCOVID_S_COLOURS: fill_colours(video); break;
    case COCOVID_S_LINES: fill_lines(video); break;
    case COCOVID_S_TIMING: fill_timing(video); break;
    default: update_pixel_section(video); break;
    }
}

static void refresh_view(Vid *video, unsigned what)
{
    if (what & (COCOVID_TAB_PICTURE | COCOVID_TAB_OFF)) {
        int picture_wide = 0, picture_high = 0;
        if (cocovid_tab_picture(video->tab, &picture_wide, &picture_high)) {
            gtk_drawing_area_set_content_width(video->pic, picture_wide);
            gtk_drawing_area_set_content_height(video->pic, picture_high);
        } else {
            gtk_drawing_area_set_content_width(video->pic, video->vp_w > 4 ? video->vp_w : 480);
            gtk_drawing_area_set_content_height(video->pic, video->vp_h > 4 ? video->vp_h : 300);
        }
        gtk_widget_queue_draw(GTK_WIDGET(video->pic));
    }
    if (what & COCOVID_TAB_TABLES) {
        gtk_label_set_text(video->status, cocovid_tab_status(video->tab));
        fill_section(video, gtk_notebook_get_current_page(video->sections));
        if (gtk_notebook_get_current_page(video->sections) != COCOVID_S_PIXEL)
            update_pixel_section(video);    /* the Break on writes buttons follow the pixel */
    }
}

static void sync_viewport(Vid *video)
{
    int width = gtk_widget_get_width(video->pic_scroll), height = gtk_widget_get_height(video->pic_scroll);
    if (width > 4 && height > 4 && (width != video->vp_w || height != video->vp_h)) {
        video->vp_w = width;
        video->vp_h = height;
        cocovid_tab_set_viewport(video->tab, width - 2, height - 2);
    }
}

void dbg_video_refresh(GtkWidget *page, gboolean force)
{
    Vid *video = g_object_get_data(G_OBJECT(page), "vid");
    gboolean mapped;
    unsigned what;
    if (!video) return;
    mapped = gtk_widget_get_mapped(page);
    if (mapped && !video->was_mapped) force = TRUE;      /* just shown */
    video->was_mapped = mapped;
    if (!video->primed) force = TRUE;
    video->primed = TRUE;
    sync_viewport(video);
    what = cocovid_tab_refresh(video->tab, force);
    if (force) what |= COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES;
    refresh_view(video, what);
}

/* The picture's room changed (window resized, split dragged): Fit follows it. */
static gboolean on_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer user_data)
{
    Vid *video = user_data;
    int picture_wide = gtk_widget_get_width(widget), picture_high = gtk_widget_get_height(widget);
    (void)clock;
    if (picture_wide > 4 && picture_high > 4 && (picture_wide != video->vp_w || picture_high != video->vp_h)) {
        sync_viewport(video);
        if (video->primed && cocovid_tab_zoom(video->tab) == 0)
            refresh_view(video, cocovid_tab_refresh(video->tab, TRUE) | COCOVID_TAB_PICTURE);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- controls ------------------------------------------------------------------- */

static void refresh_all(Vid *video)
{
    refresh_view(video, cocovid_tab_refresh(video->tab, TRUE) | COCOVID_TAB_PICTURE | COCOVID_TAB_TABLES);
}

static void on_view(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    Vid *video = user_data;
    (void)object; (void)pspec;
    if (video->updating) return;
    cocovid_tab_set_view(video->tab, (int)gtk_drop_down_get_selected(video->view));
    refresh_all(video);
}

static void on_zoom(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    Vid *video = user_data;
    int count;
    const int *zoom = cocovid_zooms(&count);
    guint index = gtk_drop_down_get_selected(video->zoom);
    (void)object; (void)pspec;
    if (video->updating || (int)index >= count) return;
    sync_viewport(video);
    cocovid_tab_set_zoom(video->tab, zoom[index]);
    refresh_all(video);
}

static void on_overlay(GtkCheckButton *button, gpointer user_data)
{
    Vid *video = user_data;
    unsigned mask = 0;
    int count, index;
    const cocovid_overlay_info *info = cocovid_overlays(&count);
    (void)button;
    if (video->updating) return;
    for (index = 0; index < count && index < video->nov; index++)
        if (gtk_check_button_get_active(video->overlay_box[index])) mask |= info[index].bit;
    cocovid_tab_set_overlays(video->tab, mask);
    refresh_all(video);
}

static void on_unpin(GtkButton *button, gpointer user_data)
{
    Vid *video = user_data;
    (void)button;
    cocovid_tab_unpin(video->tab);
    gtk_widget_queue_draw(GTK_WIDGET(video->pic));
    update_pixel_section(video);
}

static void on_break(GtkButton *button, gpointer user_data)
{
    (void)button;
    do_break_pixel(user_data);
}

static void on_section(GtkNotebook *notebook, GtkWidget *page, guint count, gpointer user_data)
{
    Vid *video = user_data;
    (void)notebook; (void)page;
    fill_section(video, (int)count);
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
    Vid *video = user_data;
    (void)widget;
    cocovid_tab_free(video->tab);
    g_free(video->regs_sig);
    g_free(video->lines_sig);
    g_free(video);
}

GtkWidget *dbg_video_new(cocodebug *dbg)
{
    Vid *video = g_new0(Vid, 1);
    GtkWidget *root, *left, *bar, *right, *box, *btns, *break_button, *unpin_button, *scroll, *ovbtn, *pop, *ovbox, *grid, *brk;
    GtkEventController *motion;
    GtkGesture *click;
    GtkTextBuffer *buffer;
    const cocovid_overlay_info *overlays;
    const int *zooms;
    const char *views[COCOVID_V_COUNT + 1], *zlabels[24];
    char zbuf[24][8];
    int count, zoom_count, index;

    video->tab = cocovid_tab_new(dbg);

    root = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    g_object_set_data(G_OBJECT(root), "vid", video);
    g_signal_connect(root, "destroy", G_CALLBACK(on_destroy), video);

    /* ---- left: toolbar, status, picture */
    left = padded_box();
    bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    for (index = 0; index < COCOVID_V_COUNT; index++) views[index] = cocovid_view_name(index);
    views[COCOVID_V_COUNT] = NULL;
    video->view = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(views));
    gtk_widget_set_tooltip_text(GTK_WIDGET(video->view),
        "Display: the last finished frame, as the TV showed it.\n"
        "Decoded: the display window rebuilt from each line's own mode, bytes and palette.\n"
        "Beam: this frame above the beam, the last one dimmed below (stopped).\n"
        "Values: each pixel as a flat false colour of its colour code.");
    zooms = cocovid_zooms(&zoom_count);
    if (zoom_count > 23) zoom_count = 23;
    for (index = 0; index < zoom_count; index++) {
        if (zooms[index] == 0) g_strlcpy(zbuf[index], "Fit", sizeof zbuf[index]);
        else g_snprintf(zbuf[index], sizeof zbuf[index], "%d×", zooms[index]);
        zlabels[index] = zbuf[index];
    }
    zlabels[zoom_count] = NULL;
    video->zoom = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(zlabels));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("View"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(video->view));
    gtk_box_append(GTK_BOX(bar), gtk_label_new("Zoom"));
    gtk_box_append(GTK_BOX(bar), GTK_WIDGET(video->zoom));

    overlays = cocovid_overlays(&count);
    if (count > MAX_OVERLAYS) count = MAX_OVERLAYS;
    video->nov = count;
    ovbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    for (index = 0; index < count; index++) {
        video->overlay_box[index] = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(overlays[index].label));
        g_signal_connect(video->overlay_box[index], "toggled", G_CALLBACK(on_overlay), video);
        gtk_box_append(GTK_BOX(ovbox), GTK_WIDGET(video->overlay_box[index]));
    }
    pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), ovbox);
    ovbtn = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(ovbtn), "Overlays");
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(ovbtn), pop);
    gtk_box_append(GTK_BOX(bar), ovbtn);

    brk = gtk_button_new_with_label("Break on writes");
    gtk_widget_set_tooltip_text(brk, "Break when the CPU writes the byte behind the inspected pixel "
                                     "(console RAM only)");
    video->brk_top = brk;
    g_signal_connect(brk, "clicked", G_CALLBACK(on_break), video);
    gtk_box_append(GTK_BOX(bar), brk);
    gtk_box_append(GTK_BOX(left), bar);

    video->status = GTK_LABEL(mono_label());
    gtk_label_set_selectable(video->status, FALSE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(video->status));

    video->pic = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_drawing_area_set_draw_func(video->pic, draw_pic, video, NULL);
    gtk_drawing_area_set_content_width(video->pic, 480);
    gtk_drawing_area_set_content_height(video->pic, 300);
    gtk_widget_set_halign(GTK_WIDGET(video->pic), GTK_ALIGN_START);
    gtk_widget_set_valign(GTK_WIDGET(video->pic), GTK_ALIGN_START);
    motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), video);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), video);
    gtk_widget_add_controller(GTK_WIDGET(video->pic), motion);
    click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(on_click), video);
    gtk_widget_add_controller(GTK_WIDGET(video->pic), GTK_EVENT_CONTROLLER(click));
    scroll = gtk_scrolled_window_new();
    video->pic_scroll = scroll;
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(video->pic));
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_add_tick_callback(scroll, on_tick, video, NULL);
    gtk_box_append(GTK_BOX(left), scroll);

    video->note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(video->note, 0);
    gtk_label_set_wrap(video->note, TRUE);
    gtk_box_append(GTK_BOX(left), GTK_WIDGET(video->note));

    /* ---- right: the five sections, in cocovid_section_name order */
    right = gtk_notebook_new();
    video->sections = GTK_NOTEBOOK(right);
    gtk_widget_set_size_request(right, 440, -1);

    /* Pixel */
    box = padded_box();
    video->pixel_text = GTK_LABEL(mono_label());
    gtk_label_set_wrap(video->pixel_text, TRUE);
    gtk_label_set_wrap_mode(video->pixel_text, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(video->pixel_text));
    video->pixel_note = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_xalign(video->pixel_note, 0);
    gtk_label_set_wrap(video->pixel_note, TRUE);
    gtk_label_set_wrap_mode(video->pixel_note, PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(video->pixel_note));
    btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    break_button = gtk_button_new_with_label("Break on writes");
    video->brk_pix = break_button;
    unpin_button = gtk_button_new_with_label("Unpin");
    g_signal_connect(break_button, "clicked", G_CALLBACK(on_break), video);
    g_signal_connect(unpin_button, "clicked", G_CALLBACK(on_unpin), video);
    gtk_box_append(GTK_BOX(btns), break_button);
    gtk_box_append(GTK_BOX(btns), unpin_button);
    gtk_box_append(GTK_BOX(box), btns);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(video->sections, scroll, gtk_label_new(cocovid_section_name(COCOVID_S_PIXEL)));

    /* Registers */
    video->regs_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(video->regs_text, FALSE);
    gtk_text_view_set_cursor_visible(video->regs_text, FALSE);
    gtk_text_view_set_monospace(video->regs_text, TRUE);
    gtk_text_view_set_wrap_mode(video->regs_text, GTK_WRAP_WORD_CHAR);
    buffer = gtk_text_view_get_buffer(video->regs_text);
    gtk_text_buffer_create_tag(buffer, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(video->regs_text));
    gtk_notebook_append_page(video->sections, scroll, gtk_label_new(cocovid_section_name(COCOVID_S_REGISTERS)));

    /* Colours: the VDG's twelve, or the GIME's sixteen palette slots */
    box = padded_box();
    grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    for (index = 0; index < NSWATCH; index++) {
        video->swatch_box[index] = GTK_DRAWING_AREA(gtk_drawing_area_new());
        gtk_drawing_area_set_content_width(video->swatch_box[index], 22);
        gtk_drawing_area_set_content_height(video->swatch_box[index], 22);
        g_object_set_data(G_OBJECT(video->swatch_box[index]), "idx", GINT_TO_POINTER(index));
        gtk_drawing_area_set_draw_func(video->swatch_box[index], draw_swatch, video, NULL);
        video->swatch_lbl[index] = GTK_LABEL(mono_label());
        gtk_label_set_selectable(video->swatch_lbl[index], FALSE);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(video->swatch_box[index]), 0, index, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(video->swatch_lbl[index]), 1, index, 1, 1);
    }
    gtk_box_append(GTK_BOX(box), grid);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_notebook_append_page(video->sections, scroll, gtk_label_new(cocovid_section_name(COCOVID_S_COLOURS)));

    /* Lines */
    video->lines_text = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(video->lines_text, FALSE);
    gtk_text_view_set_cursor_visible(video->lines_text, FALSE);
    gtk_text_view_set_monospace(video->lines_text, TRUE);
    buffer = gtk_text_view_get_buffer(video->lines_text);
    gtk_text_buffer_create_tag(buffer, "changed", "background", "#C8B58C", "foreground", "#000000",
                               "weight", 700, NULL);
    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(video->lines_text));
    gtk_notebook_append_page(video->sections, scroll, gtk_label_new(cocovid_section_name(COCOVID_S_LINES)));

    /* Timing */
    box = padded_box();
    video->beam = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_text(video->beam, "beam");
    gtk_progress_bar_set_show_text(video->beam, TRUE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(video->beam));
    video->timing_text = GTK_LABEL(mono_label());
    gtk_label_set_selectable(video->timing_text, FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(video->timing_text));
    gtk_notebook_append_page(video->sections, box, gtk_label_new(cocovid_section_name(COCOVID_S_TIMING)));

    gtk_paned_set_start_child(GTK_PANED(root), left);
    gtk_paned_set_end_child(GTK_PANED(root), right);
    gtk_paned_set_resize_start_child(GTK_PANED(root), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(root), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(root), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(root), FALSE);

    /* start from the controller's state (the COCO_DEBUGGER_VIDEO_* variables) */
    video->updating = TRUE;
    gtk_drop_down_set_selected(video->view, (guint)cocovid_tab_view(video->tab));
    for (index = 0; index < zoom_count; index++)
        if (zooms[index] == cocovid_tab_zoom(video->tab)) gtk_drop_down_set_selected(video->zoom, (guint)index);
    for (index = 0; index < video->nov; index++)
        gtk_check_button_set_active(video->overlay_box[index], (cocovid_tab_overlays(video->tab) & overlays[index].bit) != 0);
    gtk_notebook_set_current_page(video->sections, cocovid_tab_env_section(video->tab));
    video->updating = FALSE;
    update_pixel_section(video);
    g_signal_connect(video->view, "notify::selected", G_CALLBACK(on_view), video);
    g_signal_connect(video->zoom, "notify::selected", G_CALLBACK(on_zoom), video);
    g_signal_connect(video->sections, "switch-page", G_CALLBACK(on_section), video);
    return root;
}
