# Definovanie cieľa a aplikácie
- **Kto?**
- PC Používateľ s viacerými audio výstupmi,  Advanced PC user, Discord moderator, mix audia, separated audio per stream, nemožnosť rozdelenia audia na viaceré skupiny…
- **Prečo?**
- Možnosť jednoduchej izolácie audio skupín per use.
- **Čo?**
- Widget aplikácia pre system tray pre KDE plasma desktop na platforme Linux.
- ![alt text](image(1).png)

# Moscow prioritization:
## Must have
- System tray icon
  - A tray icon for quick access
  - I can switch audio without opening the app
- Allow many to many connections
  - Connect one audio source to multiple outputs
  - I can play sound on speakers and headset simultaneously
- Manage volume
  - Adjust volume for each route
  - I can lower game volume while keeping music loud
- Be able to group sources and outputs
  - Create separate sound channels
  - Use upps without sound overlap between apps
- View audio source devices
  - Be able to pick up sources into sound group
  - I could make custom separated groups.
## Should have
- Remember setup
  - App remembers my last setup
  - I don’t need to reconfigure everything
- Create profiles
  - Create a “Gaming” audio profile
  - All my devices are set correctly with one click
  - I can switch between “Gaming” and “Music” profiles easily
## Nice to have
- Test sound button
  - Test sound button for each device
  - I can verify it works
- Auto switch profile
  - Auto-apply a profile when I connect headphones
  - I don’t need to change settings manually
- Hotkey swap
  - Swap profiles based on shortcut
  - Fast swap profiles based on ocaasion
- Visual indicator
  - Visual indicator of active device / profile
  - I instantly know where audio goes or which profile is active
## Future/Vision
- Allow virtual devices output
  - Create virtual audio outputs
  - I can manage audio routing more flexibly

# Implementation coverage (current project state)

## Must have
- [x] System tray icon
  - Plasma 6 plasmoid. The tray icon carries a dot in the active room's colour; the tooltip names the active room.
  - The popup opens on click with the room list and the routing board.
- [x] Allow many to many connections
  - One stream can be wired to several outputs (a `module-combine-sink` per set of outputs, shared by all streams with that set).
  - Several streams can be wired to one output.
- [x] Manage volume
  - A volume slider (0–150%) on every application stream and every output device, plus one for the default output.
  - These set the sound server's own volumes. They are not stored per room.
- [x] Be able to group sources and outputs
  - Rooms: create (name, colour, icon), edit, delete, reorder, switch on and off. One room is on at a time.
  - Switching a room on applies its wiring; switching it off puts the streams back on the default output.
- [x] View audio source devices
  - Live lists of application streams and output devices.
  - Saved sources that are not playing and wired devices that are unplugged are shown greyed out.

## Should have
- [x] Remember setup
  - Rooms, wires and virtual nodes are saved to `~/.config/audiorouter/groups.json` (atomic write).
  - The active room is re-applied at login, when a stream or device appears later, and after the sound server restarts.
- [x] Create profiles
  - Rooms are the profiles.

## Nice to have
- [ ] Test sound button
- [ ] Auto switch profile
- [x] Hotkey swap
  - Global shortcuts for next / previous room (default `Ctrl+Alt+→` / `Ctrl+Alt+←`), handled in the plugin so they work before the popup has ever been opened.
- [x] Visual indicator of active profile/device
  - Coloured dot on the tray icon; "default" label on the default output; wires coloured by output.

## Future/Vision
- [x] Allow virtual devices output
  - Virtual nodes (`module-null-sink` + `module-loopback`) per room.
  - Network nodes: a virtual node shared between devices on the local network. See `docs/NETWORK_NODES.md`.

# Dependencies

Build: CMake ≥ 3.20, Extra CMake Modules, Qt 6 (Core, Gui, Quick, Network, Test), KDE Frameworks 6 (CoreAddons, Config, Package, GlobalAccel, I18n, XmlGui), libplasma, libpulse, libsodium.

Run: Plasma 6, and PipeWire with pipewire-pulse (or PulseAudio).

# Earlier notes ("Fix:")
- *New Group somehow have removed custom color and icon* — the room dialog was rebuilt; the chosen colour and icon are passed through on create and on edit. Not re-tested by hand with the colour picker.
- *Shortcut* — handled in the plugin now (see "Hotkey swap").
- *Scrolling* — every column of the board scrolls on its own, only when it does not fit.
