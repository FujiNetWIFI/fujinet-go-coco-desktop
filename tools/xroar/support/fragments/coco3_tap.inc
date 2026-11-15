// Debugger bus tap (machine_debug.h), called from cpu_cycle() below: writes
// before write_byte() (so the old byte can still be read), reads after
// read_byte(). cpu_cycle_noclock(), the debug path, never reaches it.

static unsigned coco3_debug_dat_bank(struct coco3 *mcc3, unsigned address) {
	if (!mcc3->dat.MMUEN || (mcc3->dat.MC3 && address >= 0xfe00 && address < 0xff00))
		return 0;
	return mcc3->dat.mmu_bank[(address >> 13) | mcc3->dat.task] >> 6;
}

static void coco3_debug_bus_tap(struct coco3 *mcc3, _Bool RnW, uint16_t address) {
	struct machine_bus_cycle cycle;
	cycle.address = address;
	cycle.value = mcc3->CPU->D;
	cycle.old_value = 0;
	cycle.is_write = !RnW;
	cycle.has_old = 0;
	cycle.physical = -1;
	if (mcc3->GIME->RAS) {
		unsigned bank = coco3_debug_dat_bank(mcc3, address);
		unsigned Zrow = mcc3->GIME->Z;
		unsigned Zcol = mcc3->GIME->Z >> 9;
		cycle.physical = machine_debug_ram_physical(mcc3->RAM, bank, Zrow, Zcol);
		if (!RnW && cycle.physical >= 0) {
			cycle.old_value = *ram_a8(mcc3->RAM, bank, Zrow, Zcol);
			cycle.has_old = 1;
		}
	}
	mcc3->public.bus_tap(mcc3->public.bus_tap_ctx, &cycle);
}

