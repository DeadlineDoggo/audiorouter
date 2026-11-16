#!/bin/bash
# Build SoundRoot and install it for the current user (into ~/.local).
#
#   bash install-local.sh              build + install the widget and its plugin
#   bash install-local.sh --qml-only   only the interface, without the plugin
#                                      (for UI work; rooms cannot be created)
#
# SoundRoot has two parts: the interface (QML) and a compiled plugin that does
# the audio routing.  Plasma only finds the plugin if its folder is on
# QT_PLUGIN_PATH, so this script records that folder for future logins and
# tells you how to restart Plasma so it is picked up right away.
#
# Environment: BUILD_DIR (default: ./build), CMAKE_ARGS (extra cmake options).
set -euo pipefail

PLASMOID_ID="org.kde.plasma.soundroot"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_PREFIX="$HOME/.local"

if [[ "${1:-}" == "--qml-only" ]]; then
    echo "==> Interface only (no plugin)"
    TARGET="$INSTALL_PREFIX/share/plasma/plasmoids/$PLASMOID_ID"
    rm -rf "$TARGET"
    mkdir -p "$TARGET"
    cp -r "$SCRIPT_DIR/package/"* "$TARGET/"
    echo "Installed to $TARGET"
    echo "Without the plugin the widget only shows a notice. Run this script without --qml-only for the real thing."
    exit 0
fi

echo "==> Building"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/build}"
# shellcheck disable=SC2086   # CMAKE_ARGS is meant to be split into words
cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
      -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF \
      ${CMAKE_ARGS:-}
cmake --build "$BUILD_DIR" --parallel
cmake --install "$BUILD_DIR"

# Where did the plugin go?  That differs between distributions:
#   lib/plugins (Arch), lib64/plugins (Fedora, openSUSE),
#   lib/x86_64-linux-gnu/plugins (Debian, Ubuntu, KDE neon).
# Ask the install manifest instead of guessing.
PLUGIN_FILE="$(grep -m1 "/plasma/applets/$PLASMOID_ID\.so\$" "$BUILD_DIR/install_manifest.txt" || true)"
if [[ -z "$PLUGIN_FILE" || ! -f "$PLUGIN_FILE" ]]; then
    echo "error: the plugin was not installed (not found in $BUILD_DIR/install_manifest.txt)" >&2
    exit 1
fi
PLUGIN_ROOT="${PLUGIN_FILE%/plasma/applets/*}"

# Left over from very old versions
rm -f "$INSTALL_PREFIX/lib/plugins/plasma/applets/plasma_applet_$PLASMOID_ID.so"

# For every future login
ENV_FILE="$HOME/.config/environment.d/50-soundroot.conf"
mkdir -p "$(dirname "$ENV_FILE")"
echo "QT_PLUGIN_PATH=$PLUGIN_ROOT:\${QT_PLUGIN_PATH}" > "$ENV_FILE"

# For this login: the file above is only read when you log in.  Tell the
# session manager now, so a restarted plasmashell finds the plugin.
SESSION_MANAGED=no
if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1; then
    current="$(systemctl --user show-environment | sed -n 's/^QT_PLUGIN_PATH=//p')"
    case ":$current:" in
        *":$PLUGIN_ROOT:"*) ;;   # already there
        *) systemctl --user set-environment "QT_PLUGIN_PATH=$PLUGIN_ROOT${current:+:$current}" ;;
    esac
    if systemctl --user is-active --quiet plasma-plasmashell.service; then
        SESSION_MANAGED=yes
    fi
fi

echo
echo "Installed:"
echo "  interface  $INSTALL_PREFIX/share/plasma/plasmoids/$PLASMOID_ID"
echo "  plugin     $PLUGIN_FILE"
echo "  login env  $ENV_FILE"
echo
echo "Now restart Plasma so it loads the plugin - either log out and back in, or run:"
echo
if [[ "$SESSION_MANAGED" == yes ]]; then
    echo "  systemctl --user restart plasma-plasmashell.service"
else
    echo "  kquitapp6 plasmashell; QT_PLUGIN_PATH=\"$PLUGIN_ROOT\${QT_PLUGIN_PATH:+:\$QT_PLUGIN_PATH}\" kstart plasmashell"
fi
echo
echo "Then: right-click the panel -> Add Widgets -> SoundRoot."
echo "If the widget says its plugin is not loaded, Plasma was started without the line above."
