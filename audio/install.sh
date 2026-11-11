#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
UCM_HOME="${CONFIG_HOME}/alsa/ucm2"
CARD_CONF="HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A.conf"
HIFI_CONF="HiFi-hp-8e6a.conf"

restart_audio() {
    systemctl --user daemon-reload
    if command -v omarchy >/dev/null 2>&1; then
        omarchy restart audio
    else
        systemctl --user restart wireplumber.service pipewire.service pipewire-pulse.service
        sleep 2
        wpctl status
    fi
}

if [[ "${1:-}" == "--uninstall" ]]; then
    echo "==> Removing user-space MT8901 SoundWire UCM2 override..."
    rm -rf "${UCM_HOME}"
    rm -f "${CONFIG_HOME}/systemd/user/wireplumber.service.d/10-alsa-ucm2.conf"
    rm -f "${CONFIG_HOME}/systemd/user/pipewire.service.d/10-alsa-ucm2.conf"
    rm -f "${CONFIG_HOME}/environment.d/10-alsa-ucm2.conf"
    restart_audio
    exit 0
fi

echo "==> Installing user-space UCM2 tree into ${UCM_HOME}..."
mkdir -p "${UCM_HOME}/conf.d/mt8901-soundwir"

for entry in /usr/share/alsa/ucm2/*; do
    base="$(basename "${entry}")"
    [[ "${base}" == "conf.d" ]] && continue
    ln -sfn "${entry}" "${UCM_HOME}/${base}"
done

for entry in /usr/share/alsa/ucm2/conf.d/*; do
    base="$(basename "${entry}")"
    [[ "${base}" == "mt8901-soundwir" ]] && continue
    ln -sfn "${entry}" "${UCM_HOME}/conf.d/${base}"
done

ln -sfn /usr/share/alsa/ucm2/conf.d/mt8901-soundwir/mt8901-soundwir.conf \
    "${UCM_HOME}/conf.d/mt8901-soundwir/mt8901-soundwir.conf"

install -Dm644 "${SCRIPT_DIR}/ucm2/conf.d/mt8901-soundwir/${CARD_CONF}" \
    "${UCM_HOME}/conf.d/mt8901-soundwir/${CARD_CONF}"
install -Dm644 "${SCRIPT_DIR}/ucm2/conf.d/mt8901-soundwir/${HIFI_CONF}" \
    "${UCM_HOME}/conf.d/mt8901-soundwir/${HIFI_CONF}"

echo "==> Installing systemd user & environment.d ALSA_CONFIG_UCM2 drop-ins..."
install -Dm644 "${SCRIPT_DIR}/systemd-user/10-alsa-ucm2.conf" \
    "${CONFIG_HOME}/systemd/user/wireplumber.service.d/10-alsa-ucm2.conf"
install -Dm644 "${SCRIPT_DIR}/systemd-user/10-alsa-ucm2.conf" \
    "${CONFIG_HOME}/systemd/user/pipewire.service.d/10-alsa-ucm2.conf"
install -Dm644 "${SCRIPT_DIR}/environment.d/10-alsa-ucm2.conf" \
    "${CONFIG_HOME}/environment.d/10-alsa-ucm2.conf"

echo "==> Reloading systemd user manager and restarting audio stack..."
restart_audio
