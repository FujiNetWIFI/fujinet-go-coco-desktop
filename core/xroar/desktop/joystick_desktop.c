/** \file
 *
 *  \brief Desktop analog joystick module for XRoar.
 *
 *  Provides the "desktop" joystick submodule whose axis/button controls read
 *  the per-port analog state set by the session (via coco_host.c). coco_host
 *  registers two profiles -- desktop0 (right port) and desktop1 (left port) --
 *  whose specs are "desktop:PORT"; configure_axis/configure_button bind a
 *  control to (PORT, index) and read it through cocohost_get_joystick_*.
 *
 *  The CoCo joystick is two 6-bit analog axes (read here as 0..65535, 32767 =
 *  centre) plus one (CoCo 1/2) or two (CoCo 3) buttons per port.
 *
 *  XRoar is GPLv3 (see COPYING.GPL).
 */

#include "top-config.h"

#include <stdlib.h>

#include "delegate.h"
#include "xalloc.h"

#include "joystick.h"
#include "module.h"

#include "../coco_host.h"

struct desktop_js_control {
	struct joystick_control joystick_control;
	int port;
	int index;
};

static int desktop_axis_read(void *sptr) {
	struct desktop_js_control *c = sptr;
	return cocohost_get_joystick_axis(c->port, c->index);
}

static int desktop_button_read(void *sptr) {
	struct desktop_js_control *c = sptr;
	return cocohost_get_joystick_button(c->port, c->index);
}

static void desktop_control_free(void *sptr) {
	free(sptr);
}

static struct desktop_js_control *new_control(char *spec, unsigned index) {
	struct desktop_js_control *c = xmalloc(sizeof(*c));
	*c = (struct desktop_js_control){0};
	c->port = spec ? atoi(spec) : 0;
	if (c->port < 0 || c->port >= JOYSTICK_NUM_PORTS) c->port = 0;
	c->index = (int)index;
	c->joystick_control.free = DELEGATE_AS0(void, desktop_control_free, c);
	return c;
}

static struct joystick_control *configure_axis(char *spec, unsigned jaxis) {
	struct desktop_js_control *c = new_control(spec, jaxis);
	c->joystick_control.read = DELEGATE_AS0(int, desktop_axis_read, c);
	return &c->joystick_control;
}

static struct joystick_control *configure_button(char *spec, unsigned jbutton) {
	struct desktop_js_control *c = new_control(spec, jbutton);
	c->joystick_control.read = DELEGATE_AS0(int, desktop_button_read, c);
	return &c->joystick_control;
}

static struct joystick_submodule desktop_js_submod = {
	.name = "desktop",
	.configure_axis = configure_axis,
	.configure_button = configure_button,
};

static struct joystick_submodule *desktop_js_submodlist[] = {
	&desktop_js_submod,
	NULL,
};

static struct joystick_module desktop_js_mod = {
	.common = { .name = "desktop", .description = "Desktop analog joystick" },
	.submodule_list = desktop_js_submodlist,
};

struct joystick_module *desktop_js_modlist[] = {
	&desktop_js_mod,
	NULL,
};
