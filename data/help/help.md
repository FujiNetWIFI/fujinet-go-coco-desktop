This is the text of FujiNet Go CoCo's built-in Help (Help ▸ FujiNet Go CoCo
Help, or Ctrl+Shift+H; ⌘? on a Mac). The app shows each part only to the
frontend it is about; here you see them all. Every topic below is one page in
the app.

# Getting started

FujiNet Go CoCo is a Tandy Color Computer 2 or 3 with a FujiNet built in. The
machine is XRoar's, with a 6809 or 6309 processor. The FujiNet runs inside the
app, joined to the emulator over the Becker port, so HDB-DOS finds it as its boot
device and the machine goes straight into FujiNet's CONFIG.

1. **Start the app.** The machine boots HDB-DOS into CONFIG. The Tandy ROMs are
   part of the app, or come from **Import BIOS…** if your build has none. See
   [BIOS and ROMs](#bios-and-roms).
2. **Pick a disk in CONFIG.** Browse hosts and the SD folder, mount an image and
   boot it. See [FujiNet](#fujinet).
3. **Or bring your own files.** **Import Disk or Tape Image…** copies a `.dsk`,
   `.os9`, `.vdk`, `.cas` or `.wav` to FujiNet's SD folder, and a `.rom` or `.ccc`
   to the cartridge folder. See [Disks, tapes and cartridges](#disks-tapes-and-cartridges).
4. **Change the machine.** The Machine, Display, Artifact Colour and CPU choices
   switch at once, with no restart. See [Machines](#machines).
5. **Play.** The keyboard is the CoCo's, and a gamepad drives the joystick ports.
   See [Controllers and keys](#controllers-and-keys).

## This Help

The list on the left has a topic for each part of the app; the search box
above it finds every section that has all the words you type. A **Help**
button in Preferences, the Debugger and the FujiNet console log opens the topic
for what it shows, and <kbd>Ctrl+Shift+H</kbd> does the same for the window you
are in.

<!-- only: gnome kde windows -->

In Preferences, the Debugger and the console log, <kbd>F1</kbd> does too. In the
main window <kbd>F1</kbd> is a CoCo key, so it goes to the machine.

<!-- end -->

The same text is on GitHub, as
[data/help/help.md](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop/blob/main/data/help/help.md).

<!-- only: macos -->

On a Mac, the Help menu's search field also finds Help topics.

<!-- end -->

<!-- context: main -->
# The window

The machine's picture fills the window. The menus have:

- **Machine** (CoCo 3 or CoCo 2), **Reset** and **Import BIOS…**: see
  [Machines](#machines) and [BIOS and ROMs](#bios-and-roms).
- **Display**, **Artifact Colour** and **CPU**: see [Machines](#machines).
- **Import Disk or Tape Image…**: see
  [Disks, tapes and cartridges](#disks-tapes-and-cartridges).
- **FujiNet Configuration…** and **FujiNet Console Log…**: see
  [FujiNet](#fujinet).
- **Debugger** (<kbd>F12</kbd>), **Preferences**, **Help** and **About**.

<!-- only: gnome -->

On GNOME, everything is in the main menu (the button at the right of the
header bar; <kbd>F10</kbd> opens it).

<!-- end -->

<!-- only: kde -->

The menu bar has Machine, Media, FujiNet, View, Settings and Help; Display, Artifact
Colour and CPU are in the View menu, and Preferences is in the Settings menu.

<!-- end -->

<!-- only: windows -->

The menu bar has Machine, View, Media, FujiNet and Help. The Machine menu has
**Settings…** and **ROMs…**, which are Preferences, and Exit; View has Fullscreen
(<kbd>F11</kbd>).

<!-- end -->

<!-- only: macos -->

On a Mac, Settings and Quit are in the app menu.

<!-- end -->

<!-- context: nobios -->
## No BIOS

A build that does not embed the Tandy ROMs shows **No CoCo BIOS**, with an
**Import BIOS…** button, until the ROMs the machine needs are imported. They are
copyrighted, so the app cannot include them. See [BIOS and ROMs](#bios-and-roms).

## The picture

The picture can be shown television-shaped, square or in whole steps, and blended
when it is stretched. Both are in [Preferences](#preferences). <kbd>F11</kbd>
toggles full screen.

# Controllers and keys

The keyboard is the CoCo's keyboard: a key you press types on the CoCo, and both
the press and the release go to the machine, because XRoar tracks held keys.

## The keyboard

Everything goes to the machine except the keys the app keeps for itself.
<kbd>Alt</kbd> is not passed to the window manager: it is a real key on a CoCo 3
keyboard.

| Shortcut | Does |
|---|---|
| <kbd>Ctrl+Shift+H</kbd> | Help |
| <kbd>F11</kbd> | Fullscreen |
| <kbd>F12</kbd> | Debugger |

<!-- only: gnome -->

<kbd>F10</kbd> opens the main menu.

<!-- end -->

<!-- only: linux -->

Key combinations with the Super (Windows) key are the desktop's.

<!-- end -->

## Gamepads

Gamepads are picked up as they are plugged in, and can come and go at any time.
A pad drives the CoCo's analog joystick ports. Preferences ▸ **Gamepads** turns
this off.

# Machines

The Machine, Display, Artifact Colour and CPU menus switch while the machine
runs, with no session restart. A machine switch resets the machine, with the
matching HDB-DOS Becker ROM, and FujiNet stays connected.

- **Machine**: *Color Computer 3* (the default) or *Color Computer 2*.
- **Display**: the television signal: *S-Video*, *Composite (blue-red)* (the
  default), *Composite (red-blue)* or *RGB*. RGB is a CoCo 3 connector, so it is
  not offered on a CoCo 2. The composite signal has the artifact colours hi-res
  games rely on.
- **Artifact Colour**: how those colours are made: *None*, *Simple (2-bit)*,
  *5-bit* (the default), *Partial NTSC* or *Simulated NTSC*.
- **CPU**: *Motorola 6809* (the default) or *Hitachi 6309 (experimental)*.

# BIOS and ROMs

The Tandy firmware (Color BASIC, Extended Color BASIC and the CoCo 3's Super
Extended Color BASIC) is copyrighted, so builds that do not embed it need your own
copy. **Import BIOS…** takes your ROM files or a zip, and **Import BIOS from a
Folder…** a folder that holds them. Each file is recognised by its size and
checksum, whatever it is called, and copied into the app's `roms` folder (see
[Files and folders](#files-and-folders)). The ROMs page of Preferences lists what
each machine needs and whether it is imported:

| For | ROM |
|---|---|
| CoCo 2 | Color BASIC |
| CoCo 2 | Extended Color BASIC |
| CoCo 3 | Super Extended Color BASIC (CoCo 3) |

# Disks, tapes and cartridges

Files come in two ways: FujiNet serves disks and tapes, and XRoar loads
cartridges.

## Import Disk or Tape Image

**Import Disk or Tape Image…** copies a file where it is used:

- A `.dsk`, `.os9`, `.vdk` (disk images), `.cas` or `.wav` (tape images) goes to
  FujiNet's SD folder, to be mounted from CONFIG or FujiNet's web page.
- A `.rom` or `.ccc` (a cartridge) goes to the app's cartridge folder.

Other file types are refused.

# FujiNet

The FujiNet is FujiNet's own firmware, built as a shared library and run inside
the app. It joins the emulator over the Becker port, DriveWire carried on loopback
TCP (port 65504). The direction is the opposite of the other desktop apps: FujiNet
listens and the emulator connects out to it, so the app starts FujiNet first.

## CONFIG

CONFIG is FujiNet's program for choosing what to boot: the SD folder and servers
on the network. Pick an image and the machine boots it.

## The FujiNet menu

- **FujiNet Configuration…** shows FujiNet's own web page, which mounts disks,
  joins networks and sets up hosts. It is on port 64002 on this computer only.
- **FujiNet Console Log…** shows what FujiNet prints. See
  [The console log](#the-console-log).

<!-- context: fujilog -->
## The console log

The FujiNet Console Log window shows FujiNet's own messages as they come,
following the end while you are at the bottom.

## An external FujiNet

**Use external FujiNet** skips the built-in FujiNet and connects to another
instance, at the **Address:port** you give, for example `127.0.0.1:65504`. Set
that instance's Becker listener to that address. FujiNet Configuration and the
console log belong to the built-in FujiNet, so they say the FujiNet is external.

## Turning it off

Preferences ▸ **FujiNet** turns the FujiNet runtime off. The machine then boots
without it.

## Behind the scenes

- FujiNet's Becker port is 65504 and its web UI 64002, on this computer only. A
  stray standalone `fujinet-pc-coco` holding 65504 makes the link misbehave; stop
  it, or point the app at it.
- FujiNet's settings and its SD folder are in the app's `fujinet` folder.

# Preferences

Preferences set up the machine, the picture, FujiNet and the sound. The Machine,
FujiNet, Sound and Gamepads settings apply by restarting the session when
Preferences closes; the picture settings apply at once. The settings are one file,
shared by every frontend of the app on the computer.

<!-- only: macos -->

On a Mac this window is called **Settings** (⌘,).

<!-- end -->

<!-- only: windows -->

On Windows, **Settings…** and **ROMs…** in the Machine menu are the two halves of
this window.

<!-- end -->

<!-- context: prefs/Machine -->
## Machine settings

- **Model**: *Color Computer 3* or *Color Computer 2* (setting `machine`).
- **Display**: the television signal (setting `tv_input`).
- **Artifact colour**: how the artifact colours are made (setting `ccr`).

## Window settings

- **Aspect ratio**: *TV (4:3)*, *Square pixels* or *Integer scale* (setting
  `aspect_mode`).
- **Smooth scaling**: interpolate when the picture is scaled, instead of leaving
  the pixels sharp (setting `smooth_scaling`).

## FujiNet settings

- **FujiNet**: run the FujiNet runtime and serve it to the CoCo (setting
  `enable_fujinet`).
- **Use external FujiNet**: connect to another instance instead (setting
  `fujinet_external`).
- **Address:port**: where to connect, for example `127.0.0.1:65504` (see
  [An external FujiNet](#an-external-fujinet); setting `fujinet_external_addr`).
- **Sound**: open the system audio device (setting `enable_audio`).
- **Gamepads**: drive the analog joystick ports from connected gamepads (setting
  `enable_gamepad`).

<!-- context: prefs/ROMs -->
## ROMs settings

The ROMs page lists what each machine needs and whether it is imported:

- **Color BASIC** and **Extended Color BASIC** for the CoCo 2, and **Super
  Extended Color BASIC (CoCo 3)** for the CoCo 3, each *Imported* or *Not
  imported*.
- **Import…** and **Import a Folder…** add your own copies (see
  [BIOS and ROMs](#bios-and-roms)).

## The settings file

The settings file is `settings.ini` (see [Files and folders](#files-and-folders)).
Each setting above names its key. The others are `cpu` (the processor) and
`window_width` and `window_height`.

# Debugger

The Debugger (<kbd>F12</kbd>) stops the machine and shows what it is doing. It has
two tabs, and closing it lets the machine go on as it was.

| Button | Does |
|---|---|
| Pause (<kbd>F5</kbd>) | Stops before the next instruction, or runs on |
| Step (<kbd>F7</kbd>) | One instruction |
| Step Over (<kbd>F8</kbd>) | Over a subroutine call |
| Step Out (<kbd>Shift+F8</kbd>) | On until the routine returns |
| Step Line (<kbd>F6</kbd>) | On to a different C source line |
| Step Over Line (<kbd>Shift+F6</kbd>) | The same, but a call on the line runs to completion |
| Clear Breakpoints | Removes every breakpoint |
| Load Symbols | Reads an LWASM `--symbol-dump` file |
| Clear Symbols | Drops loaded symbols, keeping the built-ins |
| Load C Source | Reads CMOC `.lst` listings |
| Clear C Source | Drops the loaded C source |
| Help | Opens the topic of the tab that shows |

<!-- context: debugger/cpu -->
## CPU

The 6809 or 6309 disassembled, registers, a memory view (type an address after
**Address:** and press **Go**) and a decoded video pane. Click a line to toggle
its breakpoint. The built-in symbols are the Color BASIC ROM's entry points
(`POLCAT`, `CHROUT`, `CSRDON` and so on).

With **Load C Source** and a program built by [CMOC](http://sarrazip.com/dev/cmoc.html)
with `cmoc -i` (which keeps the listing and linker map), a source pane marks the
current line, **Step Line** and **Step Over Line** go by C statement, a click on a
line sets a breakpoint, and a Locals pane shows parameters and locals by name and
value. For a program with several `.c` files, pick all their `.lst` files at once;
loading adds each module rather than replacing what was loaded.

<!-- context: debugger/fujinet -->
## FujiNet packets

Every DriveWire packet between the CoCo and FujiNet, both ways. **Hide READY
probes** hides the `E2 00` that fujinet-lib asks before every call, and **Clear**
empties the list.

# Files and folders

The app keeps its settings in one file, and everything else in its data
folder:

<!-- only: linux -->

On Linux, the settings are `~/.config/fujinet-go-coco/settings.ini` and
the data is in `~/.local/share/fujinet-go-coco` (or under
`$XDG_CONFIG_HOME` and `$XDG_DATA_HOME`). The GNOME and KDE apps share them.
As a Flatpak, they are under `~/.var/app/`, in the app's own folder.

<!-- end -->

<!-- only: windows -->

On Windows, the settings are `%APPDATA%\fujinet-go-coco\settings.ini` and
the data is in `%LOCALAPPDATA%\fujinet-go-coco`.

<!-- end -->

<!-- only: macos -->

On a Mac, the settings are `~/.config/fujinet-go-coco/settings.ini` and
the data is in `~/.local/share/fujinet-go-coco`.

<!-- end -->

In the data folder:

| Folder | Has |
|---|---|
| `roms` | the Tandy ROMs you imported |
| `carts` | imported cartridges |
| `fujinet` | FujiNet's settings, its SD folder (`SD`) and its data |

# Troubleshooting

- **The machine does not start**: its ROMs are not imported. See
  [BIOS and ROMs](#bios-and-roms).
- **RGB is missing from Display**: the machine is a CoCo 2; RGB is a CoCo 3
  connector.
- **Unsupported media**: only `.dsk`, `.os9`, `.vdk`, `.cas` and `.wav` go to
  FujiNet, and `.rom` and `.ccc` are cartridges.
- **A port is taken**: another program is using 65504 or 64002. Close the other
  FujiNet, or point the app at it with **Use external FujiNet**.
- **Keys don't reach the machine**: a dialog or another window is in front, or
  the key is one the app keeps (see
  [Controllers and keys](#controllers-and-keys)).
- **A gamepad does nothing**: Gamepads may be off in Preferences, and a game has to
  read the joystick ports.

# About and licences

FujiNet Go CoCo is free software under the GNU General Public License, version 3
or later. Copyright © 2026 Thomas Cherryhomes.

- **XRoar** (the Dragon and CoCo emulator, GPL-3.0-or-later) is the machine.
- **The FujiNet firmware** (GPL-3.0-or-later) runs inside the app.
- The Tandy ROMs are Tandy's copyrighted firmware; see `COMPLIANCE.md`. "Tandy"
  and "Color Computer" are trademarks of their owners, used to name the machine
  emulated; this project is not affiliated with them.

[fujinet.online](https://fujinet.online/) ·
[fujinet-go-coco-desktop](https://github.com/FujiNetWIFI/fujinet-go-coco-desktop)
