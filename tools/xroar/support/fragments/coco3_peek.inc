// Debugger memory access (machine_debug.h): the CPU's view of memory without
// running a bus cycle, so nothing is acknowledged or consumed.

static uint8_t *coco3_debug_ram_ptr(struct coco3 *mcc3, unsigned address) {
	_Bool ras = 0;
	uint32_t zaddr = 0;
	tcc1014_debug_decode(mcc3->GIME, address, &ras, &zaddr);
	if (!ras)
		return NULL;
	return ram_a8(mcc3->RAM, coco3_debug_dat_bank(mcc3, address), zaddr, zaddr >> 9);
}

static uint8_t coco3_peek_byte(struct machine *mach, unsigned address) {
	struct coco3 *mcc3 = (struct coco3 *)mach;
	address &= 0xffff;
	_Bool ras = 0;
	uint32_t zaddr = 0;
	unsigned select = tcc1014_debug_decode(mcc3->GIME, address, &ras, &zaddr);
	uint8_t *where;
	if (ras) {
		where = coco3_debug_ram_ptr(mcc3, address);
		return where ? *where : 0xff;
	}
	switch (select) {
	case 0:
		where = mcc3->ROM0 ? rombank_a8(mcc3->ROM0, address) : NULL;
		return where ? *where : 0xff;
	case 1:
		where = (mcc3->cart && mcc3->cart->ROM) ? rombank_a8(mcc3->cart->ROM, address & 0x3fff) : NULL;
		return where ? *where : 0xff;
	case 2:
		return mc6821_peek((address & 32) == 0 ? mcc3->PIA0 : mcc3->PIA1, address);
	case 7:
		if (address >= 0xff90 && address < 0xffc0) {
			struct tcc1014_debug_state gime;
			tcc1014_get_debug_state(mcc3->GIME, &gime);
			if (address < 0xffa0) {
				// The IRQ/FIRQ status registers: report, do not acknowledge.
				if (address == 0xff92)
					return gime.irq_state & 0x3f;
				if (address == 0xff93)
					return gime.firq_state & 0x3f;
				return gime.registers[address & 15];
			}
			if (address < 0xffb0)
				return gime.mmu_bank[address & 15] & 0x3f;
			return gime.palette_reg[address & 15] & 0x3f;
		}
		return 0xff;
	default:
		// Cartridge I/O (Becker, floppy): reading is not free of side effects.
		return 0xff;
	}
}

static _Bool coco3_poke_byte(struct machine *mach, unsigned address, uint8_t value) {
	struct coco3 *mcc3 = (struct coco3 *)mach;
	uint8_t *where = coco3_debug_ram_ptr(mcc3, address & 0xffff);
	if (!where)
		return 0;
	*where = value;
	return 1;
}

static void coco3_describe_address(struct machine *mach, unsigned address, struct machine_memory_region *out) {
	struct coco3 *mcc3 = (struct coco3 *)mach;
	_Bool ras = 0;
	uint32_t zaddr = 0;
	unsigned select = tcc1014_debug_decode(mcc3->GIME, address, &ras, &zaddr);
	out->kind = MACHINE_REGION_UNMAPPED;
	out->name = "UNMAPPED";
	if (ras) {
		out->kind = MACHINE_REGION_RAM;
		out->writable = 1;
		out->name = "RAM";
		unsigned bank = coco3_debug_dat_bank(mcc3, address);
		out->physical_base = machine_debug_ram_physical(mcc3->RAM, bank, zaddr, zaddr >> 9);
		if (mcc3->dat.MC3 && address >= 0xfe00 && address < 0xff00)
			out->name = "RAM FExx";
		return;
	}
	switch (select) {
	case 0:
		out->kind = MACHINE_REGION_ROM;
		out->name = (address >= 0xffe0) ? "VECTORS" : "ROM";
		break;
	case 1:
		out->kind = MACHINE_REGION_CART_ROM;
		out->name = "CART ROM";
		break;
	case 2:
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		out->name = (address & 32) ? "PIA1" : "PIA0";
		break;
	case 6:
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		out->name = "CART IO";
		break;
	default:
		if (address >= 0xff90 && address < 0xffa0) {
			out->name = "GIME";
		} else if (address >= 0xffa0 && address < 0xffb0) {
			out->name = "MMU";
		} else if (address >= 0xffb0 && address < 0xffc0) {
			out->name = "PALETTE";
		} else if (address >= 0xffc0 && address < 0xffe0) {
			out->name = "SAM";
		} else {
			break;
		}
		out->kind = MACHINE_REGION_IO;
		out->writable = 1;
		break;
	}
}

static void coco3_memory_map(struct machine *mach, struct machine_memory_map *map) {
	struct coco3 *mcc3 = (struct coco3 *)mach;
	memset(map, 0, sizeof(*map));
	struct tcc1014_debug_state gime;
	tcc1014_get_debug_state(mcc3->GIME, &gime);
	map->is_coco3 = 1;
	map->gime_coco_mode = gime.COCO;
	map->mmu_enabled = gime.MMUEN;
	map->constant_ram = gime.MC3;
	map->mc2 = gime.MC2;
	map->rom_mode = (gime.MC1 ? 2 : 0) | (gime.MC0 ? 1 : 0);
	map->task = gime.TR;
	map->timer_fast = gime.TINS;
	map->sam_map_type = gime.TY;
	for (unsigned index = 0; index < 16; index++)
		map->mmu_bank[index] = gime.mmu_bank[index] & 0x3f;
	machine_debug_build_memory_map(mach, coco3_describe_address, map);
}

