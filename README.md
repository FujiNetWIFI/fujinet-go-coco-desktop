# FujiNet Go CoCo — desktop

A self-contained Tandy Color Computer with FujiNet built in. One shared C
session core plus four native frontends: GTK4/libadwaita on GNOME, Qt6 Widgets
on KDE, AppKit on macOS and Win32 on Windows.

Emulation is a headless build of [XRoar](https://www.6809.org.uk/xroar/).
FujiNet is not a subprocess: the firmware is built as a shared library,
`dlopen`'d into the process, and joined to the emulator over the **Becker
port** — DriveWire carried on loopback TCP 65504 — so HDB-DOS finds it as its
boot device and the machine drops straight into FujiNet's CONFIG.

Note the direction, because it is the opposite of the other two desktop ports:
here **FujiNet listens and the emulator connects out to it**, so the session
starts FujiNet first.

This is the third member of the FujiNet Go desktop family;
[`fujinet-go-adam-desktop`](https://github.com/FujiNetWIFI/fujinet-go-adam-desktop)
is the model repository, and its `PORTING.md` is the specification this
follows.

> **Status: in progress (M1 of 10).** The core and the GNOME frontend are up:
> a CoCo 3 or CoCo 2 boots, paced to display vsync, with keyboard input, live
> machine / TV-input / artifact-colour switching, disk and ROM import, and
> gamepads on both analog joystick ports. The in-process FujiNet runtime, the
> KDE / macOS / Windows frontends, the 6809 debugger, CI and packaging are
> still to come. See `TODO`.

## Building

```sh
git clone https://github.com/FujiNetWIFI/fujinet-go-coco-desktop
cd fujinet-go-coco-desktop
cmake -B build -G Ninja
cmake --build build
./build/frontends/gnome/fujinet-go-coco-gnome
```

No preparation step: the build provides its own dependencies. A plain
`git clone`, a source tarball with no git metadata, and a flatpak build all
end up with a usable checkout. XRoar arrives as a SHA256-pinned release
tarball (it has no git repository to submodule), which the configure step
downloads and verifies for itself.

Build dependencies: CMake ≥ 3.20, Ninja, a C11 compiler, Python 3 and SDL3.
The GNOME frontend additionally needs libadwaita ≥ 1.4 / GTK ≥ 4.10, and the
KDE one Qt6 ≥ 6.4 Widgets. WebKitGTK 6.0 and QtWebEngine are optional; without
them the FujiNet web UI opens in the system browser.

### Useful options

| Option | Default | Meaning |
|---|---|---|
| `FRONTEND` | `all` | `gnome`, `kde`, `macos`, `windows`, `all`, `none`. `all` considers only the host's viable frontends. |
| `WITH_FUJINET` | `ON` | Build and embed the FujiNet runtime. |
| `WITH_WEBVIEW` | `ON` | Embed the FujiNet web UI rather than opening a browser. |
| `WITH_COCO_ROMS` | `ON` | Embed the Tandy system ROMs (see `COMPLIANCE.md`). |
| `XROAR_SRC` | — | Build against an out-of-tree XRoar checkout instead of the pinned tarball. |
| `FUJINET_SRC` | — | Likewise for the FujiNet firmware. |
| `XROAR_RESTAGE` | `OFF` | Re-stage XRoar — how to pick up uncommitted edits in an `XROAR_SRC` checkout. |

Developing against local checkouts:

```sh
cmake -B build -G Ninja \
      -DXROAR_SRC=$HOME/Workspace/xroar-1.11 \
      -DFUJINET_SRC=$HOME/Workspace/fujinet-pc-coco \
      -DXROAR_RESTAGE=ON
```

## CoCo system ROMs

Color BASIC, Extended Color BASIC and the CoCo 3's Super Extended Color BASIC
are Tandy/Microware copyrighted firmware and are **not** freely licensed. The
HDB-DOS DW3 cartridge ROMs are freely redistributable and are always embedded,
so the FujiNet link works either way.

- **`WITH_COCO_ROMS=ON`** (the default for a local build) compiles the system
  ROMs in, so the machine boots out of the box. **A build made this way must
  not be redistributed.**
- **`WITH_COCO_ROMS=OFF`** builds without them, and is what every published
  artifact uses. Supply your own with **Import System ROMs…**, which copies
  them into `~/.local/share/fujinet-go-coco/roms` (override with
  `COCO_ROM_DIR`). XRoar looks ROMs up by exact basename, so keep the names
  (`bas13.rom`, `extbas11.rom`, `coco3.rom`).

This is the opposite default from the Apple II port, which follows AppleWin's
own decade-long precedent of shipping with Apple's ROMs compiled in. XRoar
sets no such precedent — it ships no ROMs and expects the user to supply them
— so this port does the same. See `COMPLIANCE.md`.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

`boot_smoke` runs the emulator headless for 120 frames and checks it produced
frames, sane geometry, a non-blank screen and a reported frame rate; it skips
(ctest 77) on a `WITH_COCO_ROMS=OFF` build, which has no system ROMs to boot.
`no_embedded_roms_*` registers only on such a build and greps every shipped
binary for the Tandy ROMs.

## Running it

`COCO_PACE_LOG=1` reports the emulator's pacing once a second — how many
frames ran, how many were on vsync, and what rate the machine declared:

```
cocosession pace: 60 fps over 1000 ms (0 behind, 60 on vsync, machine 59940 mHz)
```

60 on vsync means the phase lock is engaged. A PAL machine (50000 mHz) will
report 0 on vsync by design: no desktop display ticks at 50 Hz, so the pacer
declines to lock and uses the wall clock instead.

If the machine boots but HDB-DOS loops connect/disconnect on a ~5 s metronome,
check for a stray standalone `fujinet-pc-coco` holding 65504 before suspecting
the bus:

```sh
ss -tlnp | grep 65504
```

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance and for
what is and is not patched against XRoar.
