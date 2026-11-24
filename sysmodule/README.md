# NXInputRelay sysmodule

This is the headless, boot-time counterpart to the NXInputRelay NRO. It keeps the
TCP 6002 authenticated input listener running independently of the foreground
application. Each authenticated client selects either Switch 1 local input or
the composite USB keyboard/gamepad/experimental digitizer connected to Switch 2.

## Build

With devkitPro installed at `C:\devkitPro`:

```powershell
python .\build.py
```

The script produces `nxinputrelay_sysmodule.nsp`, stages an installable tree at
`atmosphere\contents\4200000000006001\`, and creates
`nxinputrelay_sysmodule_v1.3.6-touch_atmosphere_4200000000006001.zip` for
convenient SD-card installation.

## Install
1. Download from the [Release](https://github.com/PaperChonChon/NXInputRelay/releases) page
2. Extract and copy this project's `atmosphere` folder to the root of the SD card and merge
   it with the existing `atmosphere` folder.
3. Run `python .\nro\tools\nxinputrelay_net.py` from the repository without a
   host. Approve token generation, then copy the generated
   `nro\tools\network-token.txt` to `/switch/NXInputRelay/network-token.txt` on the
   SD card. Keep the local copy for the client.
4. Fully reboot Atmosphere. The service starts at boot and listens on TCP 6002.
5. Inspect `/switch/NXInputRelay/network-sysmodule.log` and
   `/switch/NXInputRelay/usb.log` if it does not accept connections.

The included `toolbox.json` exposes the service as `sys-NXInputRelay` in compatible
Tesla sysmodule managers. Toggling it changes its boot state and requires a
full reboot before the change takes effect.

The sysmodule uses program ID `4200000000006001`. Change it in both
`nxinputrelay_sysmodule.json` and `build.py` if that ID conflicts with another
installed custom sysmodule.

For compatibility, the sysmodule reads `/switch/RecurCIV/network-token.txt`
only when the new NXInputRelay token path is absent. Logs remain under the
legacy RecurCIV data directory in this release.

## Switching between the two versions

Do not run the NRO while this sysmodule is active. Both versions require
exclusive access to `usb:ds`, so only one can own the USB keyboard/gamepad
device. To use the NRO again, delete or rename:

`/atmosphere/contents/4200000000006001/flags/boot2.flag`

Then fully reboot. Restoring `boot2.flag` and rebooting enables the sysmodule
again.

The service survives foreground application changes. It cannot receive network
commands while Horizon has powered Wi-Fi down in sleep, and it cannot keep USB
or networking alive through a full system power-off.

Port 6002 starts independently of `usb:ds`. If the foreground NRO owns USB,
the sysmodule keeps retrying USB every five seconds without blocking its TCP
listener. During that interval `sw1` local input remains available; `sw2`
authentication returns `ERR busy` until USB is acquired.

## Protocol

Use the same client and commands as the NRO. The sysmodule always listens on
port 6002, so specify that port when targeting Switch 1 (whose NRO default is
otherwise port 6001):

```powershell
python ..\nro\tools\nxinputrelay_net.py 192.168.137.249 --target sw1 --port 6002
python ..\nro\tools\nxinputrelay_net.py 192.168.137.249 --target sw2 --port 6002
```

`sw1` creates temporary `hid:dbg` virtual controller, keyboard, and touch
state consumed by Switch 1 itself. `sw2` retains the existing USB forwarding
path and adds an experimental single-contact absolute HID digitizer on interface
2 / endpoint 0x83. Host support is not guaranteed; an unenumerated digitizer
returns `ERR touch-unavailable` without making touch a prerequisite for
keyboard or gamepad readiness. Legacy
clients that send only `auth <token>` continue to select Switch 2.

The listener reads the same token and uses the same self-healing socket logic,
macro syntax, HOME/CAPTURE commands, and rotating diagnostic log as the NRO.
Keyboard commands accept the same case-insensitive descriptive names and
modifier chords as the NRO (`key up`, `key ctrl+a`, `key alt+f4`, and
`keyDown shift+left`) while retaining numeric HID usages such as `key 82`.
Run `python ..\nro\tools\nxinputrelay_net.py --list-keys` for the complete map.
Run `python ..\nro\tools\nxinputrelay_net.py --list-inputs` for the
controller, keyboard, and touch protocol, including `tap`, `touchDown`
(`tapDown` alias), `touchMove`, and `touchUp`.
The sysmodule writes `/switch/NXInputRelay/network-sysmodule.log` and rotates
it to `network-sysmodule.log.1`; these are separate from the NRO logs.
While IPv4 is available it sends an eight-byte UDP keepalive to the configured
local gateway every 15 seconds. Address loss/change rebuilds the BSD socket
service and listener instead of trusting a stale local socket descriptor.
The UDP keepalive is only an idle-prevention hint: an unreachable gateway is
logged and retried with backoff, but never disconnects a healthy TCP session.

## License

This source fork retains RecurCIV's GPL-3.0-only licensing and incorporated
notices. See `LICENSE`, `COPYING`, `LICENSING.md`, and `CREDITS.md`.
