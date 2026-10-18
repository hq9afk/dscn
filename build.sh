#!/usr/bin/env bash

# Build script for dscn
# Use:
# <empty>: build normally
# setup: install dependencies
# install: build, deploy into /usr and enable the service
# run: install and follow the log
# uninstall: stop the service and remove the installed files

set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"

cmd_setup() { sudo pacman -S --needed meson ninja gcc pkgconf libevdev; }

cmd_build() {
    [[ -f build/build.ninja ]] || meson setup build --prefix=/usr
    meson compile -C build
}

cmd_install() {
    cmd_build
    sudo modprobe uinput
    sudo meson install -C build --no-rebuild
    sudo systemctl daemon-reload
    sudo systemctl enable dscn.service
    sudo systemctl restart dscn.service
}

cmd_run() { cmd_install; journalctl -fu dscn.service; }

cmd_uninstall() {
    sudo systemctl disable --now dscn.service || true
    sudo ninja -C build uninstall
    sudo systemctl daemon-reload
}

main() {
    local cmd="${1:-build}"
    case "$cmd" in
    setup | build | install | run | uninstall) shift || true; "cmd_$cmd" "$@" ;;
    *) echo "unknown command: $cmd" >&2; exit 2 ;;
    esac
}

main "$@"
