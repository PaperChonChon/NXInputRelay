import subprocess
import sys
import tempfile
import importlib.util
from pathlib import Path


root = Path(__file__).resolve().parents[1]
client = root / "tools" / "nxinputrelay_net.py"
result = subprocess.run(
    [sys.executable, str(client), "--list-keys"],
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
    [sys.executable, str(client), "--list-inputs"],
    cwd=root, text=True, capture_output=True, check=True,
).stdout.lower()
for required in ("tap 640 360", "touchdown", "tapdown", "touchmove", "touchup", "1280x720", "touch-unavailable"):
    assert required in inputs, required
assert "connecting to" not in inputs

spec = importlib.util.spec_from_file_location("nxinputrelay_net", client)
module = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(module)
assert module.DEFAULT_TOKEN_FILE == client.with_name("network-token.txt")

with tempfile.TemporaryDirectory(prefix="nxinputrelay-token-") as directory:
    token_file = Path(directory) / "network-token.txt"
    generated = subprocess.run(
        [sys.executable, str(client), "--token-file", str(token_file)],
        cwd=root,
        input="yes\n",
        text=True,
        capture_output=True,
        check=True,
    )
    token = token_file.read_text(encoding="ascii").strip()
    assert 8 <= len(token) <= 64
    assert token.isascii() and token.isprintable() and not any(character.isspace() for character in token)
    assert "/switch/RecurCIV/network-token.txt" in generated.stdout
    assert token not in generated.stdout
    try:
        module.generate_token_file(token_file)
    except ValueError as error:
        assert "refusing to overwrite" in str(error)
    else:
        raise AssertionError("generator overwrote an existing token")

    declined_file = Path(directory) / "declined-token.txt"
    declined = subprocess.run(
        [sys.executable, str(client), "--token-file", str(declined_file)],
        cwd=root,
        input="n\n",
        text=True,
        capture_output=True,
    )
    assert declined.returncode == 2
    assert not declined_file.exists()

print("PASS: offline input maps and safe interactive token generation.")

