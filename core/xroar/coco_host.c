/*
 * Desktop host for the headless XRoar core. Drives xroar_init()/xroar_run()
 * and owns the state the custom XRoar modules (ui_desktop/vo, ao_desktop,
 * joystick_desktop) talk to. Pure C so it includes XRoar's headers natively.
 *
 * Ported from the Android app's coco_host.c; see coco_host.h for what changed.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "coco_host.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "top-config.h"
#include "cart.h"
#include "events.h"
#include "hkbd.h"
#include "joystick.h"
#include "machine.h"
#include "ui.h"
#include "vo.h"
#include "xroar.h"

/* --- log sink --------------------------------------------------------------
 * Replaces android/log.h. Same three levels, same call sites. */
static CocoLogSink g_log_sink = NULL;
static void *g_log_user = NULL;

void cocohost_set_log_sink(CocoLogSink sink, void *user) {
    g_log_sink = sink;
    g_log_user = user;
}

static void host_log(int level, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    CocoLogSink sink = g_log_sink;
    if (sink) {
        sink(level, msg, g_log_user);
    } else {
        static const char *const tag[] = { "info", "warning", "error" };
        fprintf(stderr, "cocohost: %s: %s\n",
                tag[level < 0 ? 0 : (level > 2 ? 2 : level)], msg);
    }
}

#define LOGI(...) host_log(0, __VA_ARGS__)
#define LOGW(...) host_log(1, __VA_ARGS__)
#define LOGE(...) host_log(2, __VA_ARGS__)

/* --- frame sink ----------------------------------------------------------- */
static CocoFrameSink g_frame_sink = NULL;
static void *g_frame_user = NULL;

void cocohost_set_frame_sink(CocoFrameSink sink, void *user) {
    g_frame_sink = sink;
    g_frame_user = user;
}

void cocohost_emit_frame(const uint32_t *xrgb8888, int width, int height) {
    CocoFrameSink sink = g_frame_sink;
    if (sink) {
        sink(xrgb8888, width, height, g_frame_user);
    }
}

/* --- frame-rate sink ------------------------------------------------------ */
static CocoFrameRateSink g_rate_sink = NULL;
static void *g_rate_user = NULL;

void cocohost_set_frame_rate_sink(CocoFrameRateSink sink, void *user) {
    g_rate_sink = sink;
    g_rate_user = user;
}

void cocohost_notify_frame_rate(int is_60hz) {
    CocoFrameRateSink sink = g_rate_sink;
    if (sink) {
        /* XRoar's own vo_render carries the boolean, not the rate; the two
         * rates a CoCo runs at are NTSC 59.94Hz and PAL 50Hz exactly. */
        sink(is_60hz ? 59940 : 50000, g_rate_user);
    }
}

/* --- config --------------------------------------------------------------- */
static char g_rompath[1024] = ".";
static char g_becker_ip[64] = "127.0.0.1";
static char g_becker_port[16] = "65504";

void cocohost_set_rompath(const char *path) {
    if (path && *path) {
        strncpy(g_rompath, path, sizeof(g_rompath) - 1);
        g_rompath[sizeof(g_rompath) - 1] = 0;
    }
}

void cocohost_set_becker_endpoint(const char *ip, int port) {
    if (ip && *ip) {
        strncpy(g_becker_ip, ip, sizeof(g_becker_ip) - 1);
        g_becker_ip[sizeof(g_becker_ip) - 1] = 0;
    }
    if (port > 0) {
        snprintf(g_becker_port, sizeof(g_becker_port), "%d", port);
    }
}

/* tools/xroar/support/becker.c's tap and link state. Declared here rather
 * than in becker.h, which stays upstream's. */
void becker_set_trace(void (*fn)(int dir, const uint8_t *buf, size_t len));
int becker_link_up(void);
int becker_rx_pending(void);

void cocohost_set_becker_trace(CocoBeckerTrace fn) {
    becker_set_trace(fn);
}

