// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Debugger support (machine_debug.h). Added by
// tools/xroar/patch-staged-tree.py for the desktop port.

void machine_debug_set_bus_tap(struct machine *mach, machine_bus_tap_func func, void *ctx) {
	if (!mach)
		return;
	mach->bus_tap = NULL;
	mach->bus_tap_ctx = ctx;
	mach->bus_tap = func;
}

// RAM

static struct ram *machine_debug_ram(struct machine *mach) {
	if (!mach)
		return NULL;
	struct ram *ram = (struct ram *)part_component_by_id_is_a(&mach->part, "RAM", "ram");
	if (!ram || ram->d_width != 8 || !ram->bank_nelems)
		return NULL;
	return ram;
}

uint32_t machine_debug_ram_size(struct machine *mach) {
	struct ram *ram = machine_debug_ram(mach);
	if (!ram)
		return 0;
	uint32_t size = 0;
	for (unsigned bank = 0; bank < ram->nbanks; bank++) {
		if (ram->d[bank])
			size += ram->bank_nelems;
	}
	return size;
}

int machine_debug_ram_peek(struct machine *mach, uint32_t physical) {
	struct ram *ram = machine_debug_ram(mach);
	if (!ram)
		return -1;
	uint32_t bank = physical / ram->bank_nelems;
	uint32_t index = physical % ram->bank_nelems;
	if (bank >= ram->nbanks || !ram->d[bank])
		return -1;
	return ((uint8_t *)ram->d[bank])[index];
}

_Bool machine_debug_ram_poke(struct machine *mach, uint32_t physical, uint8_t value) {
	struct ram *ram = machine_debug_ram(mach);
	if (!ram)
		return 0;
	uint32_t bank = physical / ram->bank_nelems;
	uint32_t index = physical % ram->bank_nelems;
	if (bank >= ram->nbanks || !ram->d[bank])
		return 0;
	((uint8_t *)ram->d[bank])[index] = value;
	return 1;
}

int32_t machine_debug_ram_physical(struct ram *ram, unsigned bank, unsigned row, unsigned col) {
	uint8_t *where = ram_a8(ram, bank, row, col);
	if (!where)
		return -1;
	return (int32_t)(bank * ram->bank_nelems + (size_t)(where - (uint8_t *)ram->d[bank]));
}

// Memory map

void machine_debug_build_memory_map(struct machine *mach, machine_debug_describe_func describe,
                                    struct machine_memory_map *map) {
	map->nregions = 0;
	map->truncated = 0;
	map->ram_size = machine_debug_ram_size(mach);
	for (unsigned address = 0; address < 0x10000; address += 16) {
		struct machine_memory_region unit;
		memset(&unit, 0, sizeof(unit));
		unit.physical_base = -1;
		describe(mach, address, &unit);
		unit.first = address;
		unit.last = address + 15;
		if (map->nregions > 0) {
			struct machine_memory_region *prev = &map->regions[map->nregions - 1];
			_Bool same = prev->kind == unit.kind && prev->writable == unit.writable &&
			             prev->name && unit.name && 0 == strcmp(prev->name, unit.name);
			if (same) {
				if (prev->physical_base < 0) {
					same = unit.physical_base < 0;
				} else {
					same = unit.physical_base == prev->physical_base + (int32_t)(prev->last + 1 - prev->first);
				}
			}
			if (same) {
				prev->last = unit.last;
				continue;
			}
		}
		if (map->nregions >= MACHINE_MEMORY_MAP_MAX_REGIONS) {
			map->truncated = 1;
			map->regions[map->nregions - 1].last = 0xffff;
			return;
		}
		map->regions[map->nregions++] = unit;
	}
}

// Video taps: chained wrappers over the render_line / signal_hs / signal_fs
// delegates of the VDG (CoCo 1/2) or GIME (CoCo 3). There is one machine, so
// one set of state; it is only touched from the emulator thread (or while it
// is parked), and forgotten on xroar_shutdown().

static struct {
	struct machine *machine;
	struct MC6847 *vdg;
	struct TCC1014 *gime;
	struct machine_video_tap tap;
	DELEGATE_T3(void, unsigned, unsigned, uint8cp) saved_render_line;
	DELEGATE_T1(void, bool) saved_signal_hs;
	DELEGATE_T1(void, bool) saved_signal_fs;
} video_tap_state;

static unsigned video_tap_scanline(void) {
	if (video_tap_state.vdg) {
		struct mc6847_debug_state vdg_state;
		mc6847_get_debug_state(video_tap_state.vdg, &vdg_state);
		return vdg_state.scanline;
	}
	if (video_tap_state.gime)
		return tcc1014_debug_scanline(video_tap_state.gime);
	return 0;
}

static void video_tap_render_line(void *sptr, unsigned burst, unsigned npixels, uint8_t const *data) {
	(void)sptr;
	DELEGATE_SAFE_CALL(video_tap_state.saved_render_line, burst, npixels, data);
	if (video_tap_state.tap.render_line) {
		struct machine_video_line line;
		line.scanline = video_tap_scanline();
		line.tick = event_current_tick;
		line.burst = burst;
		line.npixels = npixels;
		line.data = data;
		video_tap_state.tap.render_line(video_tap_state.tap.ctx, &line);
	}
}

