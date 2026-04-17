#!/bin/bash
# Quick-install the SoundRoot plasmoid for local testing.
#
# Default: full CMake build + install so the C++ backend is available.
# Use --qml-only only when you intentionally want to test the UI without it.
set -euo pipefail

PLASMOID_ID="org.kde.plasma.soundroot"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ "${1:-}" != "--qml-only" ]]; then
    echo "==> Full build + install (CMake)"
    BUILD_DIR="$SCRIPT_DIR/build"
    INSTALL_PREFIX="$HOME/.local"
    LEGACY_PLUGIN_PATH="$INSTALL_PREFIX/lib/plugins/plasma/applets/plasma_applet_org.kde.plasma.soundroot.so"

    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$SCRIPT_DIR" -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
    make -j"$(nproc)"
    make install

    rm -f "$LEGACY_PLUGIN_PATH"

    # Ensure QT_PLUGIN_PATH includes local plugins so Plasma can find the .so
    export QT_PLUGIN_PATH="$INSTALL_PREFIX/lib/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"

    # Persist for future sessions via systemd environment.d
    ENV_DIR="$HOME/.config/environment.d"
    ENV_FILE="$ENV_DIR/50-soundroot.conf"
    mkdir -p "$ENV_DIR"
    echo 'QT_PLUGIN_PATH=${HOME}/.local/lib/plugins:${QT_PLUGIN_PATH}' > "$ENV_FILE"

    echo ""
    echo "Done. C++ plugin + QML package installed to $INSTALL_PREFIX"
    echo "      QT_PLUGIN_PATH set for this shell and persisted in $ENV_FILE"
    echo "      Removed legacy plugin file if it existed: $LEGACY_PLUGIN_PATH"
else
    echo "==> QML-only package install (no C++ backend)"
    TARGET="$HOME/.local/share/plasma/plasmoids/$PLASMOID_ID"

    rm -rf "$TARGET"
    mkdir -p "$TARGET"
    cp -r "$SCRIPT_DIR/package/"* "$TARGET/"

    echo "Done. QML package installed to $TARGET"
    echo "Warning: the native backend is not installed in this mode."
fi

echo ""
echo "Quick-test commands:"
echo "  source build/prefix.sh                # set env vars for this shell"
echo "  plasmawindowed $PLASMOID_ID          # standalone test window"
echo "  QT_PLUGIN_PATH=~/.local/lib/plugins:\$QT_PLUGIN_PATH plasmashell --replace &"
echo "  kquitapp6 plasmashell && QT_PLUGIN_PATH=~/.local/lib/plugins:\$QT_PLUGIN_PATH plasmashell &"
