# mceir — Media Center IR remote for Windows 11 on ARM64

`mceir` brings a Windows Media Center infrared receiver and remote back to life
on **Windows 11 ARM64**, which no longer ships a driver for them. The receiver
is bound to Microsoft's built-in **WinUSB** driver, and a small user-mode
program reads its USB traffic, decodes the remote's **RC-6** signals and turns
each button into a keystroke or a volume change. No kernel driver, no test
signing, no third-party binaries.

It targets the **Philips eHome MCE infrared transceiver** (`USB\VID_0471&PID_060C`,
which may show up as *"BB+ Dongle(e.d)"*) and the standard **Microsoft Media
Center remote**, whose 57 buttons are all mapped out of the box.

- [Why this exists](#why-this-exists)
- [Features](#features)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Building](#building)
- [Binding the receiver to WinUSB](#binding-the-receiver-to-winusb)
- [Running mceir](#running-mceir)
- [Starting automatically](#starting-automatically)
- [Key map](#key-map)
- [Logs](#logs)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [Tests](#tests)
- [Limitations](#limitations)
- [Status](#status)
- [Credits](#credits)

---

## Why this exists

The transceiver identifies itself to Windows with the Microsoft Consumer-IR
compatible ID, in its MS OS descriptor:

```text
USB\MS_COMP_USBCIR&MS_SUBCOMP_IR2CMPT
USB\MS_COMP_USBCIR
```

On x64 Windows that ID loads the eHome infrared stack (`irbus.sys`,
`hidir.sys`). On Windows 11 ARM64 **nothing claims it**: no INF in
`C:\Windows\INF` or the DriverStore matches `MS_COMP_USBCIR`, and the eHome
drivers exist nowhere on the system. The receiver enumerates perfectly and
then sits at **Code 28** (`CM_PROB_FAILED_INSTALL`). The old x64 driver cannot
help, because an ARM64 kernel does not load x64 drivers.

### The misleading "working properly"

A common attempted fix is to force-install some HID driver from Device
Manager's *Update driver → Let me pick* list. Device Manager then reports
*"This device is working properly"* — but nothing is running. The selected
section is a null-service HID section applied to a USB device, so the
device's `Service` registry value is empty and no driver is loaded at all.
`setupapi.dev.log` gives it away:

```text
!    pol:  Selected driver node does not match this device (force-install)
!    dvi:  Add Service: NULL Controlling service.
```

So **check the `Service` value, not the status text** — see
[Troubleshooting](#troubleshooting).

## Features

- Every button of the Microsoft MCE remote does something; any button can be
  rebound or unbound in a plain-text config file.
- Volume up/down and mute act on the **current default audio output** through
  Core Audio, so they work even while an elevated window has focus.
- Navigation and volume buttons auto-repeat while held.
- Survives unplug/replug and hubs or docks dropping off the bus: it waits,
  backs off, and reattaches by itself.
- Ignores presses the receiver buffered while nobody was listening (before
  startup, or while the machine slept), so a stale *Power* press cannot send
  the machine straight back to sleep.
- Runs as a **Windows service** (recommended) or a logon Scheduled Task, with
  no console window and a size-capped log.
- Receive only — the transceiver's IR blaster outputs are not used.

## Requirements

**To run**

- Windows 11 on ARM64. Developed and tested on build 28000 (26H1).
- A Philips eHome MCE transceiver, `USB\VID_0471&PID_060C`. Other MCE
  transceivers speak the same protocol and may work with the INF's hardware ID
  changed; that is untested.
- A remote that speaks RC-6 mode 6A — the Microsoft MCE remote, or one of the
  many MCE-compatible remotes that send the same `0x800F` codes. Other RC-6 6A
  remotes decode too and just need key map entries.

**To build**

- An ARM64 Windows machine with Visual Studio 2022 or later, including the
  *MSVC ARM64 build tools* with the native ARM64-hosted compiler
  (`VC\Tools\MSVC\<ver>\bin\Hostarm64\arm64\cl.exe`).
- A Windows SDK that has `winusb.h`.
- Only for the optional self-signed driver package (`sign.ps1`): `signtool`
  and `Inf2Cat` from the Windows Kits (`infverif` from the WDK is used if
  present).

## Quick start

From PowerShell in the repository root:

1. **Build** (and run the tests):

   ```powershell
   .\build.ps1 -Test
   ```

2. **Bind the receiver to WinUSB** — see
   [Binding the receiver to WinUSB](#binding-the-receiver-to-winusb). With
   Secure Boot on, use Option B.

3. **Try it** in a console and press some buttons:

   ```powershell
   .\build\ARM64\Release\mceir.exe --print -v
   ```

4. **Install it** to start automatically, from an elevated prompt:

   ```powershell
   .\install-service.ps1
   ```

## How it works

```text
  remote ~~ 36 kHz IR ~~>  Philips eHome transceiver  (USB, VID_0471 PID_060C)
                              interrupt OUT 0x01 / IN 0x82, 16-byte packets
                                           |
                                    winusb.sys  (inbox, Microsoft-signed)
                                           |  device interface {65216FE6-...}
  mceir.exe  ------------------------------+----------------------------------
    device.cpp      open by interface GUID, initialise firmware, overlapped reads
    mce_stream.cpp  MCE byte stream  ->  pulse/space samples (microseconds)
    rc6.cpp         samples  ->  RC-6 mode 6A frame  ->  scancode + toggle
    main.cpp        press / repeat logic, stale-press filter, reconnect loop
    keymap.cpp      scancode  ->  virtual key  ->  SendInput
    audio.cpp       volume and mute  ->  Core Audio on the default output
    service.cpp     service mode: keeps a per-user copy running in the console
```

### The USB side

`mceir` finds the receiver by the **device interface GUID**
`{65216FE6-9E29-41CB-9ABD-4CBE9CAD8AA4}`, not by hardware ID, so it does not
care which INF bound WinUSB as long as that GUID is registered.

The receiver has two **interrupt** endpoints — `0x01` OUT and `0x82` IN, 16-byte
packets, 1 ms interval — and a vendor-specific interface (class, subclass and
protocol all `0xFF`). On opening it, `mceir` sends:

| Bytes | Command |
|---|---|
| `00 FF AA` | resume (wake the firmware) |
| `FF 0B` | get firmware revision |
| `9F 0D` | get receive timeout |
| `9F 15` | get receive sensor |
| `9F 0C 07 D0` | set receive timeout to 100 ms (2000 × 50 µs) |

The IN pipe has one overlapped read always in flight, with no pipe timeout and
short packets completing the read immediately. Re-issuing the read on every
tick would drop whatever arrived in between. The main loop waits on it in
50 ms ticks so it can notice idle periods and stop requests.

If a read fails, the device is closed and reopened. A receiver that is
dropping off the bus can stay enumerated for several seconds while every read
fails, so reopening is backed off: 1 s, doubling to 8 s while sessions keep
failing before delivering any data. If the device has gone, `mceir` polls
every 3 s and reports the reason once, not on every attempt.

### The MCE byte stream

The IN pipe carries two kinds of packet, told apart by the first byte:

- **IR data:** a header `0x80 | n` followed by `n` sample bytes. Each sample is
  one run of carrier or silence: bit 7 says which, bits 0–6 give the length in
  **50 µs** units. A run longer than 127 units is split across several bytes of
  the same polarity, so runs are merged before decoding.
- **Command replies:** a port byte (`0xFF` system, `0x9F` IR) and a command
  byte, followed by a command-specific number of data bytes.

`0x9F` is the IR port's prefix, but `0x9F & 0xE0 == 0x80`, so it also passes
the IR-data test. The protocol resolves the clash in the command's favour,
which means **an IR data packet carries at most 30 samples, never 31**. The
parser checks for `0x9F` *before* the IR-data mask. Without that, every
firmware reply — including the `9F 01` end-of-signal marker that closes each
burst — is read as a 31-sample packet and swallows the 31 bytes behind it.

A burst ends at `9F 01`, or when a 50 ms read tick passes with nothing new,
whichever comes first. Zero-length samples are discarded as noise, as the Linux `mceusb` driver
does.

### RC-6 mode 6A decoding

An RC-6 frame is built from a 444 µs time unit (16 cycles of the 36 kHz
carrier):

```text
leader    6 units carrier + 2 units silence
header    start bit (always 1) + 3 mode bits (110 = mode 6)
trailer   1 bit at double width
body      8, 16, 20, 24 or 32 bits
          then at least 6 units of silence
```

Bits are bi-phase: a 1 is carrier then silence, a 0 the other way round.
Rather than a transition-driven state machine, `rc6.cpp` expands every sample
into the number of 444 µs half-bit **slots** it covers and then reads the frame
by position. The double-width trailer then needs no special case.

Two details that cost real debugging time:

- **The trailer bit is not a length selector.** Mode 6A is variable length,
  and the length is however many bits the remote actually sent. The MCE remote
  sends a 32-bit body with the trailer *clear*; reading the trailer as a
  16-vs-32 selector truncated every frame at the customer code. The decoder
  reads bits until the slots run out and accepts the result only if the count
  is a legal 6A length.
- **Microsoft's remotes keep their toggle in the payload.** The 32-bit body is
  the `0x800F` customer code, then the toggle in **bit 15**, then the key. The
  toggle flips on every new press and stays put while a button is held. It is
  stripped from the scancode (`body & ~0x8000`) and kept separately, otherwise
  every other press would look like a different button.

### From scancode to action

- **Press or hold:** a frame with the same scancode and toggle as the previous
  one, less than 300 ms after it, belongs to a held button. A held button
  repeats only if its binding allows it: first after 400 ms, then at most every
  100 ms.
- **Stale presses:** the receiver keeps IR it picked up while nobody was
  reading and hands it over on the first read. That once showed up as *Power*
  decoded 87 ms after startup, with the next real press 85 s later. Frames
  decoded within **500 ms** of opening the device, or after this process was
  frozen for over 2 s (sleep, or Modern Standby suspending desktop apps), are
  dropped.
- **Volume and mute** go to the default audio output through Core Audio
  (`IAudioEndpointVolume`). The output is looked up on every key press, so
  switching the default device to headphones is followed immediately. Because
  this is not input injection, it works whichever window has focus.
- **Everything else** is a key press and release sent with `SendInput`,
  flagged as an extended key where Windows expects one (arrows, navigation
  block, media keys). Windows blocks input from a normal process into an
  elevated window (UIPI), so those keys do nothing while, for example, Task
  Manager has focus. `mceir` logs a plain explanation the first time it
  happens.

### Service mode

A Windows service runs in **session 0**, which has no desktop the user can
see: `SendInput` from there lands nowhere. So the service
(`mceir.exe --service`, LocalSystem, automatic start) does not handle the
remote itself. It is a supervisor that keeps one copy of the program — the
**agent** (`mceir.exe --agent`) — running in the session of the user logged on
at the console:

- The agent is started with that **user's own token** (`WTSQueryUserToken`) and
  environment, so it has exactly the rights a logon task would give it, and
  logs into the user's own profile.
- No agent runs until someone logs on. It starts on the logon notification,
  moves to the new session on a fast user switch, and is restarted if it dies:
  after 2 s, doubling to 60 s while it keeps dying young.
- After a logoff it is **not** restarted into the dying session. Exit code
  `0x40010004`, or `0xC000026B` for an agent started too late, marks the
  session as ending; the next logon starts a fresh agent.
- The agent inherits exactly two handles: a "please exit" event and a handle
  on the service process. It exits when either fires, so stopping the service
  stops it cleanly, and killing the service cannot leave an orphan holding the
  receiver.
- The Service Control Manager restarts the service itself if it dies: after
  5 s, 5 s, then every 60 s.

## Building

```powershell
.\build.ps1                     # Release build
.\build.ps1 -Test               # build, then build and run the tests
.\build.ps1 -Configuration Debug
.\build.ps1 -Clean -Test        # wipe build\ first
```

`build.ps1` drives `cl.exe` and `link.exe` directly, without MSBuild. It finds
the toolset by enumerating every Visual Studio installation and keeping one
that actually ships the ARM64-hosted ARM64 compiler (`vswhere -latest` can
report a product with no C++ tools at all). Release builds use the static CRT,
so `mceir.exe` runs on a machine with no redistributable installed.

Output: `build\ARM64\Release\mceir.exe`. The program is linked
`/SUBSYSTEM:WINDOWS`, so it never opens a console window of its own; run from
a terminal, it attaches to that terminal's console and prints there.

## Binding the receiver to WinUSB

The receiver must be driven by `winusb.sys`, with the device interface GUID
`{65216FE6-9E29-41CB-9ABD-4CBE9CAD8AA4}` registered for it. There are two ways
to get there.

| | Option A: `mceir.inf` | Option B: inbox WinUSB |
|---|---|---|
| What is installed | this repo's INF + a self-signed catalog | Microsoft's own `winusb.inf` |
| Secure Boot / Memory Integrity on | **rejected** (see below) | works |
| Effort | two scripts | Device Manager + one registry value |

### Option A — `mceir.inf` with a self-signed catalog

```powershell
.\sign.ps1        # validate the INF, build and sign the catalog (no admin needed)
.\install.ps1     # elevated: trust the certificate, import and install the package
```

- `sign.ps1` creates a self-signed code-signing certificate in
  `CurrentUser\My` (or reuses it), stages `build\ARM64\Release\package\`,
  builds `mceir.cat` with `Inf2Cat`, and signs the catalog and `mceir.exe`.
  `-Timestamp` adds a timestamp.
- `install.ps1` puts the certificate in the machine's **Root** store (so the
  chain validates) and **TrustedPublisher** store (so the install is not
  prompted). It then deletes any earlier copy of the package and imports it
  again. That order matters, because the driver store records a package's
  signer at import time and never re-checks it: a package imported before the
  certificate was trusted stays *"Signer Name: Unknown"* for good.
- `.\install.ps1 -Uninstall` removes the package and the certificate
  (`-KeepCertificate` keeps the certificate). The receiver goes back to
  Code 28.

The package installs no binary of its own — it binds the inbox,
Microsoft-signed `winusb.sys` — but Windows still checks the catalog. **With
Secure Boot and Memory Integrity on, Windows accepts only Microsoft-signed
packages:** a fresh import fails with `0x800B0109` (*"does not belong to
Trusted Root Certificates, and Code Integrity is enforced"*), and a
self-signed package may refuse to start with Code 52. Trusting the certificate
does not change that, and test signing is ignored while Secure Boot is on.
On such a machine, use Option B, or have Microsoft sign the package:
`make-cab.ps1` builds the attestation-signing submission
(`build\ARM64\Release\submission\mceir.cab`), which needs a Partner Center
account and an EV code-signing certificate.

### Option B — inbox WinUSB (works with Secure Boot)

This uses only Microsoft-signed components.

1. In **Device Manager**, open the receiver (an unknown device, *BB+ Dongle*,
   or whatever it shows as) → **Update driver** → **Browse my computer** →
   **Let me pick from a list** → **Universal Serial Bus devices** →
   **WinUsb Device**, and accept the warning.
2. In an **elevated** PowerShell, register the interface GUID and restart the
   device:

   ```powershell
   $dev = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_0471&PID_060C\*'
   $key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
   New-ItemProperty -Path $key -Name DeviceInterfaceGUIDs -PropertyType MultiString `
       -Value '{65216FE6-9E29-41CB-9ABD-4CBE9CAD8AA4}' -Force | Out-Null
   pnputil /restart-device "$($dev.InstanceId)"    # or unplug and replug it
   ```

WinUSB reads `DeviceInterfaceGUIDs` when the device starts and registers the
interface, which is all `mceir` looks for.

### Checking the binding

```powershell
$dev = Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_0471&PID_060C\*'
$dev | Format-List Status, Class, FriendlyName, InstanceId
(Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)").Service   # must be WINUSB
```

Status `OK` with `Service` = `WINUSB` is the goal. See
[The misleading "working properly"](#the-misleading-working-properly) for why
the status alone is not enough.

## Running mceir

Run it in a console first to see that buttons arrive:

```powershell
.\build\ARM64\Release\mceir.exe --print -v
```

```text
14:02:11.512  Transceiver ready: IN 0x82
14:02:15.031  OK            800F0422  -> ENTER
14:02:16.204  VolumeUp      800F0410  -> VOLUME_UP
```

`--print` reports buttons without acting on them, so it is also how you find
the scancode of a button you want to rebind. Ctrl+C stops it.

| Option | Meaning |
|---|---|
| *(none)* | decode and act on buttons — the normal mode |
| `--print` | decode and report buttons, send nothing |
| `--dump-keymap` | print the active key map (built-in + `mceir.conf`) and exit |
| `--config <path>` | key map overrides (default: `mceir.conf` beside the exe) |
| `--log <path>` | write the log to a file instead of the console |
| `-v`, `--verbose` | also report device details and decode failures |
| `--trace` | also dump raw USB traffic and every IR sample |
| `-q`, `--quiet` | print nothing (a `--log` file still gets normal lines) |
| `--rx-timeout <ms>` | silence that ends a burst in the firmware, 1–3276 (default 100) |
| `--no-rx-timeout` | leave the firmware's receive timeout as it is |
| `--service` | run under the Service Control Manager; other options pass to the agent |
| `-h`, `--help` | usage |

`--agent`, `--stop-handle` and `--parent-handle` are internal: the service
uses them to start its agent.

Only one process can open the receiver at a time. To experiment while the
service is installed, stop it first (`Stop-Service mceir`, elevated).

## Starting automatically

Nothing responds unless an `mceir` process is running and has the receiver
open. Pick **one** of the two methods below; they refuse to coexist, because
two copies would fight over the receiver.

### Option 1 — Windows service (recommended)

From an **elevated** prompt, after `.\build.ps1`:

```powershell
.\install-service.ps1               # install, or update after a rebuild
.\install-service.ps1 -Uninstall    # remove (logs are kept)
```

It removes the logon task if there is one, copies `mceir.exe` to
`C:\Program Files\mceir\` (where only administrators can replace a binary that
the service starts), registers the `mceir` service (automatic start,
restart-on-failure), and starts it. The service log directory,
`%ProgramData%\mceir`, is locked down to SYSTEM and Administrators with
read-only access for users, because the service, running as SYSTEM, rotates
its log there.

Check it:

```powershell
Get-Service mceir
Get-CimInstance Win32_Process -Filter "Name='mceir.exe'" | Select-Object ProcessId, SessionId, CommandLine
```

You should see the service (session 0) and an agent in your session.

### Option 2 — logon Scheduled Task

Uses the signed copy that `sign.ps1` puts in `build\ARM64\Release\package\`.
From an **elevated** prompt:

```powershell
.\install-autostart.ps1              # normal privileges
.\install-autostart.ps1 -Elevated    # run elevated: keys also reach elevated windows
.\install-autostart.ps1 -Uninstall
```

The task starts `mceir.exe --log "%LOCALAPPDATA%\mceir\mceir.log"` 10 seconds
after logon, with no time limit and three restarts a minute apart. It refuses
to install while the service is installed.

`-Elevated` is the only way to make *all* keys work over elevated windows,
since the service's agent always runs with normal rights. The cost is a
permanently elevated process, so it is off by default.

Without any admin rights at all, a hidden launcher in the Startup folder
works too: `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\mceir.vbs`
running `mceir.exe --log ...` with window style 0. Both installers remove it,
since it would be a second copy.

## Key map

Every button of the Microsoft MCE remote is bound. Buttons with no natural
desktop meaning get function keys (the coloured and "jump to" buttons) or
mnemonic letters (the modal ones), which are easy to bind to actions in a media
player.

| Scancode | Button | Sends | Repeats |
|---|---|---|---|
| `800F0400`–`0409` | Num0–Num9 | `0`–`9` | no |
| `800F040A` | Clear | Backspace | no |
| `800F040B` | Enter | Enter | no |
| `800F040C` | Power | **Sleep** | no |
| `800F040D` | Start | Windows key | no |
| `800F040E` | Mute | Volume mute | no |
| `800F040F` | Info | `I` | no |
| `800F0410` | VolumeUp | Volume up | yes |
| `800F0411` | VolumeDown | Volume down | yes |
| `800F0412` | ChannelUp | Page Up | yes |
| `800F0413` | ChannelDown | Page Down | yes |
| `800F0414` | Forward | Next track | yes |
| `800F0415` | Rewind | Previous track | yes |
| `800F0416` | Play | Play/Pause | no |
| `800F0417` | Record | `R` | no |
| `800F0418` | Pause | Play/Pause | no |
| `800F0419` | Stop | Stop | no |
| `800F041A` | Skip | Next track | no |
| `800F041B` | Replay | Previous track | no |
| `800F041C` | Hash `#` | `N` | no |
| `800F041D` | Star `*` | `F` | no |
| `800F041E` | Up | Up | yes |
| `800F041F` | Down | Down | yes |
| `800F0420` | Left | Left | yes |
| `800F0421` | Right | Right | yes |
| `800F0422` | OK | Enter | no |
| `800F0423` | Back | Escape | no |
| `800F0424` | DVDMenu | F12 | no |
| `800F0425` | LiveTV | F11 | no |
| `800F0426` | Guide | F5 | no |
| `800F0427` | Aspect | `Z` | no |
| `800F0432` | Visualize | `V` | no |
| `800F0433` | Slideshow | `W` | no |
| `800F0434` | Eject | `E` | no |
| `800F0446` | TV | F6 | no |
| `800F0447` | Music | F7 | no |
| `800F0448` | RecordedTV | F8 | no |
| `800F0449` | Pictures | F9 | no |
| `800F044A` | Videos | F10 | no |
| `800F044D` | Title | `T` | no |
| `800F0450` | Radio | `K` | no |
| `800F045A` | Subtitle | `C` | no |
| `800F045B` | Red | F1 | no |
| `800F045C` | Green | F2 | no |
| `800F045D` | Yellow | F3 | no |
| `800F045E` | Blue | F4 | no |
| `800F046E` | PlayPause | Play/Pause | no |
| `800F0481` | PlayPause2 | Play/Pause | no |

> **Power sends Sleep.** A stray reflection of a Power press can suspend the
> machine. To keep the button recognised but harmless, add
> `800f040c = NONE` to `mceir.conf`.

### `mceir.conf`

Overrides live in `mceir.conf` beside `mceir.exe` (for the service:
`C:\Program Files\mceir\mceir.conf`; `install-service.ps1` copies a
`mceir.conf` from the repository root there if one exists).
`mceir.conf.sample` is a commented starting point.

```text
# <scancode hex> = <key name> [repeat|norepeat]
800f040c = NONE            # recognise Power, send nothing
800f0426 = F5              # Guide -> F5
800f041a = RIGHT repeat    # Skip seeks forward instead of next track
0x12345678 = A             # a button from another RC-6 6A remote
```

- Scancodes are hex, with or without `0x`, at most 8 digits. Anything that is
  not cleanly hex is rejected with a message, not half-parsed.
- Key names: `UP DOWN LEFT RIGHT ENTER RETURN ESCAPE ESC BACKSPACE TAB SPACE
  HOME END PAGEUP PAGEDOWN INSERT DELETE LWIN RWIN APPS SLEEP VOLUME_UP
  VOLUME_DOWN VOLUME_MUTE MEDIA_PLAY_PAUSE MEDIA_STOP MEDIA_NEXT_TRACK
  MEDIA_PREV_TRACK LAUNCH_MEDIA_SELECT BROWSER_BACK BROWSER_FORWARD
  BROWSER_HOME`, `F1`–`F24`, a single letter `A`–`Z` or digit `0`–`9`, or
  `NONE`. Names are case-insensitive.
- `repeat` / `norepeat` overrides auto-repeat; otherwise a rebound button keeps
  its built-in setting, and a new one does not repeat.
- `#` or `;` starts a comment.
- Buttons not in the map are logged as `Unmapped button: scancode ...` so you
  can copy the code into the file. `--print` shows them too.

The map is read at startup: restart `mceir` (or the service) after editing,
and check the result with `mceir.exe --dump-keymap`.

## Logs

| Who | Where |
|---|---|
| console run | the console (`--log <path>` to write a file) |
| service agent, logon task | `%LOCALAPPDATA%\mceir\mceir.log` |
| the service itself | `%ProgramData%\mceir\service.log` |

- Each line in a log file is stamped with the date and time.
- At 1 MB a log is renamed to `<name>.old`, replacing the previous one, and a
  new file is started.
- Files are shared for reading, so `Get-Content -Wait` works while `mceir`
  runs.
- At the default level a log holds only events: start/stop, the receiver
  appearing and disappearing, errors and the first refused keystroke. Button
  presses are logged with `-v`; raw USB traffic with `--trace`. For the
  service, options after `--service` on its command line are passed to the
  agent. From an elevated `cmd` prompt:
  `sc config mceir binPath= "\"C:\Program Files\mceir\mceir.exe\" --service -v"`,
  then restart the service. Re-running `install-service.ps1` resets it.

## Troubleshooting

**Nothing happens when I press buttons.**

1. Is `mceir` running? `Get-Service mceir` should say `Running`, and there
   should be an `mceir.exe --agent` process in your session.
2. Is the receiver there?
   `Get-PnpDevice -PresentOnly | Where-Object InstanceId -like 'USB\VID_0471&PID_060C*'`
   — if that returns nothing, the receiver is not on the bus (see the next
   item).
3. Is it bound? `Service` must be `WINUSB` ([Checking the binding](#checking-the-binding)).
4. Run `mceir.exe --print -v` (service stopped) and watch for decoded frames.

**The receiver keeps disappearing, or shows up as "Unknown USB Device
(Device Descriptor Request Failed)".** The receiver is drawing power but has
stopped answering USB enumeration, or the hub or dock it hangs off has dropped
off the bus and taken it along. No software can reach it in that state:
replug it, or restart the dock (a Thunderbolt dock whose PCIe link comes back
broken can leave its USB controller missing until the dock is restarted).
`mceir` reattaches by itself once the receiver enumerates again; the log shows
`Waiting for the transceiver...` and later `Transceiver ready`.

**"No MCE transceiver found: our device interface is not registered."** Either
the receiver is unplugged, or it is not bound to WinUSB with the interface
GUID — see [Binding the receiver to WinUSB](#binding-the-receiver-to-winusb).

**"Could not open the device: Access is denied. (0x00000005) Another process
already has the transceiver open."** Two copies are running — usually the service and a
console run, or a leftover logon task. Keep one.

**Buttons work except while Task Manager, an admin console or a UAC prompt is
focused.** That is UIPI: Windows refuses injected input from a normal process
into an elevated window. Volume and mute still work. Only an elevated `mceir`
(`install-autostart.ps1 -Elevated`) drives elevated windows.

**The machine goes to sleep on its own.** Power is mapped to Sleep. Add
`800f040c = NONE` to `mceir.conf`.

**A button does nothing in my media player.** Check the key it sends with
`--dump-keymap`, then either bind that key in the player or rebind the button
in `mceir.conf`.

**Device Manager says "This device is working properly" but nothing works.**
See [The misleading "working properly"](#the-misleading-working-properly):
check the `Service` value.

## Project layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | entry point, options, logging, press/repeat logic, reconnect loop |
| `src/device.cpp` | WinUSB discovery, pipe policies, firmware commands, overlapped reads |
| `src/mce_stream.cpp` | MCE packet framing into pulse/space samples |
| `src/rc6.cpp` | RC-6 mode 6A decoder |
| `src/keymap.cpp` | built-in key map, `mceir.conf` parser, `SendInput` |
| `src/audio.cpp` | volume and mute through Core Audio |
| `src/service.cpp` | Windows service: per-user agent supervisor |
| `src/mceir.h` | shared declarations |
| `test/rc6_test.cpp` | receive-chain tests: framing and decoding, no hardware needed |
| `test/keymap_test.cpp` | key name and `mceir.conf` parsing tests |
| `test/inject_probe.cpp` | manual diagnostic: shows whether injected volume keys move the system volume |
| `mceir.inf` | WinUSB binding for `USB\VID_0471&PID_060C` (Option A) |
| `mceir.conf.sample` | commented key map overrides |
| `build.ps1` | build, optionally with tests |
| `sign.ps1` | self-signed catalog for `mceir.inf`, signs `mceir.exe` |
| `install.ps1` | trust the certificate and install `mceir.inf` (elevated) |
| `install-service.ps1` | install or remove the Windows service (elevated) |
| `install-autostart.ps1` | install or remove the logon Scheduled Task (elevated) |
| `make-cab.ps1` | attestation-signing submission cab for Partner Center |

## Tests

`.\build.ps1 -Test` builds and runs two test programs. Neither needs the
hardware.

- **`rc6_test`** (23 checks) builds RC-6 frames, encodes them the way the
  firmware does (50 µs sample bytes in IR data packets), and pushes them
  through the real `MceStream` and `Rc6Decoder`. It covers MCE buttons with
  and without the toggle, 16/24/32-bit bodies, a frame **captured off the
  real receiver**, packets split across 1-, 7- and 13-byte reads, firmware
  replies and the end-of-signal marker in front of a frame, a zero-length
  sample inside a frame, and frames that must be rejected (no leader, wrong
  mode, Manchester violation, truncated).
- **`keymap_test`** (15 checks) covers key names (including `F1X` and `F25`
  being rejected) and `mceir.conf` parsing: the `0x` prefix, `NONE`,
  `repeat`/`norepeat`, new buttons, and the lines that must be rejected without
  changing anything (trailing garbage, a sign, more than 32 bits, unknown key
  names).

`test\inject_probe.cpp` is built but not run: it moves the real system volume.
Run `build\ARM64\test\inject_probe.exe watch <seconds>` by hand to check that
injected volume keys reach Windows.

## Limitations

- **Receive only.** The transceiver's IR blaster outputs are not used.
- **No wake from sleep.** The remote cannot wake the machine.
- **No logon screen, lock screen or UAC prompt.** Keystrokes come from a
  user-mode process in your session; elevated windows are reached only by an
  elevated `mceir` (volume and mute work regardless).
- **RC-6 mode 6A only.** RC-5, NEC and other protocols are not decoded.
- **One hardware ID.** `mceir.inf` lists only `USB\VID_0471&PID_060C`.

## Status

- Tested with a Philips eHome transceiver and a Microsoft MCE remote on
  Windows 11 ARM64 build 28000, bound through `mceir.inf` (Option A) and run
  both as a logon task and as the service.
- Option B (inbox WinUSB) binds the same driver and registers the same
  interface, but it has not been exercised with this receiver yet.
- The size of the firmware-revision reply (`FF 0B`) is assumed to be 2 data
  bytes; the Linux driver uses 4. It only matters at startup, inside the
  window where frames are dropped anyway.
- The logoff and resume-from-sleep handling has been checked by code review
  and unit-level reasoning only, not by an automated test.

## Credits

The transceiver protocol — packet framing, command sizes, the `0x9F` quirk — was
worked out from the Linux kernel's `mceusb` driver
(`drivers/media/rc/mceusb.c`) and checked against this receiver's own traffic.
No code from it is included. The RC-6 frame format is the publicly documented
Philips RC-6 protocol.
