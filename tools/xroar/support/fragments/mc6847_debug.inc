
// Debugger support (machine_debug.h).
void mc6847_get_debug_state(struct MC6847 *vdgp, struct mc6847_debug_state *out) {
	struct MC6847_private *vdg = (struct MC6847_private *)vdgp;
	out->scanline = vdg->scanline;
	out->GM = vdg->GM;
	out->nA_S = vdg->nA_S;
	out->nA_G = vdg->nA_G;
	out->EXT = vdg->EXT;
	out->CSS = vdg->CSS;
}
