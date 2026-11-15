// Debugger support (machine_debug.h).

// Where a CPU address lands in RAM, without running a cycle: the bank
// (RAS0/RAS1) and the row and column the SAM would present.
_Bool mc6883_debug_ram_address(struct MC6883 *samp, uint16_t address, unsigned *bank, unsigned *row, unsigned *col) {
	struct MC6883_private *sam = (struct MC6883_private *)samp;
	if (samp->mem_cycle != mc6883_mem_cycle)
		return 0;
	_Bool is_ffxx = ((address >> 8) & 0xff) == 0xff;
	_Bool is_ram = !(address & 0x8000) || (sam->TY && !is_ffxx);
	if (!is_ram)
		return 0;
	*bank = (sam->ram_ras1 && (address & sam->ram_ras1_bit)) ? 1 : 0;
	*row = RAM_TRANSLATE_ROW(address);
	*col = RAM_TRANSLATE_COL(address);
	return 1;
}

_Bool mc6883_get_debug_state(struct MC6883 *samp, struct mc6883_debug_state *out) {
	struct MC6883_private *sam = (struct MC6883_private *)samp;
	if (samp->mem_cycle != mc6883_mem_cycle)
		return 0;
	out->V = sam->V;
	out->F = sam->F;
	out->P = sam->P;
	out->R = sam->R;
	out->M = sam->M;
	out->TY = sam->TY;
	out->reg = mc6883_get_register(samp);
	out->video_address = (sam->vcounter[VC_B15_5].value << 5)
	                   | (sam->vcounter[VC_B4].value << 4)
	                   | sam->vcounter[VC_B3_0].value;
	return 1;
}