int cocohost_becker_link_up(void) {
    return becker_link_up();
}

int cocohost_becker_rx_pending(void) {
    return becker_rx_pending();
}

/* --- joystick state (UI thread writes, emulator thread reads) ------------- */
static atomic_int g_js_axis[JOYSTICK_NUM_PORTS][JOYSTICK_NUM_AXES];
static atomic_int g_js_button[JOYSTICK_NUM_PORTS][JOYSTICK_NUM_BUTTONS];
static int g_js_initialised = 0;

static void js_state_init(void) {
    for (int p = 0; p < JOYSTICK_NUM_PORTS; ++p) {
        for (int a = 0; a < JOYSTICK_NUM_AXES; ++a)
            atomic_store(&g_js_axis[p][a], 32767);  /* centre */
        for (int b = 0; b < JOYSTICK_NUM_BUTTONS; ++b)
            atomic_store(&g_js_button[p][b], 0);
    }
    g_js_initialised = 1;
}

void cocohost_set_joystick_axis(int port, int axis, int value) {
    if (port < 0 || port >= JOYSTICK_NUM_PORTS ||
        axis < 0 || axis >= JOYSTICK_NUM_AXES)
        return;
    if (value < 0) value = 0;
    if (value > 65535) value = 65535;
    atomic_store(&g_js_axis[port][axis], value);
}

void cocohost_set_joystick_button(int port, int button, int pressed) {
    if (port < 0 || port >= JOYSTICK_NUM_PORTS ||
        button < 0 || button >= JOYSTICK_NUM_BUTTONS)
        return;
    atomic_store(&g_js_button[port][button], pressed ? 1 : 0);
}

int cocohost_get_joystick_axis(int port, int axis) {
    if (port < 0 || port >= JOYSTICK_NUM_PORTS ||
        axis < 0 || axis >= JOYSTICK_NUM_AXES)
        return 32767;
    return atomic_load(&g_js_axis[port][axis]);
}

int cocohost_get_joystick_button(int port, int button) {
    if (port < 0 || port >= JOYSTICK_NUM_PORTS ||
        button < 0 || button >= JOYSTICK_NUM_BUTTONS)
        return 0;
    return atomic_load(&g_js_button[port][button]);
}

/* --- audio ring ------------------------------------------------------------
 * Interleaved stereo S16 at 44100 Hz. ao_desktop pushes completed buffers; the
 * session's audio backend drains. Bounded blocking wait so the audio device
 * paces consumption and the 60Hz emulator loop paces production. */
#define AUDIO_RING_SAMPLES (44100 * 2)  /* ~1s stereo */
static int16_t g_audio_ring[AUDIO_RING_SAMPLES];
static size_t g_audio_head = 0;  /* read */
static size_t g_audio_tail = 0;  /* write */
static size_t g_audio_count = 0;
static pthread_mutex_t g_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_cond = PTHREAD_COND_INITIALIZER;
static atomic_int g_audio_active = 0;

void cocohost_audio_set_active(int active) {
    pthread_mutex_lock(&g_audio_mutex);
    atomic_store(&g_audio_active, active ? 1 : 0);
    pthread_cond_broadcast(&g_audio_cond);
    pthread_mutex_unlock(&g_audio_mutex);
}

void cocohost_clear_audio(void) {
    pthread_mutex_lock(&g_audio_mutex);
    g_audio_head = g_audio_tail = g_audio_count = 0;
    pthread_cond_broadcast(&g_audio_cond);
    pthread_mutex_unlock(&g_audio_mutex);
}

void cocohost_push_audio(const int16_t *interleaved, int nframes) {
    if (!interleaved || nframes <= 0) return;
    const size_t samples = (size_t)nframes * 2;
    pthread_mutex_lock(&g_audio_mutex);
    for (size_t i = 0; i < samples; ++i) {
        if (g_audio_count >= AUDIO_RING_SAMPLES) {
            /* Overflow: drop oldest to keep latency bounded. */
            g_audio_head = (g_audio_head + 1) % AUDIO_RING_SAMPLES;
            g_audio_count--;
        }
        g_audio_ring[g_audio_tail] = interleaved[i];
        g_audio_tail = (g_audio_tail + 1) % AUDIO_RING_SAMPLES;
        g_audio_count++;
    }
    pthread_cond_broadcast(&g_audio_cond);
    pthread_mutex_unlock(&g_audio_mutex);
}

