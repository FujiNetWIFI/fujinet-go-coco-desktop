# FujiNet Go CoCo — desktop

[![Linux](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/linux.yml/badge.svg)](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/linux.yml)
[![macOS](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/macos.yml/badge.svg)](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/macos.yml)
[![Windows](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/windows.yml/badge.svg)](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/windows.yml)
[![Flatpak](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/flatpak.yml/badge.svg)](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/actions/workflows/flatpak.yml)

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

| Frontend | Toolkit | Binary | Status |
|---|---|---|---|
| GNOME | GTK4 + libadwaita (+ WebKitGTK) | `fujinet-go-coco-gnome` | complete |
| KDE | Qt6 Widgets (+ QtWebEngine) | `fujinet-go-coco-kde` | complete |
| macOS | AppKit (+ WKWebView) | `FujiNet Go CoCo.app` | complete (incl. debugger + bundled FujiNet) |
| Windows | Win32 (GDI + DwmFlush) | `fujinet-go-coco-windows.exe` | complete (incl. debugger + bundled FujiNet) |

The maintainer develops on Linux without Mac or Windows hardware: those
builds are compiled, tested and packaged on CI's macOS and Windows runners,
and reports from real users on those platforms are very welcome.

## Features

- CoCo 2 / CoCo 3 emulation (a headless build of XRoar), phase-locked to
  your display's vsync whenever a ~60 Hz frame clock is available,
  wall-clock paced otherwise. Live machine, TV input, artifact-colour and
  CPU (6809/HD6309) switching, all without a session restart.
- In-process FujiNet: disk mounting from TNFS hosts or the local SD folder,
  and network config through the embedded FujiNet web UI (`FujiNet ▸
  Configuration`), plus a live console-log window.
- Automatic gamepad support with hotplug (SDL3) driving the analog
  joystick ports.
- Import disk images into the FujiNet SD folder; Import System ROMs… for
  the Tandy firmware this project does not (and cannot) ship — see
  "CoCo system ROMs" below.
- Settings/Preferences windows (GNOME, KDE, macOS ⌘,, Windows) for aspect
  ratio, smooth scaling, FujiNet enable and gamepad enable.

### Developer debugger (F12)

Breakpoints, pause/step into/over/out, run-to, a 6809/HD6309 disassembler,
memory and register views, a decoded video pane, and symbol loading:
LWASM's `--symbol-dump` format, plus a small built-in table of Color BASIC
ROM entry points (`POLCAT`, `CHROUT`, `CSRDON`, …). Available in every
frontend — GTK, Qt, AppKit and Win32 — over the one shared engine.

C source-level stepping for programs built with
[CMOC](http://sarrazip.com/dev/cmoc.html): a source pane with the current
line marked, step/step-over by C statement, breakpoints set by clicking a
line, and a Locals pane showing parameters and locals by name and value.
Load it with the "Load C Source…" button, pointing at the `.lst` listing —
its sibling `.map` and every source file it names are found automatically.
For a program built from several `.c` files, select all of their `.lst`
files at once (every frontend's dialog allows multiple); loading is
additive, so each one adds its module's functions, lines and locals rather
than replacing what was already loaded. CMOC names the `.map` after the
linked executable, not any one source file, so when a listing's own name
doesn't match a `.map` next to it, the one `.map` actually present in that
directory is used instead — the common case for a multi-file build. Requires
compiling with `cmoc -i` (`--intermediate`), which is the flag that keeps
the listing and linker map around; without it CMOC deletes both once the
binary is linked. Available in every frontend.

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

## Cutting a release

Pushing a `v*` tag runs `.github/workflows/release.yml`, which builds every
artifact this repository currently publishes and attaches them to a draft
GitHub Release:

| Asset | Contents |
|---|---|
| `fujinet-go-coco-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-coco-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `online.fujinet.go.coco.gnome.flatpak` | single-file bundle: `flatpak install ./…flatpak` |
| `online.fujinet.go.coco.kde.flatpak` | same, for the KDE frontend |
| `fujinet-go-coco-<version>-windows.zip` | portable folder: unzip and run, no install |
| `fujinet-go-coco-<version>-windows-setup.exe` | NSIS installer (per-user, no admin rights) |
| `fujinet-go-coco-<version>-macos.zip` | `FujiNet Go CoCo.app`, unsigned unless the signing secrets are configured (see below) |

The version is declared in the tree, not derived from the tag, so a
downloaded build's About box can never disagree with the download it came
from — the release job's first job checks the tag, `CMakeLists.txt` and
both frontends' metainfo agree, and fails the whole run before anything is
built if they do not. To release 0.2.0:

1. `project(... VERSION 0.2.0 ...)` in `CMakeLists.txt` (everything else —
   the About boxes, `COCO_VERSION_STRING`, the CPack package names — follows
   from there).
2. Add a `<release version="0.2.0" date="…"/>` entry to both
   `frontends/*/data/*.metainfo.xml` — this is what software centres show,
   so put the user-visible notes there.
3. Commit, then `git tag v0.2.0 && git push origin v0.2.0`.

The release is created as a **draft** so the notes can be edited before it
goes out; if you prefer to write the release in the web UI first, do that
and the assets are attached to it when the tag build finishes.

### Signing the macOS build

`release.yml`'s `macos-package` job signs and notarises the app bundle when
these repository secrets exist, and quietly ships an unsigned bundle when
they do not — so a tag pushed before any of this is configured still
produces a working (if Gatekeeper-warned) release:

| Secret | What it is |
|---|---|
| `MACOS_CERTIFICATE` | "Developer ID Application" certificate + key, exported as `.p12`, then `base64 -i cert.p12` |
| `MACOS_CERTIFICATE_PASSWORD` | the password used for that export |
| `MACOS_SIGN_IDENTITY` | e.g. `Developer ID Application: Your Name (TEAMID)` |
| `MACOS_NOTARY_KEY` | App Store Connect API key `.p8`, base64-encoded |
| `MACOS_NOTARY_KEY_ID` | that key's Key ID |
| `MACOS_NOTARY_ISSUER` | the Issuer ID from App Store Connect |

All of it requires a paid Apple Developer Program membership. Certificate
but no notary key is a valid halfway house (the app is signed and its
origin verifiable, but first launch still needs right-click ▸ Open); the
job says so in a warning rather than failing. None of these secrets are
currently configured for this repository, so today's releases ship
unsigned — see `fujinet-go-adam-desktop`'s README for the full walkthrough
of producing them (none of it requires owning a Mac).

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance and for
what is and is not patched against XRoar.
