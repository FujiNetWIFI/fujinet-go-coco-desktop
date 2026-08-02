# Licence and provenance

Per-component origin, licence, and how each enters the build. Written before
the first public build, as the family convention requires.

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | `FujiNetWIFI/fujinet-go-coco-desktop` | GPL-3.0-or-later | the repository itself |
| XRoar 1.11 (emulator core) | `https://www.6809.org.uk/xroar/dl/xroar-1.11.tar.gz` | GPL-3.0-or-later | pinned release tarball, SHA256-verified, unpacked to `third_party/xroar`, staged into `core/xroar-generated` and compiled as part of this project |
| FujiNet firmware | `FujiNetWIFI/fujinet-firmware` | GPL-3.0-or-later | pinned checkout `third_party/fujinet-firmware`, built as a shared library and `dlopen`'d |
| portalib | vendored in XRoar | GPL-3.0-or-later (sds.c: BSD-2-Clause) | compiled from the staged tree |
| SDL3 | system | Zlib | audio and gamepad only |
| TRE / libsystre | system (Windows only) | BSD-2-Clause | POSIX regex for `portalib/sdsx.c`; MinGW-w64 has no `regex.h` |
| mbedTLS 3.6.5 | system or built from the pinned tag | Apache-2.0 | linked into the FujiNet library |

Both halves are GPL-3.0-or-later, so the combined work is **GPL-3.0**. Unlike
the Apple II target there is no GPL-2.0 component to relicense upward.

## XRoar is used unmodified except where noted

XRoar is not vendored as a fork. `cmake/StageXRoar.cmake` copies `src/`,
`portalib/` and `top-config.h` out of the pinned tarball, replaces
`src/becker.c` (see below), and runs `tools/xroar/patch-staged-tree.py`,
which applies three further kinds of transform to the staged copy, all
idempotent and hard-failing on a missing anchor (a version bump that moves
one is a build error, not a silent no-op), and all recorded in
`core/xroar-generated/.source-info`:

1. **`config.h` is generated** instead of by XRoar's autotools `configure`,
   which we do not run. It selects the headless CoCo/Dragon feature set and
   resolves the host-dependent half with the preprocessor, so one file
   serves Linux, macOS and Windows — including `WINDOWS32`, the macro
   upstream's own code keys basic POSIX-vs-Winsock branching on in
   `src/becker.c` and several other unmodified files (not `_WIN32`, which
   this generated file uses everywhere else; autotools would normally
   define it, and nothing else here does that job) and `HAVE_STRSEP`
   (correctly *not* defined on Windows: MinGW-w64 lacks `strsep()`, unlike
   the glibc/Darwin libc this project also builds against).
2. **Two module registrations** are added to `src/ui.c` and `src/ao.c`,
   guarded on `XROAR_DESKTOP_MODULES`, so XRoar can find
   `core/xroar/desktop/{ui,ao,joystick}_desktop.c`. This is the same edit the
   Android app makes under `__ANDROID__`. A further nine narrow guards
   (`src/xroar.c` ×6, `src/ui.c` ×2 more, `src/hkbd.c` ×1) exclude upstream's
   *native* win32 GUI module (`ui_windows32`, never registered here) and its
   `-C` console-allocation option from a Windows build — without them,
   `WINDOWS32` being correctly defined for reason 1 above also pulls in a
   header wanting SDL2 (this project is SDL3-only) and several externs whose
   defining files are never compiled here.
3. **A small read-only accessor** is added to the GIME (TCC1014) part
   (`src/tcc1014/tcc1014.h`/`.c`), for the debugger's video pane
   (`core/debugger/cocovideo.c`). GIME's video-mode state has no public
   getter and, per real hardware, is mostly write-only even through the
   CPU's own bus.

`src/becker.c` itself is not one of these three: `cmake/StageXRoar.cmake`
replaces the whole file, before `patch-staged-tree.py` ever runs, with
`tools/xroar/support/becker.c` — a lazy-connecting variant. See below.