int cocohost_fill_audio(int16_t *out, int max_samples) {
    if (!out || max_samples <= 0) return 0;
    pthread_mutex_lock(&g_audio_mutex);
    /* Wait (bounded) for a full block, unless audio is being torn down. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 50 * 1000000L;  /* 50ms cap */
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (atomic_load(&g_audio_active) && g_audio_count < (size_t)max_samples) {
        if (pthread_cond_timedwait(&g_audio_cond, &g_audio_mutex, &deadline) != 0)
            break;  /* timed out: emit what we have, silence-padded */
    }
    int produced = 0;
    while (produced < max_samples && g_audio_count > 0) {
        out[produced++] = g_audio_ring[g_audio_head];
        g_audio_head = (g_audio_head + 1) % AUDIO_RING_SAMPLES;
        g_audio_count--;
    }
    pthread_mutex_unlock(&g_audio_mutex);
    /* Silence-pad the remainder so the consumer always gets a full block. */
    for (int i = produced; i < max_samples; ++i) out[i] = 0;
    return max_samples;
}

/* --- command queue (applied on the emulator thread at run_frame) ---------- */
struct key_event { int down; int scancode; };
/* Deep enough that cocohost_release_all_keys() -- which can enqueue one
 * release per held scancode -- cannot overflow a queue that already has a
 * frame's worth of typing in it. */
#define KEY_Q_SIZE 512
static struct key_event g_key_q[KEY_Q_SIZE];
static int g_key_head = 0, g_key_tail = 0;
/* What the host believes is held down, so a focus loss can release exactly
 * those. Written and read under g_cmd_mutex on the UI thread only. */
static uint8_t g_key_held[256 / 8];
static pthread_mutex_t g_cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_pending_tv_input = -1;
static atomic_int g_pending_reset = 0;

/* Caller holds g_cmd_mutex. */
static void key_enqueue_locked(int down, int scancode) {
    int next = (g_key_tail + 1) % KEY_Q_SIZE;
    if (next == g_key_head) return;  /* full: drop */
    g_key_q[g_key_tail].down = down;
    g_key_q[g_key_tail].scancode = scancode;
    g_key_tail = next;
}

void cocohost_inject_key(int down, int scancode) {
    if (scancode < 0 || scancode > 255) return;
    down = down ? 1 : 0;
    pthread_mutex_lock(&g_cmd_mutex);
    key_enqueue_locked(down, scancode);
    if (down)
        g_key_held[scancode >> 3] |= (uint8_t)(1u << (scancode & 7));
    else
        g_key_held[scancode >> 3] &= (uint8_t)~(1u << (scancode & 7));
    pthread_mutex_unlock(&g_cmd_mutex);
}

void cocohost_release_all_keys(void) {
    pthread_mutex_lock(&g_cmd_mutex);
    for (int sc = 0; sc < 256; ++sc) {
        if (g_key_held[sc >> 3] & (1u << (sc & 7)))
            key_enqueue_locked(0, sc);
    }
    memset(g_key_held, 0, sizeof(g_key_held));
    pthread_mutex_unlock(&g_cmd_mutex);
}

/* --- artifact-setting mapping (COCO_* -> XRoar) --------------------------- */
static const char *tv_input_arg(int mode) {
    switch (mode) {
        case COCO_TV_SVIDEO:       return "cmp";
        case COCO_TV_COMPOSITE_RB: return "cmp-rb";
        case COCO_TV_RGB:          return "rgb";
        case COCO_TV_COMPOSITE_BR:
        default:                   return "cmp-br";
    }
}

