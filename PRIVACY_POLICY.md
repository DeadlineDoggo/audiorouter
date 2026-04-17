# SoundRoot Privacy Policy

Effective date: 2026-04-10

This Privacy Policy explains how SoundRoot (the KDE Plasma audio-routing applet in this repository) handles data.

## 1. Who this policy applies to

This policy applies to users running SoundRoot locally on Linux in their own user session.

## 2. Data we process

SoundRoot does not request account credentials, contacts, location, or browser history.

To provide audio routing, SoundRoot processes session-level audio metadata exposed by PulseAudio or PipeWire's PulseAudio compatibility server:

- Application name (for example from `PA_PROP_APPLICATION_NAME`)
- Media name/title when available (for example from `PA_PROP_MEDIA_NAME`)
- Application icon name (for example from `PA_PROP_APPLICATION_ICON_NAME`)
- Technical stream metadata (sink-input index, sink index, channel count)
- Output sink names and sink descriptions

Why this is needed:

- List available audio sources and outputs in UI
- Match saved routes to active streams
- Move streams between sinks and apply route volume

## 3. Data we store

SoundRoot stores only routing configuration in the local user config directory:

- `~/.config/audiorouter/groups.json`

Stored values include:

- Group identifiers
- Group names and colors
- Group active status
- Route source app names
- Route output sink names
- Route volume percentages

Purpose of storage:

- Keep your routing setup between sessions
- Re-apply active groups after audio server reconnect

Retention:

- Data remains until changed or deleted by the user

## 4. Data we do not intentionally collect or transmit

Within this repository implementation, SoundRoot does not intentionally:

- Send analytics or telemetry to external servers
- Upload routing configuration to cloud services
- Record microphone content or audio samples

## 5. Legal basis and control

SoundRoot processes local data strictly for app functionality requested by the user (audio routing and configuration persistence).

User control options:

- Disable or remove routes/groups in the UI
- Delete `~/.config/audiorouter/groups.json`
- Stop using the plasmoid and uninstall it

## 6. Required local permissions

SoundRoot requires standard user-session capabilities:

- Access to PulseAudio-compatible server APIs in the current user session
- Ability to read/write the local config file path above
- KDE Plasma/KF6 applet runtime permissions for UI and shortcut integration

SoundRoot does not require root privileges for normal operation.

## 7. Third-party platform behavior

SoundRoot depends on Qt, KDE Frameworks, and PulseAudio/PipeWire integration. These components may have their own data handling behavior outside this repository. Review their official policies if needed.

## 8. Security notice

Route names and media/application names may reveal usage habits. Protect your local account and home-directory access, especially on shared machines.

## 9. Changes to this policy

This policy may be updated when project behavior changes. The latest version in the repository should be treated as authoritative for the current code.
