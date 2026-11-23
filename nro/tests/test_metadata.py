"""Check the built NRO embeds the intended icon and NACP metadata."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
icon = (root / 'assets' / 'nxinputrelay-icon.jpg').read_bytes()
nacp = (root / 'build' / 'NXInputRelay.nacp').read_bytes()
nro = (root / 'build' / 'NXInputRelay.nro').read_bytes()
assert icon.startswith(b'\xff\xd8') and icon.endswith(b'\xff\xd9')
assert nro.count(icon) == 1, 'custom icon is not embedded exactly once'
assert nro.count(nacp) == 1, 'generated NACP is not embedded exactly once'
assert nacp[:0x200].split(b'\0', 1)[0] == b'NXInputRelay'
assert nacp[0x200:0x300].split(b'\0', 1)[0] == b'NXInputRelay contributors'
assert nacp[0x3060:0x3070].split(b'\0', 1)[0] == b'1.4.3'
print('PASS: custom JPEG and NXInputRelay NACP are embedded in the NRO.')
