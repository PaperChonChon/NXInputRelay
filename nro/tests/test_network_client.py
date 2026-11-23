import subprocess
import sys
from pathlib import Path


root = Path(__file__).resolve().parents[1]
result = subprocess.run(
    [sys.executable, str(root / "tools" / "recurciv_net.py"), "--list-keys"],
    cwd=root,
    text=True,
    capture_output=True,
    check=True,
)
reference = result.stdout.lower()
for required in (
    "ctrl+a", "alt+f4", "shift+up", "f1 through f24", "digit0",
    "pageup/pgup", "pagedown/pgdn", "numeric hid usages 4-231",
):
    assert required in reference, required
assert "connecting to" not in reference
inputs = subprocess.run(
    [sys.executable, str(root / "tools" / "recurciv_net.py"), "--list-inputs"],
    cwd=root, text=True, capture_output=True, check=True,
).stdout.lower()
for required in ("tap 640 360", "touchdown", "tapdown", "touchmove", "touchup", "1280x720", "touch-unavailable"):
    assert required in inputs, required
assert "connecting to" not in inputs
print("PASS: offline exhaustive key map requires no host, token, or network connection.")

