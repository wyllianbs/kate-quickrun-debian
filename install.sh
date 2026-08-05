#!/usr/bin/env bash
#
# Convenience build + install for the Kate Quick Run plugin.
#
#   ./install.sh          → build and install system-wide (needs sudo)
#   ./install.sh --user   → build and install into ~/.local (no sudo)
#
# System-wide is the recommended mode; --user is handy for testing without
# touching system files (needs QT_PLUGIN_PATH to point at ~/.local — see below).

set -euo pipefail

MODE="system"
[[ "${1:-}" == "--user" ]] && MODE="user"

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SRC_DIR}/build"

if [[ "${MODE}" == "user" ]]; then
    PREFIX="${HOME}/.local"
    cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${PREFIX}"
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    cmake --install "${BUILD_DIR}"
    echo
    echo "Installed into ${PREFIX}."
    echo "If Kate does not see the plugin, launch it once with:"
    echo "  QT_PLUGIN_PATH=${PREFIX}/lib/\$(uname -m)-linux-gnu/plugins kate"
else
    cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    echo "Installing system-wide (sudo)…"
    sudo cmake --install "${BUILD_DIR}"
    # Refresh the plugin/service cache so Kate picks up the new plugin.
    command -v kbuildsycoca6 >/dev/null 2>&1 && kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    echo
    echo "Done. Restart Kate and enable 'Quick Run' in Settings → Configure Kate → Plugins."
fi
