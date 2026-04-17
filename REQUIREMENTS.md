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
  - KDE tray icon implemented as a Plasma 6 Plasmoid (PlasmoidItem).
  - Compact representation shows audio icon in system tray panel.
  - Full popup opens on click with complete group management UI.
- [x] Allow many to many connections
  - One source can route to multiple outputs using PulseAudio combined sinks.
  - Same source can be reused across groups.
  - Multi-output selection via inline "Add Output" flow in route editor.
- [x] Manage volume
  - Per-route volume slider (0–150%) in group detail UI.
  - Volume is persisted in settings and applied to matching sink-input streams.
- [x] Be able to group sources and outputs
  - Full group CRUD: create (with name + color), delete, toggle active.
  - Group activation immediately applies/deactivates all routes via PulseAudio.
  - Group list with active switch toggles in main popup view.
- [x] View audio source devices
  - Live source list (sink-inputs) and output list (sinks) exposed to QML.
  - Source/output ComboBoxes populated from PulseAudio live data when adding routes.

## Should have
- [x] Remember setup
  - Groups/routes are persisted to JSON (~/.config/audiorouter/groups.json).
  - Group active state is persisted and re-applied after PulseAudio connects.

- [ ] Create profiles
  - Not implemented yet as a first-class profile entity (current groups can act as a basic profile-like mechanism).

## Nice to have
- [ ] Test sound button
- [ ] Auto switch profile
- [ ] Hotkey swap
- [ ] Visual indicator of active profile/device

## Future/Vision
- [ ] Allow virtual devices output (beyond current combined sink workflow)


Fix:
- New Group somehow have removed custom color and icon
- Shortcut
- Scrolling