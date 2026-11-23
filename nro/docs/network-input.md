# Network input

## Network Control page

The NRO home screen includes a fourth **Network Control** tile. It reports the
listener and Wi-Fi link, current and last target, client address, last command,
queue depths, keepalives and most recent recovery reason. An authenticated
session automatically opens this page. While the page reports **NETWORK MODE
ACTIVE**, local USB keyboard/controller pages are locked so they cannot race
the remote owner. Press `+` to request neutral cleanup and stop the session.

The listener remains available in the background while browsing idle menus.
Opening a local live/sending/controller operation makes new remote
authentication return `ERR busy`; menu browsing alone does not claim USB.

NRO diagnostics are written to `sdmc:/switch/RecurCIV/network-nro.log`.
At approximately 1 MiB, the previous `.1` backup is deleted, the current log
becomes `network-nro.log.1`, and a new current log is opened.

The listener checks the current IPv4 state every five seconds. Missing or
loopback-only addressing closes the listener; restoration or address changes
recreate the complete BSD socket service before accepting clients. While an
IPv4 link is usable, NXInputRelay sends an eight-byte UDP keepalive to the
configured local gateway every 15 seconds to discourage access-point idle
disconnects. This packet is local to the LAN and is not Internet traffic.
It is strictly best-effort: an unreachable gateway never closes a TCP client
or rebuilds the listener, and repeated failures back off to one retry per
minute.

NXInputRelay can accept authenticated, newline-delimited input commands over TCP.
The Python client chooses either `--target sw2` to forward them through the
existing USB keyboard/gamepad interfaces, or `--target sw1` to inject them locally through a
temporary `hid:dbg` virtual controller and keyboard. The
listener uses TCP port `6001`, so an installed sys-botbase can continue using
port `6000`.

After console wake, a long application suspension, or a return from background
focus, NXInputRelay discards stale TCP state and recreates the socket service and
port `6001`. If Wi-Fi has not reconnected yet, listener creation retries with a
1-second exponential backoff capped at 30 seconds. An active input session still
performs neutral HID cleanup before a new client can authenticate. During true
system sleep the NRO is suspended and cannot answer network requests; recovery
starts when execution resumes.

## Network diagnostics

NXInputRelay appends timestamped network diagnostics to
`sdmc:/switch/RecurCIV/network-nro.log`. The log records IP-address changes,
listener generations and file descriptors, accepted clients, authentication
failures/timeouts, send/receive/accept errors, recovery attempts, retry delays,
and a health snapshot every 60 seconds. Listener descriptor and `SO_ERROR`
health are checked every five seconds; a failed check requests recovery.

The log is flushed after every entry. At 1 MiB it rotates to
`network-nro.log.1`,
preserving one previous log. The broader USB log remains at
`sdmc:/switch/RecurCIV/usb.log`, but that file is recreated whenever the USB
service is initialized, so copy it before restarting NXInputRelay when investigating
an active failure.

## Install and connect

1. Copy the rebuilt `NXInputRelay.nro` to
   `sdmc:/switch/NXInputRelay/NXInputRelay.nro`.
2. Run `python .\tools\nxinputrelay_net.py` without a host. If
   `tools\network-token.txt` does not exist, approve the prompt to generate a
   unique token. The token value is not printed.
3. Copy the generated file to
   `sdmc:/switch/NXInputRelay/network-token.txt`. Keep the original beside
   `nxinputrelay_net.py`; the client loads it automatically.
4. Connect Switch 1 to the same isolated LAN as the controlling computer and
   start NXInputRelay. No Internet connection is used or required.
5. Connect to Switch 1 on TCP port 6001 and send either `auth TOKEN switch2\n`
   or `auth TOKEN switch1\n` within ten seconds. `switch2` authentication
   succeeds only while NXInputRelay's USB keyboard and gamepad are ready. `switch1`
   initializes a temporary local virtual controller and keyboard and does not
   require a USB connection to Switch 2. Both targets require that no local
   macro, sender, controller passthrough, or CIV operation owns input.

While authenticated, local NXInputRelay input producers are paused. Press `+` on
Switch 1, send `quit`, close the socket, or remain idle for five minutes to end
the session. NXInputRelay sends keyboard-up and a neutral gamepad report before
returning control. If the USB target disconnects during cleanup, reconnect it so
neutral delivery can finish.

Existing installations may leave the token at
`sdmc:/switch/RecurCIV/network-token.txt`. NXInputRelay checks that legacy path
only when the new token file is absent.

## Commands

Commands are ASCII lines terminated by `\n` (CRLF is also accepted).

