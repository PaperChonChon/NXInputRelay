"""Small authenticated client for NXInputRelay's TCP input service."""

from __future__ import annotations

import argparse
import math
import re
import secrets
import socket
import time
from pathlib import Path


DEFAULT_TOKEN_FILE = Path(__file__).resolve().with_name("network-token.txt")
LEGACY_TOKEN_FILE = Path(__file__).resolve().parents[1] / "network-token.txt"
SWITCH_TOKEN_PATH = "/switch/RecurCIV/network-token.txt"

KEY_MAP = """NXInputRelay descriptive keyboard map (case-insensitive)

Chord syntax
  key ctrl+a                 tap Ctrl+A
  key ctrl+shift+escape      tap Ctrl+Shift+Escape
  key alt+f4                tap Alt+F4
  keyDown shift+up          hold Shift+Up; release with keyUp
  key ctrl                  tap a modifier by itself

Modifiers (generic names select the left-hand key)
  ctrl/control, shift, alt/option, gui/win/windows/meta/cmd/command
  lctrl/leftctrl, lshift/leftshift, lalt/leftalt, lgui/leftgui/leftwin
  rctrl/rightctrl, rshift/rightshift, ralt/rightalt/altgr,
  rgui/rightgui/rightwin

Letters and number row
  a through z               HID 4-29
  digit1 through digit9     HID 30-38
  digit0                    HID 39

Editing and punctuation
  enter/return  escape/esc  backspace/bksp  tab  space/spacebar
  minus/hyphen  equal/equals  leftbracket/lbracket
  rightbracket/rbracket  backslash  semicolon  apostrophe/quote
  grave/backtick  comma  period/dot  slash  capslock/caps

Function and navigation
  f1 through f24
  printscreen/prtsc  scrolllock  pause  insert/ins  home
  pageup/pgup  delete/del  end  pagedown/pgdn
  right/rightarrow  left/leftarrow  down/downarrow  up/uparrow

Keypad and application
  numlock  kpslash/kpdivide  kpmultiply  kpminus  kpplus  kpenter
  kp0 through kp9  kpdecimal/kpdot  kpequal
  application/menu  power

Editing/media usages
  execute  help  select  stop  again  undo  cut  copy  paste  find
  mute  volumeup  volumedown

Raw compatibility
  key 82                    numeric HID usages 4-231 remain accepted
  keyMod a 1               legacy modifier index 0-7 remains accepted
  keyDown a 2              legacy modifier-byte form remains accepted

Controller commands are separate: for example, click DUP and click HOME.
"""

INPUT_MAP = """NXInputRelay network input map

Touchscreen coordinates
  Coordinates use the native 1280x720 landscape space:
  X = 0..1279 from left to right; Y = 0..719 from top to bottom.

  tap X Y                    complete an 80 ms down/up tap
  tap X Y MILLISECONDS       complete a 1..5000 ms down/up tap
  touchDown X Y              start and hold one finger
  tapDown X Y                friendly alias for touchDown
  touchMove X Y              move the held finger
  touchUp                    release the held finger

  Example center-screen tap: tap 640 360
  Example drag:
    touchDown 100 300
    touchMove 900 300
    touchUp

Target behavior
  sw1  Native hid:dbg touchscreen injection; expected to be reliable.
  sw2  Experimental absolute USB HID digitizer on interface 2 / endpoint 0x83.
       Commands return ERR touch-unavailable if Switch 2 does not enumerate it.
       Keyboard and controller remain available when experimental touch is absent.

Safety
  Only one contact is supported. Session stop, timeout, disconnect, neutral, and
  recovery release the contact. A held contact must be released before `tap`.

Other input families
  Keyboard: run --list-keys (or `help keys` interactively).
  Controller buttons: click/press/release followed by
    Y B A X L R ZL ZR MINUS PLUS L3 R3 HOME CAPTURE DUP DDOWN DLEFT DRIGHT
  Sticks: setStick LEFT|RIGHT -32768..32767 -32768..32767
"""


def print_key_map() -> None:
    print(KEY_MAP.rstrip())


def print_input_map() -> None:
    print(INPUT_MAP.rstrip())


def validate_token(token: str, source: str) -> str:
    if not 8 <= len(token) <= 64:
        raise ValueError(f"{source}: token must be 8-64 characters")
    if any(ord(character) < 33 or ord(character) > 126 for character in token):
        raise ValueError(f"{source}: token must contain printable ASCII without whitespace")
    return token