static int tv_input_xroar(int mode) {
    switch (mode) {
        case COCO_TV_SVIDEO:       return TV_INPUT_SVIDEO;
        case COCO_TV_COMPOSITE_RB: return TV_INPUT_CMP_KRBW;
        case COCO_TV_RGB:          return TV_INPUT_RGB;
        case COCO_TV_COMPOSITE_BR:
        default:                   return TV_INPUT_CMP_KBRW;
    }
}

static int ccr_xroar(int ccr) {
    switch (ccr) {
        case COCO_CCR_NONE:      return VO_CMP_CCR_PALETTE;
        case COCO_CCR_SIMPLE:    return VO_CMP_CCR_2BIT;
        case COCO_CCR_PARTIAL:   return VO_CMP_CCR_PARTIAL;
        case COCO_CCR_SIMULATED: return VO_CMP_CCR_SIMULATED;
        case COCO_CCR_5BIT:
        default:                 return VO_CMP_CCR_5BIT;
    }
}

static atomic_int g_pending_ccr = -1;

void cocohost_set_tv_input(int tv_input) {
    atomic_store(&g_pending_tv_input, tv_input_xroar(tv_input));
}

void cocohost_set_ccr(int ccr) {
    atomic_store(&g_pending_ccr, ccr_xroar(ccr));
}

/* Pending in-place machine switch (applied on the emulator thread).
 * g_pending_machine is the trigger; set the tv companion first. */
static atomic_int g_pending_machine = -1;
static atomic_int g_pending_machine_tv = -1;

void cocohost_set_machine(int machine, int tv_input) {
    atomic_store(&g_pending_machine_tv, tv_input);
    atomic_store(&g_pending_machine, machine);
}

/* --- CPU switch, in-place (COCO_CPU_*) -------------------------------------
 * g_current_cpu mirrors the last CPU actually applied -- by cocohost_core_
 * start or a prior cocohost_set_cpu -- so an in-place *machine* switch can
 * reapply it to the newly selected machine_config too (each named config
 * keeps its own struct machine_config::cpu, defaulted to CPU_MC6809, so
 * switching machines would otherwise silently drop back to 6809). */
static int cpu_xroar(int cpu) {
    return (cpu == COCO_CPU_HD6309) ? CPU_HD6309 : CPU_MC6809;
}
static atomic_int g_current_cpu = COCO_CPU_MC6809;
static atomic_int g_pending_cpu = -1;

void cocohost_set_cpu(int cpu) {
    atomic_store(&g_pending_cpu, cpu);
}

void cocohost_core_reset(void) {
    atomic_store(&g_pending_reset, 1);
}

