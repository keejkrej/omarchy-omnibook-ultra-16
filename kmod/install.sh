#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG_NAME="omnibook-ffa-fix"
PKG_VER="1.0.0"
SRC_DIR="/usr/src/${PKG_NAME}-${PKG_VER}"

echo "==> Building ${PKG_NAME} against $(uname -r)..."
make -C "${SCRIPT_DIR}"

echo "==> Loading omnibook_ffa_fix.ko into running kernel..."
if lsmod | grep -q "^omnibook_ffa_fix "; then
    sudo rmmod omnibook_ffa_fix
fi
sudo insmod "${SCRIPT_DIR}/omnibook_ffa_fix.ko"

if command -v dkms >/dev/null 2>&1; then
    echo "==> Installing ${PKG_NAME}-${PKG_VER} via DKMS..."
    sudo rm -rf "${SRC_DIR}"
    sudo mkdir -p "${SRC_DIR}"
    sudo cp "${SCRIPT_DIR}/omnibook_ffa_fix.c" "${SCRIPT_DIR}/Makefile" "${SCRIPT_DIR}/dkms.conf" "${SRC_DIR}/"
    sudo dkms remove -m "${PKG_NAME}" -v "${PKG_VER}" --all 2>/dev/null || true
    sudo dkms add -m "${PKG_NAME}" -v "${PKG_VER}"
    sudo dkms build -m "${PKG_NAME}" -v "${PKG_VER}"
    sudo dkms install -m "${PKG_NAME}" -v "${PKG_VER}" --force
else
    echo "==> Installing module into /lib/modules/$(uname -r)/extra/..."
    sudo mkdir -p "/lib/modules/$(uname -r)/extra"
    sudo cp "${SCRIPT_DIR}/omnibook_ffa_fix.ko" "/lib/modules/$(uname -r)/extra/"
    sudo depmod -a
fi

echo "==> Configuring auto-load at boot (/etc/modules-load.d/omnibook-ffa-fix.conf)..."
echo "omnibook_ffa_fix" | sudo tee /etc/modules-load.d/omnibook-ffa-fix.conf >/dev/null

echo "==> Done! Current power supplies:"
ls -la /sys/class/power_supply/
