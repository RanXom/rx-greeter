# rx-greeter

A small, fullscreen Wayland greeter for [greetd](https://git.sr.ht/~kennylevinsen/greetd)
that ports the look and interactions of
[VoidSDDM](https://github.com/talyamm/VoidSDDM) and adds:

- **Matugen colors**, generated with the same palette keys as the local
  `waylock` theme.
- **The current blurred Sway wallpaper**, read from
  `~/.cache/swaylock-bg.png` (the file `sway-wallpaper` already maintains).

It is written in C and uses Wayland, Cairo, xkbcommon, and json-c. There is no
Qt, GTK, Electron, or browser runtime. The greeter does not implement PAM;
greetd owns authentication.

The live `/etc/greetd/config.toml` was **not** modified by this project. See
[greetd configuration](#greetd-configuration) before switching the live
greeter.

## Features

- Fullscreen xdg-shell window with a cover-scaled, pre-blurred wallpaper and a
  configurable dark overlay.
- Centered password field with dots, waylock-compatible geometry, red error
  border and shake, and password clearing on failure.
- Optional clock and date.
- User selector (`/etc/passwd`, uid >= 1000, valid shell).
- Session selector (`wayland-sessions` desktop files, local entries take
  precedence), including the local `sway-session` wrapper.
- Power selector plus F10 suspend, F11 poweroff, F12 reboot.
- Caps Lock indicator, fade-in, selector expand animations, help tips.
- Keyboard and mouse navigation.
- Matugen template and example theme.
- Test-only virtual keyboard/pointer injector and a mock greetd server.

## Requirements

Runtime/build libraries (discovered with `pkg-config`):

- `wayland-client`, `wayland-protocols`, `wayland-scanner`
- `cairo`
- `xkbcommon`
- `json-c`
- `fontconfig`
- a readable font; the example uses **IosevkaTerm Nerd Font Mono**. The
  configured fallback is Iosevka, then fontconfig's `sans` if needed.

Compositor: Sway (a wlroots compositor with `xdg-shell`). Sway is used for the
nested greeter session. The host's Sway/SwayFX is not linked into the binary.

Test-only: `sway`, `grim`, Python 3. `make integration` needs wlroots virtual
keyboard and pointer protocol support (present in wlroots 0.20/Sway 1.12).

## Build

```sh
devenv shell -- make
```

or, with the dependencies installed system-wide:

```sh
make
```

Outputs:

- `build/rx-greeter` — the greeter
- `build/rx-greeter-power` — fixed-action privileged helper
- `build/unit-tests` — unit tests
- `build/wl-input` — test-only input injector

The binary intentionally records ordinary shared-library sonames, not Nix
store paths. `ldd build/rx-greeter` after a devenv build should resolve against
the host libraries.

## Test

```sh
devenv shell -- make check        # parser/password/session/greetd unit tests
devenv shell -- make integration  # nested headless Sway + mock greetd
```

`make integration` starts a private headless Sway
(`WLR_BACKENDS=headless`, `WLR_RENDERER=pixman`,
`WLR_LIBINPUT_NO_DEVICES=1`), a mock greetd socket, and the virtual input tool.
It verifies:

- wallpaper/theme loading and the local Sway session entry
- typing, Backspace, Escape, Tab and Shift+Tab
- password dots, Caps Lock on/off
- user, session, and power selectors; mouse clicks on arrows and outside
- F10/F11/F12 power dry-runs
- authentication error feedback and password clearing
- the visible auth-message path
- `create_session` → secret response → `success` → `start_session`, including
  the selected session argv and environment

Screenshots are written under `build/test-artifacts/<timestamp>/` with `grim`.

The test binary and mock are not part of the installed greeter.

## Install

```sh
sudo make install
```

By default this installs:

| Path | Purpose |
| --- | --- |
| `/usr/local/bin/rx-greeter` | greeter |
| `/usr/local/bin/rx-greeter-session` | greetd session wrapper |
| `/usr/local/libexec/rx-greeter-power` | fixed-action power helper |
| `/usr/local/libexec/rx-greeter-sync-assets` | optional greetd-readable asset sync |
| `/usr/local/share/rx-greeter/theme.conf` | example theme |
| `/usr/local/share/rx-greeter/greetd-config.toml` | example greetd stanza |
| `/usr/local/share/rx-greeter/matugen/rx-greeter.conf` | Matugen template |

`make install-system-config` installs the example theme to
`/etc/rx-greeter/theme.conf` only if that file does not already exist.

### Power helper policy

The greeter runs as the `greetd` user and cannot power off/reboot/suspend
directly. The helper accepts exactly `suspend`, `poweroff`, or `reboot`; no UI
string is ever passed to a shell.

Option A — setuid root, executable only by root/greetd:

```sh
sudo make install-power-setuid
```

This installs mode `4750`, owner `root:greetd`.

Option B — doas. Install normally, then add a doas rule for the greetd user:

```
permit nopass greetd cmd /usr/local/libexec/rx-greeter-power
```

When the helper is not root it re-executes itself through
`/usr/bin/doas -n -u root`. Set `RX_GREETER_POWER_NO_DOAS=1` to disable that
behavior.

Both the selector and F10/F11/F12 call the same helper. `--power-dry-run` (or
`RX_GREETER_POWER_DRY_RUN=1`) only prints the action and is what the integration
test uses.

## Theme and Matugen

The user setup script registers the template and creates an initial theme:

```sh
./scripts/install-user-theme.sh
# or, to immediately regenerate the theme (and all other Matugen outputs):
./scripts/install-user-theme.sh --generate
```

It backs up `~/.config/matugen/config.toml`, installs
`~/.config/matugen/templates/rx-greeter.conf`, and adds:

```toml
[templates.rx-greeter]
input_path = "/home/ranxom/.config/matugen/templates/rx-greeter.conf"
output_path = "/home/ranxom/.config/rx-greeter/theme.conf"
```

`sway-wallpaper` already calls Matugen, so future wallpaper changes regenerate
`~/.config/rx-greeter/theme.conf` automatically.

Theme search order:

1. `--theme PATH`
2. `$RX_GREETER_THEME`
3. `$XDG_CONFIG_HOME/rx-greeter/theme.conf`
4. `$HOME/.config/rx-greeter/theme.conf`
5. `/etc/rx-greeter/theme.conf`
6. `/var/lib/rx-greeter/theme.conf`
7. `/usr/local/etc/rx-greeter/theme.conf`

Built-in defaults use the live waylock palette: background `#121415`, surface
`#1e2021`, text `#e2e2e3`, primary `#a2cdde`, error `#ffb4ab`, outline
`#8b9295`. The Matugen template uses the same keys as waylock:

```ini
background_color = {{ colors.surface.default.hex }}
surface_color = {{ colors.surface_container.default.hex }}
text_color = {{ colors.on_surface.default.hex }}
primary_color = {{ colors.primary.default.hex }}
error_color = {{ colors.error.default.hex }}
outline_color = {{ colors.outline.default.hex }}
```

Greeter-specific keys include `background_image`, `background_overlay`,
`box_width`, `box_height`, `border_radius`, `box_x`, `box_y`, `dot_spacing`,
`dot_radius`, `font_family`, `fallback_font_family`, `font_size`, `fade_ms`,
`animation_ms`, `element_opacity`, `element_spacing`, `show_cursor`,
`show_clock`, `show_date`, `clock_format`, `date_format`, `clock_*`,
`show_user_selector`, `show_session_selector`, `show_power_buttons`,
`selector_*`, `default_user`, `default_session`, `allow_empty_password`,
`clear_password_on_error`, `login_error_*`, `error_hold_ms`,
`auth_timeout_ms`, `power_helper`, and `power_dry_run`. See
[`configs/theme.conf`](configs/theme.conf) for the complete example.

Clock/date formats are `strftime(3)` formats (`%H:%M`, `%A, %B %d`), not the
VoidSDDM JavaScript placeholders.

## Wallpaper synchronization

The default background is the same pre-blurred file used by waylock:

```
/home/ranxom/.cache/swaylock-bg.png
```

`~/.local/bin/sway-wallpaper` generates it with ffmpeg and regenerates Matugen
outputs. rx-greeter never takes a screenshot and never re-blurs at login.

The theme path and background path are both configurable. `--background PATH`
or `RX_GREETER_BACKGROUND` overrides the theme's `background_image`. Only PNG
is supported by Cairo's image loader.

The user's home and cache directories are traversable/readable by greetd in the
current setup (`namei -l` shows 755/644), so the greeter reads the live files
directly and no copy is needed. If a future setup makes the home private, sync
readable copies:

```sh
sudo /usr/local/libexec/rx-greeter-sync-assets
```

That writes `/var/lib/rx-greeter/theme.conf` and
`/var/lib/rx-greeter/background.png` and rewrites the copied theme to point at
the copy. Re-run it after wallpaper/Matugen changes. The session script fails
over to `/var/lib/rx-greeter/theme.conf` automatically.

## Session and compositor

`rx-greeter-session` is modeled on `noctalia-greeter-session`:

- creates a private `XDG_RUNTIME_DIR`
- unsets stale `WAYLAND_DISPLAY`, `SWAYSOCK`, and `I3SOCK`
- runs under `dbus-run-session`
- pins wlroots to the AMD card/render node when present
- starts a minimal Sway with no user config, bars, idle daemons, or wallpaper
- launches rx-greeter as the only fullscreen window
- exits Sway when the greeter exits, so greetd can start the chosen session
- parks stdout/stderr off the VT by default

Useful environment variables:

| Variable | Meaning |
| --- | --- |
| `RX_GREETER_BIN` | greeter binary |
| `RX_GREETER_THEME` | theme path |
| `RX_GREETER_LOG` | `stderr`, `none`, or a writable file path |
| `RX_GREETER_DRM_CARD` / `RX_GREETER_DRM_RENDER` | override AMD device paths |
| `RX_GREETER_DGPU_OFF` | optional executable to power off the dGPU |
| `RX_GREETER_XKB_LAYOUT` / `RX_GREETER_XKB_OPTIONS` | greeter keyboard layout/options |
| `RX_GREETER_OUTPUT_SCALE` | Sway output scale, e.g. `1.4` |
| `RX_GREETER_COMPOSITOR` | compositor binary (default `sway`) |
| `WLR_LOG` | wlroots log level |

The minimal Sway config does not set an output scale. On a HiDPI display,
export `RX_GREETER_OUTPUT_SCALE` to match the user session (the user's sway
config uses `1.4`); otherwise geometry and fonts will be physically smaller.

The NVIDIA dGPU is kept out of wlroots by `WLR_DRM_DEVICES`. It is already
removed on this machine. If you want the greeter to actively power it off, add
a fixed hook at `/usr/local/libexec/rx-greeter-dgpu-off` or set
`RX_GREETER_DGPU_OFF`; the script runs it best-effort. Do not point it at a
script that prompts or blocks for 15 seconds.

## greetd configuration

Do not edit the live file until the greeter has passed testing. Back it up
first:

```sh
sudo cp -a /etc/greetd/config.toml /etc/greetd/config.toml.bak
```

Example stanza (also at
[`configs/greetd-config.toml`](configs/greetd-config.toml)):

```toml
[terminal]
vt = 7

[default_session]
command = "/usr/local/bin/rx-greeter-session"
user = "greetd"
```

The greeter connects to `$GREETD_SOCK`, sends `create_session`, answers
`auth_message` prompts, sends `start_session`, and exits so greetd can launch
the session. `auth_error` keeps the greeter running with the password cleared.
Escape during authentication sends `cancel_session`.

## Keys

| Key | Action |
| --- | --- |
| Up / Down | move between password, user, session, power |
| Tab / Shift+Tab | cycle focus, skipping hidden selectors |
| Left / Right | change the active selector |
| Enter | submit password, confirm power item, or return from user/session selector |
| Escape | clear password, leave a selector, or cancel authentication |
| Ctrl+U | clear password |
| F10 | suspend |
| F11 | poweroff |
| F12 | reboot |

Mouse clicks work on the field, selector arrows/labels, previews, and power
labels. Clicking outside an open selector returns focus to the password field.
Set `show_cursor = false` to hide the compositor cursor.

## Security notes

- Authentication is entirely greetd/PAM. rx-greeter never reads `/etc/shadow`.
- Passwords are not logged. The test debug status file contains only
  `password_len`, never password text.
- The password buffer and serialized greetd request buffer are zeroed after
  use; the mock test logs only response lengths.
- The power helper accepts a fixed action enum and never invokes a shell.
- The debug status file is opt-in (`--debug-status PATH`) and intended for
  testing, not the installed session.

## Known limitations

- One xdg-shell fullscreen window; multi-monitor layouts are not handled.
- PNG backgrounds only.
- Fractional Sway scaling is handled by the compositor, so a fractional scale
  may render slightly soft. Set an integer output scale for crisp output.
- The optional VoidSDDM show-password eye button is not ported.
- No avatar/image selector previews; user and session previews are text.
- No remembered last user/session.
- The session list only reads the two system `wayland-sessions` directories;
  it deliberately does not use `~/.local/share/wayland-sessions` because the
  greeter runs as greetd.
- Real greetd on a VT, suspend, reboot, and poweroff have not been exercised;
  only nested/mock tests and the helper's dry-run mode were. Test in a VM or
  spare TTY before switching the live config.
- The Sway session wrapper and theme paths are tailored to `ranxom`; adjust
  `default_user`, `default_session`, the Matugen paths, and the theme search in
  `rx-greeter-session` for another user.
