/** \file
 *
 *  \brief Desktop UI + video-output module for XRoar.
 *
 *  Headless counterpart to src/sdl2/{ui_sdl2,vo_sdl2}.c: builds a vo_interface
 *  backed by a vo_render that writes XRGB8888 into an owned buffer, and on each
 *  vsync hands the completed viewport to the desktop host (coco_host.c) via
 *  cocohost_emit_frame(), which the session copies into its frame store for the
 *  UI thread to paint.
 *
 *  XRoar is GPLv3 (see COPYING.GPL).
 */

#include "top-config.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "delegate.h"
#include "xalloc.h"

#include "hkbd.h"
#include "joystick.h"
#include "messenger.h"
#include "module.h"
#include "ui.h"
#include "vo.h"
#include "vo_render.h"

#include "../coco_host.h"

// Matches src/sdl2/vo_sdl2.c maxima (covers the 736x276 "underscan" viewport),
// and COCOSESSION_FB_MAX_{WIDTH,HEIGHT} in core/include/cocosession.h.
#define MAX_VIEWPORT_WIDTH  (800)
#define MAX_VIEWPORT_HEIGHT (300)

// Desktop analog joystick module (joystick_desktop.c).
extern struct joystick_module *desktop_js_modlist[];

struct vo_desktop_interface {
	struct vo_interface vo_interface;
	uint32_t *pixels;   // XRGB8888, MAX_VIEWPORT_WIDTH * MAX_VIEWPORT_HEIGHT
	int vp_w;
	int vp_h;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void vo_desktop_set_viewport(void *sptr, int vp_w, int vp_h) {
	struct vo_desktop_interface *vod = sptr;
	struct vo_interface *vo = &vod->vo_interface;
	struct vo_render *vr = vo->renderer;

	if (vp_w < 16) vp_w = 16;
	if (vp_w > MAX_VIEWPORT_WIDTH) vp_w = MAX_VIEWPORT_WIDTH;
	if (vp_h < 6) vp_h = 6;
	if (vp_h > MAX_VIEWPORT_HEIGHT) vp_h = MAX_VIEWPORT_HEIGHT;

	vo_render_set_viewport(vr, vp_w, vp_h);
	vr->buffer_pitch = vr->viewport.w;
	vod->vp_w = vr->viewport.w;
	vod->vp_h = vr->viewport.h;
	vo_set_draw_area(vo, 0, 0, vr->viewport.w, vr->viewport.h);
}

// Called once per emulated frame (vo_vsync with draw=1). The render buffer
// holds the completed viewport; ship it to the session.
static void vo_desktop_draw(void *sptr) {
	struct vo_desktop_interface *vod = sptr;
	cocohost_emit_frame(vod->pixels, vod->vp_w, vod->vp_h);
}

static void vo_desktop_resize(void *sptr, unsigned int w, unsigned int h) {
	(void)sptr; (void)w; (void)h;  // the frontend's widget owns sizing/scaling
}

// XRoar reports whether the machine is running at 60Hz. Unlike the Android
// host, which ignored this, the desktop session needs it: the vsync phase lock
// can only work when the machine's rate and the display's rate agree, so a
// 50Hz (PAL) machine has to fall back to wall-clock pacing.
static void vo_desktop_notify_frame_rate(void *sptr, _Bool is_60hz) {
	(void)sptr;
	cocohost_notify_frame_rate(is_60hz ? 1 : 0);
}

static void vo_desktop_free(void *sptr) {
	struct vo_desktop_interface *vod = sptr;
	struct vo_render *vr = vod->vo_interface.renderer;
	// xroar_shutdown invokes this delegate (vo_interface->free) directly, NOT
	// via vo_free(), so we own the full teardown: unregister the vo's messenger
	// client (otherwise a stale callback dangles into freed memory across a
	// machine switch), free the renderer + buffer, then the vo struct itself.
	messenger_client_unregister(vod->vo_interface.msgr_client_id);
	if (vr) vo_render_free(vr);
	free(vod->pixels);
	free(vod);
}

static struct vo_interface *vo_desktop_new(void) {
	struct vo_desktop_interface *vod = vo_interface_new(sizeof(*vod));
	struct vo_interface *vo = &vod->vo_interface;

	vo_interface_init(vo);

	// Use the explicit ARGB8 base format, NOT the VO_RENDER_FMT_ARGB32 alias:
	// that alias is gated on `#if __BYTE_ORDER == __BIG_ENDIAN`, but neither
	// macro is in scope here (no <endian.h>), so the alias wrongly resolves to
	// the big-endian branch. ARGB8's map_argb8() packs 0xFFRRGGBB, i.e. a
	// little-endian uint32 of 0x00RRGGBB with the alpha byte set -- exactly the
	// XRGB8888 every frontend's display widget paints, with no per-pixel
	// conversion anywhere.
	struct vo_render *vr = vo_render_new(VO_RENDER_FMT_ARGB8);
	vo_set_renderer(vo, vr);

	vod->pixels = xmalloc(MAX_VIEWPORT_WIDTH * MAX_VIEWPORT_HEIGHT * sizeof(uint32_t));
	memset(vod->pixels, 0, MAX_VIEWPORT_WIDTH * MAX_VIEWPORT_HEIGHT * sizeof(uint32_t));
	vo_render_set_buffer(vr, vod->pixels);

	vo->free = DELEGATE_AS0(void, vo_desktop_free, vod);
	vo->set_viewport = DELEGATE_AS2(void, int, int, vo_desktop_set_viewport, vod);
	vo->draw = DELEGATE_AS0(void, vo_desktop_draw, vod);
	vo->resize = DELEGATE_AS2(void, unsigned, unsigned, vo_desktop_resize, vod);
	vr->notify_frame_rate = DELEGATE_AS1(void, bool, vo_desktop_notify_frame_rate, vod);

	// Sensible default viewport (640x240 "title"); the machine refines it.
	vo_desktop_set_viewport(vod, 640, 240);

	return vo;
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static void *ui_desktop_new(void *cfg);
static void ui_desktop_free(void *sptr);

struct ui_module ui_desktop_module = {
	.common = { .name = "desktop", .description = "Desktop UI",
	            .new = ui_desktop_new, },
	.joystick_module_list = desktop_js_modlist,
};

static void *ui_desktop_new(void *cfg) {
	(void)cfg;
	struct ui_interface *ui = xmalloc(sizeof(*ui));
	*ui = (struct ui_interface){0};

	ui->free = DELEGATE_AS0(void, ui_desktop_free, ui);
	ui->vo_interface = vo_desktop_new();

	// Host keyboard scancode subsystem (we inject via hk_scan_press/release).
	hk_init();

	return ui;
}

static void ui_desktop_free(void *sptr) {
	struct ui_interface *ui = sptr;
	// Do NOT free ui->vo_interface here: xroar_shutdown already tore it down
	// via its own vo_interface->free delegate. Calling vo_free() here as well
	// was a double free of the renderer.
	free(ui);
}
