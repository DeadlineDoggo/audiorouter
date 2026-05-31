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

- **Audio Rooms** — Create named, colour-coded rooms with independent routing configurations
- **Many-to-Many Routing** — Wire any source to any combination of output sinks
- **Per-Group Volume** — Volume control (0–150%) per audio room
- **Drag-to-Wire** — Visual connection board with bezier wires coloured by sink
- **Global Keyboard Shortcuts** — Switch rooms instantly via configurable KDE shortcuts (default: `Ctrl+Alt+→` / `Ctrl+Alt+←`)
- **Persistent Config** — All rooms and routes are saved and restored automatically
- **Drag-Reorder Rooms** — Arrange rooms in the sidebar by drag-and-drop
- **System Tray Integration** — Compact tray icon with full popup UI

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

**Dependencies:** CMake, Extra CMake Modules, Qt 6, KDE Frameworks 6, PipeWire (with pipewire-pulse)

## Usage

1. **Add the widget** to your panel or desktop
2. **Create a room** — click the "+" button, pick a name, colour, and icon
3. **Add routes** — select a source app and one or more output sinks
4. **Activate** — toggle the room switch or press `Ctrl+Alt+→` to cycle rooms
5. **Adjust volume** — use the per-route slider in the room detail view

### Configuration

Settings are stored in:

```
~/.config/audiorouter/groups.json
```

Keyboard shortcuts can be configured in **System Settings → Shortcuts → SoundRoot**.

## Keyboard Shortcuts

| Action | Default Shortcut |
|---|---|
| Next Room | `Ctrl + Alt + →` |
| Previous Room | `Ctrl + Alt + ←` |

## Project Structure

```
package/        # KDE Plasmoid package (metadata.json, QML UI)
plugin/         # C++ plugin entry point
backend/        # PulseAudio/PipeWire backend (AudioBackend, models)
promo-web/      # Project website (static HTML)
```

## Legal

- [Privacy Policy](PRIVACY_POLICY.md)
- [Terms of Use](TERMS_OF_USE.md)
- [Privacy & Permissions](PRIVACY_AND_PERMISSIONS.md)

## License

This project is licensed under the **GNU General Public License v2.0 or later** — see the [LICENSE](LICENSE) file for details.