| Command | Meaning |
|---|---|
| `ping` | Check the connection. |
| `status` | Return USB readiness and queue counts. |
| `click A` | Press A for 80 ms and release it. |
| `press A` / `release A` | Hold or release a gamepad button. |
| `setStick LEFT 32767 0` | Set a signed 16-bit stick position. |
| `tap 640 360` | Touch the center of the screen for 80 ms, then release. An optional hold time in milliseconds may follow. |
| `touchDown 640 360` / `tapDown 640 360` | Start and hold one touch contact. `tapDown` is an alias; `touchDown` is canonical because no tap has completed yet. |
| `touchMove 800 360` | Move the held contact to a new absolute coordinate. |
| `touchUp` | Release the held contact. |
| `key up` / `key 82` | Tap keyboard Up Arrow by descriptive name or USB HID usage. |
| `key a enter` | Tap multiple named keyboard keys in order. |
| `keyMod a 1` | Tap A with modifier bit 1 (Left Shift). Numeric usages remain accepted. |
| `keyDown up` / `keyUp` | Hold keyboard Up Arrow, then release it. An optional modifier byte may follow the key. |
| `key ctrl+a` | Tap a descriptive modifier chord. Multiple modifiers such as `ctrl+shift+escape` are accepted. |
| `keyDown shift+up` / `keyUp` | Hold a descriptive chord, then release every key and modifier. |
| `typeHex 48656c6c6f0a` | Type UTF-8 bytes supplied as hexadecimal. |
| `neutral` | Discard queued work and release keyboard/gamepad state. |
| `quit` | End the session and perform neutral cleanup. |

Buttons: `Y B A X L R ZL ZR MINUS PLUS L3 R3 HOME CAPTURE DUP DDOWN DLEFT DRIGHT`.
Keyboard names are case-insensitive. Supported names include `A`-`Z`, `DIGIT0`-`DIGIT9`,
`F1`-`F12`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `ENTER`, `ESCAPE`, `BACKSPACE`,
`TAB`, `SPACE`, `HOME`, `END`, `INSERT`, `DELETE`, `PAGEUP`, `PAGEDOWN`,
`CAPSLOCK`, `NUMLOCK`, `PRINTSCREEN`, `SCROLLLOCK`, `PAUSE`, and `MENU`.
Common abbreviations such as `ESC`, `BKSP`, `INS`, `DEL`, `PGUP`, and `PGDN`
are also accepted. Bare numbers remain HID usages from 4 through 231, so the
unambiguous `DIGIT0`-`DIGIT9` names select number-row keys.

Modifiers may be joined to a key with `+`. Generic `CTRL`, `SHIFT`, `ALT`, and
`GUI`/`WIN` names select the left modifier. Explicit left/right forms include
`LCTRL`, `RCTRL`, `LSHIFT`, `RSHIFT`, `LALT`, `RALT`, `LGUI`, and `RGUI`.
Use the complete offline reference at any time without connecting to a Switch:

```powershell
python .\tools\nxinputrelay_net.py --list-keys
```

In interactive mode, `help`, `help keys`, or `keys` prints the same key
reference. The complete controller/keyboard/touch command reference is available
without a network connection:

```powershell
python .\tools\nxinputrelay_net.py --list-inputs
```

Interactive `help inputs`, `inputs`, `help touch`, or `touches` prints
that reference. Touch uses the Switch native 1280x720 coordinate space:
`x=0..1279`, `y=0..719`.

For `--target sw1`, touch is injected natively through Switch 1's `hid:dbg`
service. For `--target sw2`, NXInputRelay exposes an experimental, single-contact
absolute USB HID digitizer on interface 2 / endpoint 0x83. Switch 2 acceptance
of a generic USB digitizer is not guaranteed; if the host does not enumerate
it, touch commands return `ERR touch-unavailable` while keyboard and gamepad
remain independently usable.

The bundled `tools/nxinputrelay_net.py` client can be used interactively:

```powershell
python .\tools\nxinputrelay_net.py 192.168.137.249
```

Or for one command:

```powershell
python .\tools\nxinputrelay_net.py 192.168.137.249 click A
python .\tools\nxinputrelay_net.py 192.168.137.249 type "Hello from Python"
python .\tools\nxinputrelay_net.py 192.168.137.249 --target sw1 click A
python .\tools\nxinputrelay_net.py 192.168.137.249 --target sw2 key ctrl+shift+escape
python .\tools\nxinputrelay_net.py 192.168.137.249 --target sw1 tap 640 360
python .\tools\nxinputrelay_net.py 192.168.137.249 --target sw2 tap 640 360
```

For longer typing jobs, use interactive mode or increase `--wait` so the client
does not disconnect before queued input finishes.

## Macro text files

Run a UTF-8 text macro with:

```powershell
python .\tools\nxinputrelay_net.py 192.168.137.249 --macro .\my-macro.txt
```

Blank lines and lines beginning with `#` are ignored. `delay` is measured in
seconds and accepts decimal values. Each command is allowed to finish before its
following delay begins. The runner stops at the first `ERR` response and socket
cleanup releases held inputs.

```text
click a
delay 1
click b
delay 0.5
click dright
delay 2
click a
```
