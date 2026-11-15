// Debugger support (machine_debug.h): what mc6821_read() would return, with
// none of its side effects -- no read strobe, no interrupt flag cleared, no
// preread delegate (so a keyboard column is the last one scanned).

uint8_t mc6821_peek(struct MC6821 *pia, uint16_t address) {
	switch (address & 3) {
	default:
	case 0:
		if (DDR_SELECTED(pia->a))
			return pia->a.direction_register;
		return pia->a.out_sink & pia->a.in_sink;
	case 1:
		return pia->a.control_register | pia->a.irq1_received | pia->a.irq2_received;
	case 2:
		if (DDR_SELECTED(pia->b))
			return pia->b.direction_register;
		return (pia->b.output_register & pia->b.direction_register) | (PIA_VALUE_B(pia) & ~pia->b.direction_register);
	case 3:
		return pia->b.control_register | pia->b.irq1_received | pia->b.irq2_received;
	}
}