No frontend source (SDL, GTK, X11, WASM, OpenGL) is compiled; those files are
simply never listed in `core/CMakeLists.txt`.

### The becker.c patch, and why it is needed

Upstream `becker_open()` connects exactly once and disables the port if that
fails. Two things about this application make that wrong:

- XRoar shares a process with the FujiNet runtime it connects *out* to. Even
  with FujiNet started first (`cocosession_start` does), a client that gives up
  at startup has no way back.
- Switching between a CoCo 2 and a CoCo 3 in place tears the link down and
  brings it back up.

The replacement connects lazily and reconnects — throttled — whenever the link
is down. Once connected, behaviour is identical to upstream. This is a
candidate to send to XRoar rather than carry indefinitely.

## CoCo system ROMs

Color BASIC (`bas13.rom`), Extended Color BASIC (`extbas11.rom`) and the
CoCo 3's Super Extended Color BASIC (`coco3.rom`) are Tandy/Microware
copyrighted firmware and are **not** freely licensed.

The HDB-DOS DW3 cartridge ROMs (`hdbdw3bck.rom`, `hdbdw3bc3.rom`) *are* freely
redistributable, and the FujiNet link does not work without them, so they are
always embedded.

Which system ROMs are embedded is controlled by the `WITH_COCO_ROMS` CMake
option:

- **`ON` (default)** — all five are embedded, so a development build boots
  straight to the CoCo 3 banner. **This configuration must not be
  redistributed.**
- **`OFF`** — only the HDB-DOS pair is embedded; the user supplies the system
  ROMs. **Every artifact this repository publishes is built this way** — the
  flatpaks, the macOS bundle, the Windows folder and installer, and the release
  job.

This is the opposite of the Apple II target's default, and deliberately so.
That project follows AppleWin's own decade-long precedent of shipping with
Apple's ROMs compiled in. XRoar sets no such precedent: it ships no ROMs at
all and expects the user to supply them, so this port does the same.

### How it works

XRoar resolves machine and cartridge ROMs by **basename** against its
`-rompath` at run time, so nothing has to be patched into the core.
`tools/xroar/embed-roms.py` generates a C table of whichever images the option
selected, and `paths.c` writes them into the user's ROM directory
(`$XDG_DATA_HOME/fujinet-go-coco/roms`, overridable with `COCO_ROM_DIR`) on
first start — **skipping any name that is already there**, so a ROM the user
imported always wins over a compiled-in one.

Users of an `OFF` build supply the ROMs through **Import System ROMs…**, which
copies them into that directory under their own names and restarts the
session. Without them the app reports what is missing and where to put it,
rather than showing a black screen.

**This is enforced, not just documented.** The `no_embedded_roms_*` ctests take
a distinctive slice of every ROM image in `tools/xroar/roms` and grep a built
binary for it; a Tandy ROM that gets compiled in despite the option fails the
build, and a *missing* HDB-DOS ROM fails it too. They register only when
`WITH_COCO_ROMS=OFF`, one per binary — `boot_smoke` plus every frontend
executable built in that configuration.

The probe rejects any 64-byte slice dominated by a single byte value, requires
real variety, and scans the whole file. That is inherited from the Apple II
port, where a first version that only rejected mostly-*zero* chunks matched a
16KB run of `0xFF` and reported a false leak — and did so invisibly, because
the small test binary it was pointed at happened to have no such run. A ROM
with nothing distinctive to search for is **reported as unchecked** rather than
silently counted as passing.

## Deliberate choices

- **The 6809/6309 disassembler is written fresh** rather than lifted from
  XRoar's `mc6809_trace.c` / `hd6309_trace.c`. Reuse would be licence-clean
  (GPL-3.0 into GPL-3.0), but those are formatted for tracing rather than for
  the line-oriented API the debugger engine and all four frontends consume.
  The **decode tables** are derived from them, which is both permitted and
  sensible; the formatting and API are not.
- **The debugger builds on XRoar's own hooks** — `struct debug_cpu`
  (`src/debug_cpu.h`) and `src/breakpoint.c` — rather than inventing a stepped
  core path of its own.
