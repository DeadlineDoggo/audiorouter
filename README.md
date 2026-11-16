# SoundRoot

**Route application audio to different outputs — visually, from the system tray.**

SoundRoot is a KDE Plasma 6 plasmoid that lets you create audio "rooms" — named groups of source-to-sink connections — and switch between them instantly with keyboard shortcuts or a click.

![Version](https://img.shields.io/badge/version-0.8.1--beta-orange)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)
![Plasma](https://img.shields.io/badge/KDE%20Plasma-6.0%2B-informational)
![PipeWire](https://img.shields.io/badge/PipeWire-compatible-green)
![Status](https://img.shields.io/badge/status-beta-yellow)

---

## Features

- **Audio Rooms** — Named, colour-coded rooms, each with its own wiring. One room is active at a time
- **Many-to-Many Routing** — Wire any application stream to any combination of outputs
- **Drag-to-Wire** — Visual board: drag from a dot to another dot; click a wire to remove it
- **Virtual Nodes** — Mix several applications into one node and send that to one or more outputs
- **Network Nodes** — Share a virtual node between devices on the same local network, encrypted ([how it works](docs/NETWORK_NODES.md))
- **Stays Applied** — Streams that start later, devices plugged in later and a restarted sound server are picked up automatically
- **Nothing Gets Lost** — Applications that are not playing and unplugged devices stay on the board, greyed out, with their wires
- **Volume Controls** — Per application stream and per output device (0–150%)
- **Global Keyboard Shortcuts** — Switch rooms from anywhere (default: `Ctrl+Alt+→` / `Ctrl+Alt+←`)
- **Persistent Config** — Rooms and wires are saved and re-applied at login
- **System Tray Integration** — Tray icon with a dot in the active room's colour

## Installation

### KDE Store

Install directly from the [KDE Store](https://store.kde.org/p/2357238/) via Plasma's "Get New Widgets" dialog, or:

```bash
# Search for SoundRoot in Discover / Add Widgets → Get New…
```

### GitHub Releases

Download `soundroot-*.tar.gz` from [Releases](https://github.com/DeadlineDoggo/audiorouter/releases), extract, and run:

```bash
tar -xzf soundroot-*.tar.gz
cd soundroot-*
bash install-local.sh
```

Then restart Plasma and add the widget:

```bash
kquitapp6 plasmashell && kstart6 plasmashell
```

Right-click your panel → **Add Widgets** → search **SoundRoot** → drag to panel.

> **Note:** The `.plasmoid` file (also available in Releases) only installs the QML layer — audio routing will not work without the C++ backend. Use the `.tar.gz` + `install-local.sh` path for full functionality.

### Build from Source

```bash
git clone https://github.com/DeadlineDoggo/audiorouter.git
cd audiorouter
bash install-local.sh
```

Then restart Plasma and add the widget:

```bash
kquitapp6 plasmashell && kstart6 plasmashell
```

Right-click your panel → **Add Widgets** → search **SoundRoot** → drag to panel.

**Dependencies:** CMake, Extra CMake Modules, Qt 6 (Core, Gui, Quick, Network; Test for the tests), KDE Frameworks 6, libplasma, libpulse, libsodium, and PipeWire with pipewire-pulse (or PulseAudio) at run time.

On Arch Linux:

```bash
sudo pacman -S --needed cmake extra-cmake-modules qt6-base qt6-declarative libplasma \
    kglobalaccel ki18n kxmlgui kcoreaddons kconfig kpackage libpulse libsodium
```

## Usage

1. **Add the widget** to your panel or desktop
2. **Create a room** — click "New Room", pick a name, colour and icon
3. **Wire it** — drag from the dot of an application to the dot of an output device. Drag to several outputs to play on all of them. Click a wire to remove it; right-click a card to remove all of its wires
4. **Switch it on** — toggle the room's switch, or press `Ctrl+Alt+→` to cycle rooms. While a room is on, changes apply immediately; while it is off, they are only saved
5. **Virtual nodes** — "Virtual node" adds a node with an IN and an OUT side: wire applications into IN and OUT to outputs. Its settings turn it into a [network node](docs/NETWORK_NODES.md)

Applications that do not have a wire in the active room play on the default output, as usual.

### Configuration

Settings are stored in:

```
~/.config/audiorouter/groups.json
```

The file is written atomically and is readable only by you (it contains network node passphrases).

Keyboard shortcuts can be configured in the widget's settings, or in **System Settings → Shortcuts → SoundRoot**.

### Network nodes

A network node connects SoundRoot on several devices: what is played into the node on one device comes out of the node on the others. Give the nodes the same channel name and passphrase. Audio is encrypted; the devices must reach each other over UDP ports 47780–47781, which many firewalls block by default. See [docs/NETWORK_NODES.md](docs/NETWORK_NODES.md).

## Keyboard Shortcuts

| Action | Default Shortcut |
|---|---|
| Next Room | `Ctrl + Alt + →` |
| Previous Room | `Ctrl + Alt + ←` |

## Project Structure

```
package/            # Plasmoid package: metadata.json and the QML UI (a view; it holds no routing state)
plugin/             # The applet: thin QML-facing layer over the backend
backend/
  audiobackend.*    # libpulse wrapper: sinks, streams, modules, reconnecting
  routing/          # RoutingEngine (applies the active room) and StreamBinder (routes → live streams)
  net/              # Network nodes: protocol, encryption, jitter buffer, UDP engine
  models/, data/    # List models for QML; configuration and its JSON format
tests/              # Unit and integration tests (no sound server needed)
docs/               # Design documents
promo-web/          # Project website (static HTML)
```

### How routing works

The configuration says what should be; the sound server reports what is. `RoutingEngine` compares the two whenever the server reports a change and issues only the missing steps (move a stream, load or unload a module). That is why a room stays applied when applications restart their streams, devices come and go, or the sound server restarts.

## Development

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests run against a fake sound server and over the loopback interface; they need neither PipeWire nor Plasma. `tests/run-in-netns.sh build/tests/tst_netengine` also exercises multicast discovery on machines whose firewall blocks it.

To try a build without touching the plasmoid that is running in your panel, install it into a separate prefix and start it with `plasmawindowed`:

```bash
cmake -B build -DCMAKE_INSTALL_PREFIX=/tmp/soundroot && cmake --build build && cmake --install build
QT_PLUGIN_PATH=/tmp/soundroot/lib/plugins XDG_DATA_DIRS=/tmp/soundroot/share:/usr/share \
    plasmawindowed org.kde.plasma.soundroot
```

## Legal

- [Privacy Policy](PRIVACY_POLICY.md)
- [Terms of Use](TERMS_OF_USE.md)
- [Privacy & Permissions](PRIVACY_AND_PERMISSIONS.md)

## License

This project is licensed under the **GNU General Public License v2.0 or later** — see the [LICENSE](LICENSE) file for details.
