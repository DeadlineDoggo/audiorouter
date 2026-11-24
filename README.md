# SoundRoot

**Route application audio to different outputs — visually, from the system tray.**

SoundRoot is a KDE Plasma 6 plasmoid that lets you create audio "rooms" — named groups of source-to-sink connections — and switch between them instantly with keyboard shortcuts or a click.

![CI](https://github.com/DeadlineDoggo/audiorouter/actions/workflows/ci.yml/badge.svg)
![Version](https://img.shields.io/github/v/release/DeadlineDoggo/audiorouter?include_prereleases&color=orange&cacheSeconds=600)
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
- **Route Into Calls** — Send a node or a microphone into an application that records (Teams, Discord, OBS, a browser call): music and your voice in one call ([how it works](docs/RECORDING.md))
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

### Packages (Arch, Debian, Fedora)

Download the package for your distribution from [Releases](https://github.com/DeadlineDoggo/audiorouter/releases). It installs where Plasma looks for plugins, so nothing else has to be set up:

```bash
sudo pacman -U soundroot-*.pkg.tar.zst   # Arch Linux
sudo apt install ./soundroot_*.deb       # Debian 13 and systems with the same Plasma and Qt
sudo dnf install ./soundroot-*.rpm       # Fedora
```

Then restart Plasma (or log out and in) and add the widget as below.

### Source archive (any other distribution)

Download `soundroot-*.tar.gz` from [Releases](https://github.com/DeadlineDoggo/audiorouter/releases), extract, and run:

```bash
tar -xzf soundroot-*.tar.gz
cd soundroot-*
bash install-local.sh
```

The script builds SoundRoot, installs it into `~/.local`, and ends by printing the command that restarts Plasma so it loads the new plugin. Run that command (or log out and back in), then right-click your panel → **Add Widgets** → search **SoundRoot** → drag to panel.

> **Note:** SoundRoot has two parts: the interface and a compiled plugin that does the audio routing. The `.plasmoid` file and the KDE Store's "Get New Widgets" can only deliver the interface. Without the plugin the widget shows a notice and cannot create rooms — use the `.tar.gz` + `install-local.sh`. See [Troubleshooting](#troubleshooting).

### Build from Source

```bash
git clone https://github.com/DeadlineDoggo/audiorouter.git
cd audiorouter
bash install-local.sh
```

Then restart Plasma with the command the script prints (or log out and back in) and add the widget as above.

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

## Troubleshooting

### "SoundRoot's plugin is not loaded" / rooms cannot be created

Plasma found the interface but not the compiled plugin. (Versions up to 0.8.1 did not say so: "New Room" simply did nothing.) The usual causes:

- **Installed from the KDE Store or a `.plasmoid` file.** Those contain only the interface. Install from the `.tar.gz` with `install-local.sh`.
- **Plasma was not restarted the right way after installing.** Plasma looks for plugins in the folders listed in `QT_PLUGIN_PATH`. The installer records the folder for future logins, so logging out and back in always works. To avoid that, use the restart command the installer prints — a plain `kquitapp6 plasmashell; kstart plasmashell` starts Plasma with the old environment.
- **Installed with 0.8.1 or older on Debian, Ubuntu, KDE neon, Fedora or openSUSE.** Those put the plugin in `~/.local/lib/x86_64-linux-gnu/plugins` or `~/.local/lib64/plugins`, but the old installer always recorded `~/.local/lib/plugins`. Run the current `install-local.sh` again; it records the folder the plugin really went to.

To check by hand:

```bash
find ~/.local -name 'org.kde.plasma.soundroot.so'       # where the plugin is
cat ~/.config/environment.d/50-soundroot.conf           # the folder Plasma is told about
```

The second must name the folder that contains `plasma/applets/` from the first.

### Removing SoundRoot

```bash
rm -rf ~/.local/share/plasma/plasmoids/org.kde.plasma.soundroot
find ~/.local -name 'org.kde.plasma.soundroot.so' -delete
rm -f ~/.config/environment.d/50-soundroot.conf
```

Your rooms stay in `~/.config/audiorouter/groups.json`; delete that too if you want them gone.

### Network nodes do not find each other

See [docs/NETWORK_NODES.md](docs/NETWORK_NODES.md#when-devices-do-not-find-each-other) — most often a firewall blocking UDP ports 47780–47781.

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

The tests run against a fake sound server and over the loopback interface; they need neither PipeWire nor Plasma. GitHub Actions runs them on Arch, Debian and Fedora for every push to `main` and every pull request ([ci.yml](.github/workflows/ci.yml)). `tests/run-in-netns.sh build/tests/tst_netengine` also exercises multicast discovery on machines whose firewall blocks it.

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
