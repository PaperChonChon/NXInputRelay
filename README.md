# NXInputRelay

NXInputRelay extends [RecurCIV](https://github.com/bbongpark/RecurCIV) and takes inspiration from [sys-botbase](https://github.com/olliz0r/sys-botbase/) with
authenticated network input for Nintendo Switch homebrew. A client on the
local network can send controller, keyboard, and experimental absolute-touch
commands either to the console running RecurCIV (`sw1`) or through its USB
device connection to another console (`sw2`).

The repository contains two implementations:

- [`nro/`](nro/README.md) — foreground RecurCIV application with its original
  keyboard, controller, macro, and Civilization VII automation features. Its
  network listener uses TCP port 6001.
- [`sysmodule/`](sysmodule/README.md) — headless Atmosphere sysmodule that
  starts at boot and listens on TCP port 6002.

The bundled Python client is at
[`nro/tools/recurciv_net.py`](nro/tools/recurciv_net.py). For the complete
command reference, run:

```powershell
python .\nro\tools\recurciv_net.py --list-inputs
```

## Security

The network service is disabled until
`sdmc:/switch/RecurCIV/network-token.txt` contains an 8–64 character printable
token without spaces. The protocol is intended for a trusted, isolated local
network; it does not provide transport encryption.

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
