// Debugger support (machine_debug.h): the address decode of
// tcc1014_mem_cycle() as a pure function. Returns the S value; *ras says
// whether the access selects RAM and *zaddr is the 19-bit RAM address
// (MMU bank << 13 | offset) it would present.
unsigned tcc1014_debug_decode(struct TCC1014 *gimep, uint16_t address, _Bool *ras, uint32_t *zaddr) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	unsigned select = 7;
	_Bool ram = 0;
	uint32_t zout = 0;
	if (address < 0xff00) {
		_Bool use_mmu = gime->MMUEN;
		if (address >= 0xfe00 && gime->MC3) {
			ram = 1;
			use_mmu = 0;
		}
		unsigned bank = use_mmu ? gime->mmu_bank[gime->TR | (address >> 13)]
		                        : (0x38 | (address >> 13));
		if (!gime->TY && bank >= 0x3c) {
			if (!gime->MC1) {
				select = (bank >= 0x3e) ? 1 : 0;
			} else {
				select = gime->MC0 ? 1 : 0;
			}
		} else {
			ram = 1;
		}
		zout = (bank << 13) | (address & 0x1fff);
	} else if (address < 0xff40) {
		if ((address & 0x10) == 0)
			select = 2;
	} else if (address < 0xff60) {
		if (gime->MC2 || address >= 0xff50)
			select = 6;
	} else if (address >= 0xffe0) {
		select = 0;
	}
	if (ras)
		*ras = ram;
	if (zaddr)
		*zaddr = zout;
	return select;
}

unsigned tcc1014_debug_scanline(struct TCC1014 *gimep) {
	struct TCC1014_private *gime = (struct TCC1014_private *)gimep;
	return gime->scanline;
}

