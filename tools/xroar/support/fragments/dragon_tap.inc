// Debugger bus tap (machine_debug.h). Called from dragon_cpu_cycle() for
// every cycle that advances the clock: writes just before the RAM store (so
// the old byte can still be read), reads once the data bus is settled.

static void dragon_debug_bus_tap(struct dragon *dragon, _Bool RnW, uint16_t address, unsigned Zrow, unsigned Zcol) {
	struct machine_bus_cycle cycle;
	cycle.address = address;
	cycle.value = dragon->CPU->D;
	cycle.old_value = 0;
	cycle.is_write = !RnW;
	cycle.has_old = 0;
	cycle.physical = -1;
	if (dragon->SAM->RAS0 || dragon->SAM->RAS1) {
		unsigned bank = dragon->SAM->RAS1 ? 1 : 0;
		cycle.physical = machine_debug_ram_physical(dragon->RAM, bank, Zrow, Zcol);
		if (!RnW && cycle.physical >= 0) {
			cycle.old_value = *ram_a8(dragon->RAM, bank, Zrow, Zcol);
			cycle.has_old = 1;
		}
	}
	dragon->public.bus_tap(dragon->public.bus_tap_ctx, &cycle);
}

