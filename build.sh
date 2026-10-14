#!/usr/bin/env bash
# Usage: ./build.sh [install|uninstall]
#   (no argument)  build into ./build
#   install        build from scratch, install /usr/bin/dscn + systemd service, enable and start it
#   uninstall      stop and remove everything install put on the system
set -e

cd "$(dirname "$(readlink -f "$0")")"

build() {
    command -v meson >/dev/null && pkg-config --exists libevdev || {
        echo "Missing dependency: meson + libevdev (dev). Install them (e.g." >&2
        echo "'sudo pacman -S meson libevdev libnotify') and re-run." >&2
        exit 1
    }
    meson setup build --reconfigure --buildtype=release
    meson compile -C build
}

install_dscn() {
    command -v notify-send >/dev/null || {
        echo "Missing dependency: notify-send (e.g. 'sudo pacman -S libnotify')." >&2
        exit 1
    }

    build

    sudo modprobe uinput
    sudo install -Dm644 misc/dscn.conf /etc/modules-load.d/dscn.conf
    sudo install -Dm755 build/dscn /usr/bin/dscn
    sudo install -Dm644 misc/dscn.service /usr/lib/systemd/system/dscn.service

    sudo systemctl daemon-reload
    sudo systemctl enable dscn.service
    sudo systemctl restart dscn.service

    cat <<EOF
==> dscn installed and started (system service, runs as root).

  Check it found your keyboard with:
       journalctl -u dscn.service -b
EOF
}

uninstall_dscn() {
    sudo systemctl disable --now dscn.service 2>/dev/null || true

    sudo rm -f /usr/bin/dscn
    sudo rm -f /usr/lib/systemd/system/dscn.service
    sudo rm -f /etc/modules-load.d/dscn.conf

    sudo systemctl daemon-reload

    echo "==> dscn uninstalled."
}

case "${1:-}" in
    "")        build ;;
    install)   install_dscn ;;
    uninstall) uninstall_dscn ;;
    *)
        echo "Usage: $0 [install|uninstall]" >&2
        exit 1
        ;;
esac
