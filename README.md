# NXInputRelay

NXInputRelay extends [RecurCIV](https://github.com/bbongpark/RecurCIV) and takes inspiration from [sys-botbase](https://github.com/olliz0r/sys-botbase/) with
authenticated network input for Nintendo Switch homebrew. Using this, you can send controller, keyboard, on-screen inputs to Nintendo Switch 1 and 2 over the network. A client on the
local network can send controller, keyboard, and experimental absolute-touch
commands either to the console, like Nintendo Switch 1, running NXInputRelay (`sw1`) or through its USB
device connection to another console like Nintendo Switch 2 (`sw2`).

The repository contains two implementations:

- [`nro/`](nro/README.md) — foreground NXInputRelay application, based on
  RecurCIV, with keyboard, controller, macro, and Civilization VII automation
  features. Its
  network listener uses TCP port 6001.
- [`sysmodule/`](sysmodule/README.md) — headless Atmosphere sysmodule that
  starts at boot and listens on TCP port 6002.

The bundled Python client is at
[`nro/tools/nxinputrelay_net.py`](nro/tools/nxinputrelay_net.py). For the complete
command reference, run:

```powershell
python .\nro\tools\nxinputrelay_net.py --list-inputs
```

**Examples for using the sysmodule version**:
If you are using the NRO version, the port is 6001
**To use macro script on Switch 2 via Switch 1. Switch 2 docked <- USB A cable to USB C <- Switch 1 device's USB C (not dock):**
```powershell
python .\nro\tools\nxinputrelay_net.py 192.168.137.1 --target sw2 --port 6002 --macro ".\nro\examples\wired_connection_macro_s2.txt"
```
**To use interactive mode on Switch 1:**
```powershell
python .\nro\tools\nxinputrelay_net.py 192.168.137.1 --target sw1 --port 6002
```
**To use one command on Switch 1:**
```powershell
python .\nro\tools\nxinputrelay_net.py 192.168.137.1 --target sw1 --port 6002 click a
```

**First-time token setup:**

```powershell
python .\nro\tools\nxinputrelay_net.py
```

If `network-token.txt` is not beside the client, it offers to generate a unique
token and prints the exact SD-card destination. Copy the generated file to
`/switch/NXInputRelay/network-token.txt`, keep the local copy beside the client,
then restart NXInputRelay before connecting.

**To enable the sysmodule on Atmosphere, press L1 + D-pad Down + R3, go to Sysmodules, go to nxinputrelay and press Y to enable (it should show a Home icon). Restart to take effect. Note that this module consumes ~1.32MB ram, you cannot enable some other sysmodules together with this (like sys-botbase).**

## Security

The network service is disabled until
`sdmc:/switch/NXInputRelay/network-token.txt` contains an 8–64 character
printable token without spaces. Existing installations may retain the token at
`sdmc:/switch/RecurCIV/network-token.txt`; both services use that location only
as a fallback when the new path is absent. Other legacy macros, settings, and
diagnostics remain compatible.
The protocol is intended for a trusted, isolated local network; it does not
provide transport encryption.

Generated logs, credentials, compiled binaries, release archives, and local
build directories are excluded from version control.

## Building and installing

See the component documentation for current build and installation details:

- [NRO documentation](nro/README.md)
- [Sysmodule documentation](sysmodule/README.md)
- [Network protocol and setup](nro/docs/network-input.md)

The sysmodule and NRO should not own the USB device stack simultaneously. See
the sysmodule documentation for the required enable/disable and reboot steps.

## License and attribution

This distribution is licensed under GPL-3.0-only while preserving the
licenses and notices of incorporated work. See [LICENSE](LICENSE),
[COPYING](COPYING), [LICENSING.md](LICENSING.md), and [CREDITS.md](CREDITS.md).
Additional third-party notices and corresponding source are retained under
`nro/licences/`, `nro/third_party/`, and `nro/upstream/`.