static void video_tap_signal_hs(void *sptr, _Bool level) {
	(void)sptr;
	DELEGATE_SAFE_CALL(video_tap_state.saved_signal_hs, level);
	if (video_tap_state.tap.hsync)
		video_tap_state.tap.hsync(video_tap_state.tap.ctx, level, video_tap_scanline(), event_current_tick);
}

static void video_tap_signal_fs(void *sptr, _Bool level) {
	(void)sptr;
	DELEGATE_SAFE_CALL(video_tap_state.saved_signal_fs, level);
	if (video_tap_state.tap.fsync)
		video_tap_state.tap.fsync(video_tap_state.tap.ctx, level, video_tap_scanline(), event_current_tick);
}

static void video_tap_chips(struct machine *mach, struct MC6847 **vdg, struct TCC1014 **gime) {
	*vdg = (struct MC6847 *)part_component_by_id_is_a(&mach->part, "VDG", "MC6847");
	*gime = *vdg ? NULL : (struct TCC1014 *)part_component_by_id_is_a(&mach->part, "GIME", "TCC1014");
}

_Bool machine_debug_video_tap_active(struct machine *mach) {
	if (!mach || video_tap_state.machine != mach)
		return 0;
	struct MC6847 *vdg;
	struct TCC1014 *gime;
	video_tap_chips(mach, &vdg, &gime);
	if (vdg && vdg == video_tap_state.vdg) {
		return vdg->render_line.func == video_tap_render_line &&
		       vdg->signal_hs.func == video_tap_signal_hs &&
		       vdg->signal_fs.func == video_tap_signal_fs;
	}
	if (gime && gime == video_tap_state.gime) {
		return gime->render_line.func == video_tap_render_line &&
		       gime->signal_hs.func == video_tap_signal_hs &&
		       gime->signal_fs.func == video_tap_signal_fs;
	}
	return 0;
}

_Bool machine_debug_video_tap_install(struct machine *mach, const struct machine_video_tap *tap) {
	if (!mach || !tap)
		return 0;
	struct MC6847 *vdg;
	struct TCC1014 *gime;
	video_tap_chips(mach, &vdg, &gime);
	if (!vdg && !gime)
		return 0;
	if (video_tap_state.machine != mach || video_tap_state.vdg != vdg || video_tap_state.gime != gime) {
		memset(&video_tap_state, 0, sizeof(video_tap_state));
		video_tap_state.machine = mach;
		video_tap_state.vdg = vdg;
		video_tap_state.gime = gime;
	}
	// Chain each delegate that is not already ours, so a delegate that
	// something else replaced mid-run is picked up again without ever
	// pointing a wrapper at itself.
	if (vdg) {
		if (vdg->render_line.func != video_tap_render_line) {
			video_tap_state.saved_render_line = vdg->render_line;
			vdg->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp, video_tap_render_line, &video_tap_state);
		}
		if (vdg->signal_hs.func != video_tap_signal_hs) {
			video_tap_state.saved_signal_hs = vdg->signal_hs;
			vdg->signal_hs = DELEGATE_AS1(void, bool, video_tap_signal_hs, &video_tap_state);
		}
		if (vdg->signal_fs.func != video_tap_signal_fs) {
			video_tap_state.saved_signal_fs = vdg->signal_fs;
			vdg->signal_fs = DELEGATE_AS1(void, bool, video_tap_signal_fs, &video_tap_state);
		}
	} else {
		if (gime->render_line.func != video_tap_render_line) {
			video_tap_state.saved_render_line = gime->render_line;
			gime->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp, video_tap_render_line, &video_tap_state);
		}
		if (gime->signal_hs.func != video_tap_signal_hs) {
			video_tap_state.saved_signal_hs = gime->signal_hs;
			gime->signal_hs = DELEGATE_AS1(void, bool, video_tap_signal_hs, &video_tap_state);
		}
		if (gime->signal_fs.func != video_tap_signal_fs) {
			video_tap_state.saved_signal_fs = gime->signal_fs;
			gime->signal_fs = DELEGATE_AS1(void, bool, video_tap_signal_fs, &video_tap_state);
		}
	}
	video_tap_state.tap = *tap;
	return 1;
}

void machine_debug_video_tap_remove(struct machine *mach) {
	if (!mach || video_tap_state.machine != mach)
		return;
	struct MC6847 *vdg;
	struct TCC1014 *gime;
	video_tap_chips(mach, &vdg, &gime);
	if (vdg && vdg == video_tap_state.vdg) {
		if (vdg->render_line.func == video_tap_render_line)
			vdg->render_line = video_tap_state.saved_render_line;
		if (vdg->signal_hs.func == video_tap_signal_hs)
			vdg->signal_hs = video_tap_state.saved_signal_hs;
		if (vdg->signal_fs.func == video_tap_signal_fs)
			vdg->signal_fs = video_tap_state.saved_signal_fs;
	} else if (gime && gime == video_tap_state.gime) {
		if (gime->render_line.func == video_tap_render_line)
			gime->render_line = video_tap_state.saved_render_line;
		if (gime->signal_hs.func == video_tap_signal_hs)
			gime->signal_hs = video_tap_state.saved_signal_hs;
		if (gime->signal_fs.func == video_tap_signal_fs)
			gime->signal_fs = video_tap_state.saved_signal_fs;
	}
	memset(&video_tap_state, 0, sizeof(video_tap_state));
}

void machine_debug_forget(void) {
	memset(&video_tap_state, 0, sizeof(video_tap_state));
}