static void apply_pending_commands(void) {
    /* Keyboard */
    pthread_mutex_lock(&g_cmd_mutex);
    while (g_key_head != g_key_tail) {
        struct key_event ev = g_key_q[g_key_head];
        g_key_head = (g_key_head + 1) % KEY_Q_SIZE;
        pthread_mutex_unlock(&g_cmd_mutex);
        if (ev.down)
            hk_scan_press((uint8_t)ev.scancode);
        else
            hk_scan_release((uint8_t)ev.scancode);
        pthread_mutex_lock(&g_cmd_mutex);
    }
    pthread_mutex_unlock(&g_cmd_mutex);

    int tv = atomic_exchange(&g_pending_tv_input, -1);
    if (tv >= 0) {
        ui_update_state(-1, ui_tag_tv_input, tv, NULL);
    }

    int ccr = atomic_exchange(&g_pending_ccr, -1);
    if (ccr >= 0) {
        ui_update_state(-1, ui_tag_ccr, ccr, NULL);
    }

    if (atomic_exchange(&g_pending_reset, 0)) {
        xroar_hard_reset();
    }

    /* CPU pending: latch it into g_current_cpu now so the machine-switch
     * branch below (if also pending this tick) reapplies it to the newly
     * selected config, rather than the switch's own hard reset picking up
     * whatever that config's cpu field was last left at. */
    int cpu_sw = atomic_exchange(&g_pending_cpu, -1);
    if (cpu_sw >= 0) {
        atomic_store(&g_current_cpu, cpu_sw);
    }

    /* In-place machine switch (CoCo 2 <-> CoCo 3): no xroar re-init. */
    int sw = atomic_exchange(&g_pending_machine, -1);
    if (sw >= 0) {
        int swtv = atomic_exchange(&g_pending_machine_tv, -1);
        const char *name = (sw == COCO_MACHINE_COCO2) ? "coco2bus" : "coco3";
        const char *rom = (sw == COCO_MACHINE_COCO2) ? "hdbdw3bck" : "hdbdw3bc3";
        int tvv = tv_input_xroar(swtv);
        struct machine_config *mc = machine_config_by_name(name);
        struct cart_config *cc = cart_config_by_name("becker");
        if (mc) {
            if (cc) { free(cc->rom); cc->rom = strdup(rom); }
            mc->cpu = cpu_xroar(atomic_load(&g_current_cpu));
            LOGI("in-place machine switch -> %s (becker rom %s)", name, rom);
            ui_update_state(-1, ui_tag_machine, mc->id, NULL);
            if (cc) ui_update_state(-1, ui_tag_cartridge, cc->id, NULL);
            xroar_hard_reset();
            /* tv-input last: selecting a CoCo 3 resets its tv-input. */
            ui_update_state(-1, ui_tag_tv_input, tvv, NULL);
        } else {
            LOGE("machine '%s' not found for in-place switch", name);
        }
    } else if (cpu_sw >= 0) {
        /* CPU-only switch: same in-place trick, applied to whichever machine
         * is currently selected rather than switching to a named one. */
        struct machine_config *mc = xroar.machine_config;
        if (mc) {
            mc->cpu = cpu_xroar(cpu_sw);
            LOGI("in-place cpu switch -> %s",
                 cpu_sw == COCO_CPU_HD6309 ? "HD6309" : "MC6809");
            xroar_hard_reset();
        }
    }
}

/* --- joystick configs ------------------------------------------------------
 * Bind two analog joystick profiles (desktop0 -> right, desktop1 -> left) to
 * the "desktop" submodule provided by joystick_desktop.c. */
static void setup_joysticks(void) {
    for (int port = 0; port < 2; ++port) {
        struct joystick_config *jc = joystick_config_new();
        char name[16];
        snprintf(name, sizeof(name), "desktop%d", port);
        jc->name = strdup(name);
        char alias[16];
        snprintf(alias, sizeof(alias), "joy%d", port);
        jc->alias = strdup(alias);
        jc->description = strdup(port == 0 ? "Desktop (right)" : "Desktop (left)");
        char spec[16];
        snprintf(spec, sizeof(spec), "desktop:%d", port);
        for (int a = 0; a < JOYSTICK_NUM_AXES; ++a)
            jc->axis_specs[a] = strdup(spec);
        for (int b = 0; b < JOYSTICK_NUM_BUTTONS; ++b)
            jc->button_specs[b] = strdup(spec);
        ui_update_state(-1, ui_tag_joystick_port, port, (void *)(intptr_t)jc->id);
    }
}

/* --- lifecycle ------------------------------------------------------------ */
static double g_tickerr = 0.0;
static int g_started = 0;

