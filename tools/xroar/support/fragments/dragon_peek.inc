// Debugger memory access (machine_debug.h): the CPU's view of memory without
// running a bus cycle, so nothing is acknowledged or consumed.

static uint8_t *dragon_debug_ram_ptr(struct dragon *dragon, unsigned address) {
	unsigned bank, row, col;
	if (!mc6883_debug_ram_address(dragon->SAM, address, &bank, &row, &col))
		return NULL;
	return ram_a8(dragon->RAM, bank, row, col);
}

static uint8_t dragon_peek_byte(struct machine *mach, unsigned address) {
	struct dragon *dragon = (struct dragon *)mach;
	address &= 0xffff;
	uint8_t *where;
	switch (dragon->SAM->decode(dragon->SAM, 1, address)) {
	case 0:
		where = dragon_debug_ram_ptr(dragon, address);
		return where ? *where : 0xff;
	case 1:
	case 2:
		where = dragon->ROM0 ? rombank_a8(dragon->ROM0, address) : NULL;
		return where ? *where : 0xff;
	case 3:
		where = (dragon->cart && dragon->cart->ROM) ? rombank_a8(dragon->cart->ROM, address & 0x3fff) : NULL;
		return where ? *where : 0xff;
	case 4:
		if (dragon->relaxed_pia0_decode || (address & 4) == 0)
			return mc6821_peek(dragon->PIA0, address);
		return 0xff;
	case 5:
		if (dragon->relaxed_pia1_decode || (address & 4) == 0)
			return mc6821_peek(dragon->PIA1, address);
		return 0xff;
	default:
		// Cartridge I/O (Becker, floppy) and the SAM: reading is not
		// free of side effects, or not readable at all.
		return 0xff;
	}
}

static _Bool dragon_poke_byte(struct machine *mach, unsigned address, uint8_t value) {
	struct dragon *dragon = (struct dragon *)mach;
	uint8_t *where = dragon_debug_ram_ptr(dragon, address & 0xffff);
	if (!where)
		return 0;
	*where = value;
	return 1;
}

static void dragon_describe_address(struct machine *mach, unsigned address, struct machine_memory_region *out) {
	struct dragon *dragon = (struct dragon *)mach;
	out->kind = MACHINE_REGION_UNMAPPED;
	out->name = "UNMAPPED";
	switch (dragon->SAM->decode(dragon->SAM, 1, address)) {
	case 0:
		out->kind = MACHINE_REGION_RAM;
		out->writable = 1;
		out->name = "RAM";
		break;
	case 1:
		out->kind = MACHINE_REGION_ROM;
		out->name = "EXTBAS ROM";
		break;
	case 2:
		out->kind = MACHINE_REGION_ROM;
		out->name = (address >= 0xffe0) ? "VECTORS" : "BASIC ROM";
		break;
	case 3:
		out->kind = MACHINE_REGION_CART_ROM;
		out->name = "CART ROM";
		break;
	case 4:
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		out->name = "PIA0";
		break;
	case 5:
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		out->name = "PIA1";
		break;
	case 6:
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		out->name = "CART IO";
		break;
	default:
		if (address >= 0xffc0 && address < 0xffe0) {
			out->kind = MACHINE_REGION_IO;
			out->writable = 1;
			out->name = "SAM";
		}
		break;
	}
}

static void dragon_memory_map(struct machine *mach, struct machine_memory_map *map) {
	struct dragon *dragon = (struct dragon *)mach;
	memset(map, 0, sizeof(*map));
	struct mc6883_debug_state sam;
	if (mc6883_get_debug_state(dragon->SAM, &sam)) {
		map->sam_map_type = sam.TY;
		map->sam_memory_size = sam.M;
		map->sam_page = sam.P;
	}
	machine_debug_build_memory_map(mach, dragon_describe_address, map);
}