def read_token_file(path: Path) -> str:
    try:
        lines = path.read_text(encoding="ascii").splitlines()
    except (OSError, UnicodeError) as error:
        raise ValueError(f"cannot read token file {path}: {error}") from error
    if not lines:
        raise ValueError(f"{path}: token file is empty")
    return validate_token(lines[0], str(path))


def generate_token_file(path: Path) -> str:
    token = secrets.token_urlsafe(32)
    validate_token(token, "generated token")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="ascii", newline="\n") as token_file:
            token_file.write(token + "\n")
    except FileExistsError as error:
        raise ValueError(f"refusing to overwrite existing token file: {path}") from error
    except OSError as error:
        raise ValueError(f"cannot create token file {path}: {error}") from error
    return token


def print_token_install_instructions(path: Path) -> None:
    print(f"Created a unique NXInputRelay token file:\n  {path}")
    print("\nCopy this file to the following location on the Switch SD card:")
    print(f"  {SWITCH_TOKEN_PATH}")
    print("\nKeep the local file beside nxinputrelay_net.py; the client will use it automatically.")
    print("After copying it, fully restart the NXInputRelay NRO or sysmodule, then run this command again.")


def resolve_token(token: str | None, token_file: Path | None) -> tuple[str, bool]:
    if token is not None:
        return validate_token(token, "--token"), False

    if token_file is not None:
        path = token_file.expanduser().resolve()
    elif DEFAULT_TOKEN_FILE.is_file():
        path = DEFAULT_TOKEN_FILE
    elif LEGACY_TOKEN_FILE.is_file():
        # Preserve compatibility with clients that stored the token one level
        # above the tools directory before NXInputRelay added onboarding.
        path = LEGACY_TOKEN_FILE
    else:
        path = DEFAULT_TOKEN_FILE

    if path.is_file():
        return read_token_file(path), False

    print(f"NXInputRelay token file was not found:\n  {path}")
    try:
        answer = input("Generate a new unique token file now? [y/N]: ").strip().lower()
    except EOFError:
        answer = ""
    if answer not in {"y", "yes"}:
        raise ValueError(
            "token generation declined; provide --token, --token-file, or run interactively to generate one"
        )

    generated = generate_token_file(path)
    print_token_install_instructions(path)
    return generated, True


def receive_line(connection: socket.socket) -> str:
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = connection.recv(1)
        if not chunk:
            raise ConnectionError("NXInputRelay closed the connection")
        data.extend(chunk)
        if len(data) > 4096:
            raise ConnectionError("response line is too long")
    return data.decode("utf-8", "replace").rstrip("\r\n")


def send_command(connection: socket.socket, command: str) -> str:
    connection.sendall(command.encode("ascii") + b"\n")
    return receive_line(connection)


def normalize_command(command: str) -> str:
    if command.lower().startswith("type "):
        return "typeHex " + command[5:].encode("utf-8").hex()
    return command


def load_macro(path: Path) -> list[tuple[int, str, str | float]]:
    steps: list[tuple[int, str, str | float]] = []
    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        name, separator, argument = line.partition(" ")
        if name.lower() == "delay":
            if not separator:
                raise ValueError(f"{path}:{line_number}: delay requires seconds")
            try:
                seconds = float(argument.strip())
            except ValueError as error:
                raise ValueError(f"{path}:{line_number}: invalid delay {argument!r}") from error
            if not math.isfinite(seconds) or seconds < 0:
                raise ValueError(f"{path}:{line_number}: delay must be a non-negative number")
            steps.append((line_number, "delay", seconds))
            continue
        if name.lower() in {"auth", "quit", "disconnect"}:
            raise ValueError(f"{path}:{line_number}: {name} is managed by the macro runner")
        steps.append((line_number, "command", normalize_command(line)))
    if not steps:
        raise ValueError(f"{path}: macro contains no commands")
    return steps


def wait_delay(connection: socket.socket, seconds: float) -> None:
    deadline = time.monotonic() + seconds
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 30.0))
        if remaining > 30.0:
            response = send_command(connection, "ping")
            if response != "OK pong":
                raise RuntimeError(f"keepalive failed: {response}")


