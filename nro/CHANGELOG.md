# Changelog

All notable NXInputRelay releases will be documented here.

## [1.4.4] — 2026-11-24

### Fixed

- Listener creation and recovery no longer wait for `gethostid()` to stop
  reporting loopback. Because the listener binds `INADDR_ANY`, it now starts at
  boot and retries even during a stale NIFM transition.
- Persistent routing errors returned by `accept()` now recycle the socket
  service after a bounded retry delay instead of generating a tight log loop.

## [1.4.3] — 2026-11-23

### Fixed

- Network and USB diagnostics now live under `/switch/NXInputRelay` instead of
  the legacy RecurCIV directory.
- Transient `accept()` routing errors no longer destroy and recreate a healthy
  TCP listener.
- Brief loopback results from `gethostid()` are now diagnostic observations;
  the listener remains bound to `INADDR_ANY` and available when the real address
  returns.
- The access-point keepalive prefers the configured DNS host over an isolated
  or intentionally unreachable gateway.

## [1.4.2] — 2026-11-23

### Added

- Interactive, non-overwriting token generation in `nxinputrelay_net.py`. The
  generated file is stored beside the client and its value is never printed.

### Changed

- The NRO and sysmodule now prefer
  `/switch/NXInputRelay/network-token.txt`.
- `/switch/RecurCIV/network-token.txt` remains a fallback for existing
  installations.

## [1.4.1] — 2026-11-23

### Fixed

- A failed best-effort UDP gateway keepalive no longer tears down healthy TCP
  clients or rebuilds the listener. This supports intentionally isolated LANs
  whose configured gateway is unreachable.
- After three failures, keepalive attempts back off to once per minute while
  the listener and authenticated session remain intact.

## [1.4.0] — 2026-11-23

### Added

- Absolute single-contact touch commands: `tap`, `touchDown`/`tapDown`,
  `touchMove`, and `touchUp`, using the Switch 1280x720 coordinate space.
- Native Switch 1 touch injection through `hid:dbg`.
- An experimental Switch 2 USB HID digitizer interface on interface 2 / EP83.
  Keyboard and gamepad readiness remains independent of touch enumeration.
- A complete offline controller, keyboard, and touch reference through
  `--list-inputs` and interactive `help inputs` / `help touch`.

### Safety

- Disconnect, timeout, neutral, and cleanup paths release any held touch.
- Touch coordinates and hold durations are range checked; only one contact is
  supported.

## [1.3.2] — 2026-11-23

### Added

- Descriptive modifier chords such as `key ctrl+a`, `key alt+f4`, and
  `keyDown shift+up`, including explicit left/right modifier names.
- A complete offline Python key reference through `--list-keys` and interactive
  `help keys`, covering aliases, chords, keypad keys, and numeric compatibility.
- Additional descriptive names for punctuation, keypad, F13-F24, editing, and
  media usages.

## [1.3.1] — 2026-11-23

### Added

- Case-insensitive descriptive keyboard names for network `key`, `keyMod`, and
  `keyDown` commands while retaining numeric USB HID usages.
- Aliases for arrows, navigation/editing keys, letters, digits, and F1-F12.

## [1.3.0] — 2026-11-23

### Added

- A fourth top-level **Network Control** landing page with listener, link,
  keepalive, recovery, client, target, queue and last-command status.
- Automatic redirection to Network Control when a remote session authenticates.
- Persistent last-target and last-command reporting after one-shot sessions.
- Manual listener restart from the idle Network Control page.

### Safety

- Active remote sessions continue to own input exclusively. The NRO locks its
  ordinary keyboard/controller pages until neutral cleanup completes; `+`
  requests a safe session stop from the landing page.

## [1.2.2] — 2026-11-23

### Changed

- NRO network diagnostics now use `network-nro.log` and
  `network-nro.log.1`, preventing interleaved entries with the sysmodule.

## [1.2.1] — 2026-11-23

### Fixed

- Treat missing and loopback-only IPv4 state as link loss instead of accepting
  a locally valid but externally unreachable listener socket.