int cocohost_core_start(int machine, int tv_input, int ccr, int cpu) {
    if (g_started) {
        LOGW("cocohost_core_start ignored; already started");
        return 1;
    }
    if (!g_js_initialised) js_state_init();

    const char *machine_name =
        (machine == COCO_MACHINE_COCO2) ? "coco2bus" : "coco3";
    /* tv-input selects the artifact mode (cmp = S-Video/no artifacts,
     * cmp-br / cmp-rb = composite artifact phases, rgb = CoCo 3 RGB). */
    const char *tv_name = tv_input_arg(tv_input);
    const char *cpu_name = (cpu == COCO_CPU_HD6309) ? "6309" : "6809";

    /* argv strings must outlive xroar_init; keep them in this static frame. */
    static char a_rompath[1024];
    static char a_ip[64];
    static char a_port[16];
    strncpy(a_rompath, g_rompath, sizeof(a_rompath) - 1);
    a_rompath[sizeof(a_rompath) - 1] = 0;
    strncpy(a_ip, g_becker_ip, sizeof(a_ip) - 1);
    a_ip[sizeof(a_ip) - 1] = 0;
    strncpy(a_port, g_becker_port, sizeof(a_port) - 1);
    a_port[sizeof(a_port) - 1] = 0;

    const char *argv[32];
    int argc = 0;
    argv[argc++] = "xroar";
    argv[argc++] = "-ui";        argv[argc++] = "desktop";
    argv[argc++] = "-ao";        argv[argc++] = "desktop";
    argv[argc++] = "-rompath";   argv[argc++] = a_rompath;
    argv[argc++] = "-machine";   argv[argc++] = machine_name;
    argv[argc++] = "-machine-cpu"; argv[argc++] = cpu_name;
    argv[argc++] = "-tv-input";  argv[argc++] = tv_name;
    /* RS-DOS with Becker port: auto-loads HDB-DOS (@rsdos_becker = hdbdw3bck). */
    argv[argc++] = "-cart";      argv[argc++] = "becker";
    if (machine != COCO_MACHINE_COCO2) {
        /* CoCo 3 wants the CoCo 3 HDB-DOS variant. */
        argv[argc++] = "-cart-rom"; argv[argc++] = "hdbdw3bc3";
    }
    argv[argc++] = "-becker";
    argv[argc++] = "-becker-ip";   argv[argc++] = a_ip;
    argv[argc++] = "-becker-port"; argv[argc++] = a_port;

    LOGI("xroar_init machine=%s tv=%s cpu=%s becker=%s:%s rompath=%s",
         machine_name, tv_name, cpu_name, a_ip, a_port, a_rompath);

    struct ui_interface *ui = xroar_init(argc, (char **)argv);
    if (!ui) {
        LOGE("xroar_init failed");
        return 0;
    }
    xroar_init_finish();
    setup_joysticks();
    /* Apply the composite cross-colour (artifact) renderer. */
    ui_update_state(-1, ui_tag_ccr, ccr_xroar(ccr), NULL);

    atomic_store(&g_current_cpu, cpu);
    g_tickerr = 0.0;
    g_started = 1;
    return 1;
}

void cocohost_core_run_frame(void) {
    if (!g_started) return;
    apply_pending_commands();

    /* Advance one 60Hz frame's worth of emulated ticks, carrying the
     * fraction. Note this is the *host* frame quantum, not the machine's:
     * XRoar's own event clock decides when a video frame completes, so a PAL
     * machine simply completes a frame every 1.2 host quanta. The session
     * paces on the real frame rate. */
    g_tickerr += (double)EVENT_TICK_RATE / 60.0;
    int nticks = (int)(g_tickerr + 0.5);
    g_tickerr -= nticks;
    xroar_run(nticks);
}

void cocohost_core_stop(void) {
    if (!g_started) return;
    xroar_shutdown();
    g_started = 0;
    /* Drop anything the UI queued but the core never consumed, so a restart
     * does not replay a keystroke or a machine switch from the last run. */
    pthread_mutex_lock(&g_cmd_mutex);
    g_key_head = g_key_tail = 0;
    memset(g_key_held, 0, sizeof(g_key_held));
    pthread_mutex_unlock(&g_cmd_mutex);
    atomic_store(&g_pending_tv_input, -1);
    atomic_store(&g_pending_ccr, -1);
    atomic_store(&g_pending_cpu, -1);
    atomic_store(&g_pending_machine, -1);
    atomic_store(&g_pending_machine_tv, -1);
    atomic_store(&g_pending_reset, 0);
}
