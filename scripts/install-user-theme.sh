#!/bin/sh
# Install the rx-greeter Matugen template and initial theme for one user.
# Run as that user, not as root.
#
#   ./scripts/install-user-theme.sh
#   ./scripts/install-user-theme.sh --generate
#
# --generate calls Matugen for the current wallpaper. That rewrites every
# template registered in the user's Matugen config (including waylock, sway,
# foot, ...), using the same wallpaper and scheme as sway-wallpaper(1).

set -eu

generate=0
if [ "${1:-}" = "--generate" ]; then
    generate=1
elif [ "$#" -gt 0 ]; then
    echo "usage: $0 [--generate]" >&2
    exit 2
fi

_here="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
if [ -f "${_here}/../configs/matugen/rx-greeter.conf" ]; then
    _repo="$(CDPATH= cd -- "${_here}/.." && pwd)"
    _src_template="${_repo}/configs/matugen/rx-greeter.conf"
    _src_theme="${_repo}/configs/theme.conf"
else
    # Installed layout: $(PREFIX)/share/rx-greeter/{matugen,theme.conf}
    _repo="${_here}"
    _src_template="${_here}/matugen/rx-greeter.conf"
    _src_theme="${_here}/theme.conf"
fi
: "${HOME:?HOME must be set}"
_config_home="${XDG_CONFIG_HOME:-${HOME}/.config}"
_rx_dir="${_config_home}/rx-greeter"
_matugen_dir="${_config_home}/matugen"
_template_dir="${_matugen_dir}/templates"
_matugen_config="${_matugen_dir}/config.toml"
_rx_template="${_template_dir}/rx-greeter.conf"
_rx_theme="${_rx_dir}/theme.conf"

mkdir -p "${_template_dir}" "${_rx_dir}"
install -m644 "${_src_template}" "${_rx_template}"
# The upstream example is written for ranxom; make it work for this user.
sed -i "s|/home/ranxom|${HOME}|g" "${_rx_template}"

if [ ! -e "${_rx_theme}" ]; then
    install -m644 "${_src_theme}" "${_rx_theme}"
    # Make the example work for users other than ranxom.
    _default_bg="${HOME}/.cache/swaylock-bg.png"
    sed -i "s|^background_image = .*|background_image = ${_default_bg}|" \
        "${_rx_theme}"
    echo "created ${_rx_theme}"
else
    echo "keeping existing ${_rx_theme}"
fi

if [ -f "${_matugen_config}" ]; then
    _backup="${_matugen_config}.bak.$(date +%Y%m%d-%H%M%S)"
    cp -p "${_matugen_config}" "${_backup}"
    echo "backed up Matugen config to ${_backup}"
fi

if ! grep -q '^\[templates\.rx-greeter\]' "${_matugen_config}" 2>/dev/null; then
    {
        echo ""
        echo "[templates.rx-greeter]"
        printf 'input_path = "%s"\n' "${_rx_template}"
        printf 'output_path = "%s"\n' "${_rx_theme}"
    } >>"${_matugen_config}"
    echo "registered [templates.rx-greeter] in ${_matugen_config}"
else
    echo "[templates.rx-greeter] is already registered"
fi

if [ "${generate}" -eq 1 ]; then
    if ! command -v matugen >/dev/null 2>&1; then
        echo "matugen is not installed; skipping generation" >&2
        exit 1
    fi
    _wall="$(cat "${HOME}/.cache/current_wallpaper" 2>/dev/null || true)"
    if [ -z "${_wall}" ] || [ ! -f "${_wall}" ]; then
        echo "no current wallpaper; run sway-wallpaper --generate instead" >&2
        exit 1
    fi
    matugen image "${_wall}" --mode dark --type scheme-content \
        --source-color-index 0
fi

echo
echo "rx-greeter theme: ${_rx_theme}"
echo "Next: restart the greeter, or run 'make integration' from the repo first."
