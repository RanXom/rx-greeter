# Changelog

## 1.0.2 — 2026-11-22

### Fixes

- Failed authentication now sends `cancel_session` before closing the greetd
  connection, so a retry no longer fails with "a session is already being
  configured".
- Raw PAM prompts and authentication errors are no longer echoed by the
  greeter. The red border and shake are the failure feedback, and the user and
  session previews stay visible. Visible challenges such as verification codes
  are still shown.
- The user and session selector rows now animate closed when focus returns to
  the password field, matching the opening animation.
- Added integration coverage for silent auth failures, session cancellation
  and retry, and the selector collapse animation. The mock greetd now rejects
  a new session if the previous failed one was not cancelled.
- Refreshed the README screenshots for the minimal layout and silent auth
  error.

## 1.0.1 — 2026-11-22

### Fixes

- Fixed an infinite text-elision loop that froze the greeter when a session
  name was wider than the selector (for example,
  `Niri (HDMI / projector)`); the hang pegged the greeter at 100% CPU.
- Removed the unconditional frame request on shared-memory buffer release, so
  the greeter idles at 0% CPU instead of redrawing continuously.
- Disabled clock and date by default in the example theme and Matugen
  template for the minimal layout.
- Added a long-session-name regression case to the integration suite.

## 1.0.0 — 2026-11-22

First release.

### Features

- Wayland greeter for greetd using wlr-layer-shell, wl_shm, Cairo, xkbcommon,
  and json-c. No Qt, GTK, Electron, or browser runtime.
- One fullscreen layer surface per output, with configurable primary output
  and on-demand keyboard focus on secondary displays.
- Native-resolution fractional scaling through wp_fractional_scale_v1 and
  wp_viewporter, with integer-scale fallback.
- VoidSDDM-style layout and interactions: centered password field, password
  dots, fade-in, error border/shake, clock/date, user selector, session
  selector, power selector, selector previews, help tips, and Caps Lock
  indicator.
- Keyboard navigation with arrows, Enter, Escape, Tab/Shift+Tab; mouse clicks
  on fields, previews, selector arrows, and power actions.
- F10 suspend, F11 poweroff, F12 reboot through a fixed-action helper.
- greetd IPC authentication with `create_session`, secret/visible/info/error
  auth messages, `start_session`, `cancel_session`, auth-error recovery and
  timeout handling.
- Local human user discovery and Wayland session discovery, with local
  `wayland-sessions` entries taking precedence.
- Matugen theme generation using the same color keys as waylock, plus
  greeter-specific geometry, font, clock, selector, and power keys.
- Current blurred wallpaper support (`swaylock-bg.png`), with configurable
  path and greetd-readable asset staging.
- Split host configuration (`config.conf`) and visual theme (`theme.conf`), so
  Matugen regeneration cannot overwrite machine policy.
- Generic defaults: first human user, first available session, automatic
  background discovery, and sibling power-helper discovery.
- Minimal nested Sway session wrapper with private runtime directory,
  dbus-run-session, AMD/non-NVIDIA DRM selection, optional dGPU-off hook, and
  off-VT logging.
- Setuid-root or doas-based power helper with dry-run mode.
- Syslog diagnostics, optional JSON debug state files, and no password
  logging.
- Unit tests, mock greetd server, virtual keyboard/pointer test tool, and
  portable headless Sway integration tests with grim screenshots.

### Fixes and hardening

- Explicitly configured unreadable config/theme paths are reported instead of
  silently falling back to built-in defaults.
- Configuration precedence is CLI > environment > host config > theme >
  built-in defaults.
- Layout is clamped on small outputs, and long clock/status/auth text is
  elided.
- Dead-key composition and locale-aware date formatting.
- Password length overflow reports an error instead of silently dropping
  input.
- Debug status files are written with mode 0600.
- Older wl_seat versions are handled without sending unsupported release
  requests.
- Power actions try loginctl with a timeout before direct fallback.
- Cancel waits briefly for greetd's acknowledgement without blocking the
  login screen.
- Malformed desktop entries and missing TryExec programs are skipped with
  diagnostics.
- Generic, portable integration tests use temporary sessions, wallpaper, and
  configuration rather than the host user's files.