def wait_until_idle(connection: socket.socket, timeout: float = 120.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        response = send_command(connection, "status")
        match = re.search(r"keyq=(\d+) padq=(\d+)(?: touchq=(\d+))?", response)
        if not match:
            raise RuntimeError(f"unexpected status response: {response}")
        if match.group(1) == "0" and match.group(2) == "0" and (match.group(3) in {None, "0"}):
            # Queue counts reach zero when the final report is submitted. Allow
            # that USB transfer and its minimum hold interval to complete.
            time.sleep(0.2)
            return
        time.sleep(0.05)
    raise TimeoutError("NXInputRelay input queues did not drain within 120 seconds")


def run_macro(connection: socket.socket, steps: list[tuple[int, str, str | float]]) -> None:
    for line_number, kind, value in steps:
        if kind == "delay":
            seconds = float(value)
            print(f"[{line_number}] delay {seconds:g}s")
            wait_delay(connection, seconds)
            continue
        command = str(value)
        response = send_command(connection, command)
        print(f"[{line_number}] {command} -> {response}")
        if response.startswith("ERR"):
            raise RuntimeError(f"macro stopped at line {line_number}: {response}")
        wait_until_idle(connection)


def main() -> int:
    parser = argparse.ArgumentParser(description="Send input through NXInputRelay")
    parser.add_argument("host", nargs="?", help="Switch 1 IP address")
    parser.add_argument("command", nargs="*", help="command, or 'type TEXT'")
    parser.add_argument("--macro", type=Path, help="run commands and delays from a UTF-8 text file")
    parser.add_argument(
        "--port",
        type=int,
        default=None,
        help="TCP port override (default: 6001 for sw1, 6002 for sw2)",
    )
    parser.add_argument(
        "--target",
        choices=("sw1", "sw2"),
        default="sw2",
        help="consume input locally on Switch 1 or forward it to Switch 2 (default: sw2)",
    )
    parser.add_argument("--token")
    parser.add_argument("--list-keys", action="store_true",
                        help="show the complete descriptive keyboard map without connecting")
    parser.add_argument("--list-inputs", action="store_true",
                        help="show touch, controller, target, and safety commands without connecting")
    parser.add_argument(
        "--token-file",
        type=Path,
        default=None,
        help="token file override (default: network-token.txt beside this script)",
    )
    parser.add_argument("--wait", type=float, default=0.5,
                        help="seconds to keep a one-shot session open after queuing")
    args = parser.parse_args()

    if args.list_keys:
        print_key_map()
        return 0
    if args.list_inputs:
        print_input_map()
        return 0
    try:
        token, generated = resolve_token(args.token, args.token_file)
    except ValueError as error:
        parser.error(str(error))
    if generated:
        return 0

    if not args.host:
        parser.error("host is required unless a token was generated or a list option is used")

    if args.macro and args.command:
        parser.error("a positional command cannot be combined with --macro")
    try:
        macro_steps = load_macro(args.macro) if args.macro else None
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))

    port = args.port if args.port is not None else (6001 if args.target == "sw1" else 6002)
    print(f"Connecting to {args.host}:{port} (target={args.target})...")
    with socket.create_connection((args.host, port), timeout=5) as connection:
        connection.settimeout(5)
        print(receive_line(connection))
        # Keep the legacy USB authentication line compatible with the existing
        # port-6002 sysmodule. Only the new local target needs an extension.
        auth_command = f"auth {token}" if args.target == "sw2" else f"auth {token} switch1"
        authenticated = send_command(connection, auth_command)
        print(authenticated)
        if not authenticated.startswith("OK "):
            return 2

        if macro_steps is not None:
            run_macro(connection, macro_steps)
            time.sleep(max(0.0, args.wait))
            return 0

        if args.command:
            command = normalize_command(" ".join(args.command))
            print(send_command(connection, command))
            time.sleep(max(0.0, args.wait))
            return 0

        print("Enter commands; Ctrl+C or 'quit' exits.")
        try:
            while True:
                command = input("nxinputrelay> ").strip()
                if not command:
                    continue
                if command.lower() in {"help", "help keys", "keys"}:
                    print_key_map()
                    continue
                if command.lower() in {"help inputs", "help touch", "touches", "inputs"}:
                    print_input_map()
                    continue
                command = normalize_command(command)
                print(send_command(connection, command))
                if command.lower() in {"quit", "disconnect"}:
                    break
        except (EOFError, KeyboardInterrupt):
            print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
