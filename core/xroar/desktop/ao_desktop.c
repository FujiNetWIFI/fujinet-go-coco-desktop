/** \file
 *
 *  \brief Desktop audio-output module for XRoar.
 *
 *  Counterpart to src/null/ao_null.c and src/sdl2/ao_sdl2.c: presents XRoar a
 *  44100 Hz interleaved stereo S16 sound buffer and, as each buffer completes,
 *  pushes it into the desktop host ring (coco_host.c) where the session's audio
 *  backend drains it. Pacing comes from the 60Hz emulator loop (production) and
 *  the audio device callback (consumption), so this module never sleeps.
 *
 *  XRoar is GPLv3 (see COPYING.GPL).
 */

#include "top-config.h"

#include <stdint.h>
#include <stdlib.h>

#include "delegate.h"
#include "xalloc.h"

#include "ao.h"
#include "module.h"
#include "sound.h"

#include "../coco_host.h"

#define AO_RATE       (44100)
#define AO_CHANNELS   (2)
#define AO_NFRAMES    (1024)

struct ao_desktop_interface {
	struct ao_interface public;
	int16_t *buffer;  // AO_NFRAMES * AO_CHANNELS samples
};

static void *new(void *cfg);
static void ao_desktop_free(void *sptr);
static void *ao_desktop_write_buffer(void *sptr, void *buffer);

struct module ao_desktop_module = {
	.name = "desktop", .description = "Desktop audio",
	.new = new,
};

static void *new(void *cfg) {
	(void)cfg;
	struct ao_desktop_interface *aod = xmalloc(sizeof(*aod));
	*aod = (struct ao_desktop_interface){0};
	struct ao_interface *ao = &aod->public;

	aod->buffer = xmalloc(AO_NFRAMES * AO_CHANNELS * sizeof(int16_t));

	ao->free = DELEGATE_AS0(void, ao_desktop_free, aod);
	ao->sound_interface = sound_interface_new(aod->buffer, SOUND_FMT_S16_HE,
						  AO_RATE, AO_CHANNELS, AO_NFRAMES);
	if (!ao->sound_interface) {
		free(aod->buffer);
		free(aod);
		return NULL;
	}
	ao->sound_interface->write_buffer =
		DELEGATE_AS1(voidp, voidp, ao_desktop_write_buffer, aod);

	cocohost_clear_audio();
	cocohost_audio_set_active(1);
	return aod;
}

static void ao_desktop_free(void *sptr) {
	struct ao_desktop_interface *aod = sptr;
	cocohost_audio_set_active(0);
	if (aod->public.sound_interface)
		sound_interface_free(aod->public.sound_interface);
	free(aod->buffer);
	free(aod);
}

// Called by sound.c when a full buffer of AO_NFRAMES stereo frames is ready.
// Push it to the host ring and reuse the same buffer for the next fill.
static void *ao_desktop_write_buffer(void *sptr, void *buffer) {
	(void)sptr;
	cocohost_push_audio((const int16_t *)buffer, AO_NFRAMES);
	return buffer;
}
