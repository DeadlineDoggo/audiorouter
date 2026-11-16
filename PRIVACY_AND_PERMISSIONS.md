# SoundRoot Privacy and Permissions

This document describes what data SoundRoot uses, where it is stored, and which privileges/capabilities are required for normal operation.

Scope: current implementation in this repository.

## 1. Personal Data and Device Data Used

SoundRoot does not collect identity data such as account names, passwords, email addresses, contacts, or files from your home directories.

It does process session-level audio metadata provided by PulseAudio/PipeWire to perform routing. Some of this metadata can be considered personal data, because it can reveal app usage habits.

### 1.1 Data read from audio server (runtime only)

From active audio streams and sinks, SoundRoot reads:

- Application name (`appName`, from `PA_PROP_APPLICATION_NAME`)
- Media name/title (`mediaName`, from `PA_PROP_MEDIA_NAME`, when available)
- Application icon name (`iconName`, from `PA_PROP_APPLICATION_ICON_NAME`)
- Stream identifiers and technical fields (sink-input index, channel count, current sink index)
- Output sink names and descriptions

Purpose:

- Show available sources/outputs in the UI
- Match routes to active application streams
- Move streams to selected outputs

Retention:

- Kept in memory while the plasmoid is running
- Refreshed from the audio server on events
- Not exported or transmitted externally

### 1.2 Data stored on disk (configuration)

SoundRoot stores user-defined routing configuration at:

- `~/.config/audiorouter/groups.json`

Stored fields:

- Group ID (UUID-like string)
- Group name, color and icon
- Group active flag
- Routes per group, including:
  - Source application name (`source`)
  - The media name the stream had when it was wired (`media`), used to find it again
  - Output sink names (`outputs`)
- Virtual nodes per group: name, internal sink name, wired outputs
- For network nodes: channel name, passphrase and optional peer addresses
- Descriptions of the output devices that rooms refer to (`sinkLabels`), to name a device while it is unplugged

The file is created readable and writable by its owner only (mode 0600), because the network passphrases in it are stored as plain text.

Purpose:

- Persist routing setup between sessions
- Re-apply active groups when the audio server reconnects

Retention:

- Until user edits or deletes the config file

### 1.3 Keyboard shortcut metadata

SoundRoot registers global actions through KDE GlobalAccel (`nextRoom`, `prevRoom`) so users can assign or use shortcuts.

Notes:

- Shortcut bindings are handled by KDE infrastructure
- This project does not implement custom telemetry or remote logging for shortcut usage

### 1.4 Network nodes (only when you enable one)

Nothing in this section applies unless you turn on "Share over the local network" for a virtual node, and then only while that node's room is switched on.

What is sent, to other devices on the same local network:

- The audio that applications play into that node. SoundRoot records it from the node's own internal sink. It never records a microphone or any other source, and the recording cannot be redirected to one.
- This device's host name and the node's name, so the other devices can show who is connected.
- Once a second, a small announcement so devices can find each other.

How:

- Directly between the devices, over UDP (ports 47780 and 47781). No server, no cloud, nothing leaves the local network: packets from public addresses are ignored and announcements are not routed.
- Everything is encrypted and authenticated with a key derived from the channel passphrase. Without the passphrase an observer on the network sees only that a device takes part in some channel.

Who can listen:

- Every device that has the channel name and the passphrase. Treat the passphrase like a Wi-Fi password.

What is received:

- Audio from the other devices on the channel is played on the outputs you wired to the node. It is not stored.

Details: `docs/NETWORK_NODES.md`.

## 2. Data Not Used

SoundRoot does not intentionally:

- Send data to external servers (no telemetry/analytics code in this repository)
- Access microphone content
- Record or transmit audio, except from a network node you enabled (section 1.4)
- Inspect file contents outside its own config file path
- Collect browser history, contacts, location, or credentials

## 3. Required Privileges and Capabilities

SoundRoot is a user-session KDE Plasma applet. It does not require root privileges for normal use.

### 3.1 Required runtime capabilities

- Access to the user audio server via PulseAudio compatibility interface
  - Needed to enumerate sinks/sink-inputs and apply routing operations
- Permission to load/unload PulseAudio modules in the current user session
  - Needed for multi-output routing (`module-combine-sink`) and virtual nodes (`module-null-sink`, `module-loopback`)
- Only with a network node enabled: UDP on the local network (ports 47780 and 47781), and a record and a playback stream on the audio server
- Read/write access to user config directory
  - Needed for `~/.config/audiorouter/groups.json`
- KDE Plasma/KF6 applet runtime and GlobalAccel integration
  - Needed for applet lifecycle and global shortcuts

### 3.2 Explicitly not required

- Root or sudo
- Network access, unless you enable a network node
- Access to system-wide user databases

## 4. Security/Privacy Implications

Even without telemetry, routing metadata may be sensitive:

- App names and media names can reveal what is currently being used or played
- Saved route names can indirectly reveal user habits (for example app-specific routing)
- A network node sends audio to every device that knows its passphrase; the passphrase is stored unencrypted in the config file

Recommended local protections:

- Keep normal user account permissions secure
- Restrict access to your home directory
- Remove or sanitize `~/.config/audiorouter/groups.json` if sharing logs/backups

## 5. Third-Party Components Involved

- PulseAudio (or PipeWire's PulseAudio compatibility server)
- KDE Frameworks 6 (including GlobalAccel)
- Qt 6
- libsodium (encryption of network nodes)

Their own platform behavior may involve additional storage or IPC managed outside this repository.

## 6. Operational Summary

- Processes audio stream metadata locally to route audio
- Stores only user routing configuration locally
- Requires user-session audio control and local config write access
- Uses no built-in remote telemetry in this codebase
- Sends audio over the local network only from network nodes you enable, encrypted, to devices that share the passphrase