- Recreate the complete BSD socket service and listener after an IPv4 address
  loss, restoration, or change.
- Send a small LAN-only keepalive to the configured gateway every 15 seconds
  to reduce Windows Mobile Hotspot idle-client disconnections.
- Recover after three consecutive LAN keepalive send failures.

## [1.2.0] — 2026-11-22

### Added

- Authenticated `switch1` network targeting through a temporary local
  `hid:dbg` virtual controller and keyboard.
- Target-aware status and network recovery diagnostics.

### Preserved

- Existing `switch2` USB forwarding and legacy authentication compatibility.

## [1.1.0] — 2026-11-05

### Added

- Live controller macro recording alongside the existing local-only recorder.
- Persistent controller passthrough before, during and after recording, with
  neutral synchronization and release-to-arm protection at session entry.
- Physical Capture-button short-press Start/Stop controls using supported libnx
  applet notifications, plus touchscreen Start/Stop and explicit session Exit.
- Visible touchscreen fallback when Capture registration is unavailable, safe
  notification queuing/debounce, and restoration of normal Capture handling on exit.
- Recording Review/Edit/Save/Load/Replay after live-session neutral cleanup, with
  explicit confirmation before replacing unsaved captured data.
- Reproducible, notice-inclusive install/source packaging and source/NRO manifests.

### Fixed

- Removed accumulating USB completion-observation latency from playback holds
  in tested fixed-latency scenarios. Each full scaled hold begins at successful
  state submission; completion still gates ordered transitions without skipped
  states, competing transfers or compressed catch-up holds.
- Improved preservation of captured data through disconnects, recording limits
  and cleanup failures, including neutral-only cleanup retries and no automatic resume.
- Expanded deterministic timing, Capture, live-session, USB and UI regression tests.

### Compatibility and known limitations

- Local recording, RCMS files/storage, playback speed scaling and finite repeats
  remain supported. USB descriptors/device identity and the approved Civ VII
  payload are unchanged; keyboard and Civ VII functionality are preserved.
- Recorded physical input may not exactly match what the host received during
  latest-state live passthrough. Analog sampling and USB/application scheduling
  can introduce small movement differences; slow transports can stretch playback.
- Capture behavior depends on the homebrew launch environment. Short-press
  screenshots are reserved during live sessions; broader long-hold/restoration
  testing remains necessary. Touchscreen fallback, exit and recovery are supported.
- USB compatibility across Switch hardware, docks and cable setups is not fully
  verified. Native Switch 2 execution is untested.

See `docs/macro-studio.md` and `docs/macro-timing.md` for workflow and measurements.

## [1.0.0] — 2026-11-05

### Initial Release

- Rebranded the application as RecurCIV.
- Integrated USB keyboard input, text-file sending, and Live Keyboard functionality.
- Added USB controller functionality.
- Added Macro Studio for creating, recording, editing, and playing controller macros.
- Added Macro Library for managing saved macros.
- Added adjustable macro playback speed and repeat counts.
- Added Civilization VII exploit automation based on CivSwitch.
- Added USB diagnostics and SD-card-based settings and macro storage.
- Updated application metadata, icon, and installation paths.

### Licensing and release preparation

- Applied GPLv3 to the combined distribution while preserving original MIT
  and third-party terms, copyright notices, and credits.
- Included the exact editable CivSwitch payload source, generator, pinned
  dependencies, provenance, and byte-for-byte reproduction instructions.
- Added runtime licensing/source records and notice-inclusive release packaging.
- Added release verification tooling without changing existing functional
  source, USB implementation, UI, build.py, assets, tests, or approved payload.

### Known Limitations

- Macro recording captures inputs locally without forwarding them to the connected USB host.
- Macro recording and playback require additional hardware verification.
- USB compatibility depends on the receiving device and its supported interfaces.

### Planned

- Live controller input forwarding during macro recording.
- Direct Macro Studio access from controller mode.
- Additional hardware compatibility testing and usability improvements.
