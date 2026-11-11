# Hex Voice Dictation (`hex-bin`) Hold-to-Dictate Shortcut on Hyprland / Omarchy

## Summary

After fixing the MT8901 SoundWire audio stack ([`audio/`](../audio/)), **`hex`** (`hex-bin 3.3.0-1`, `/usr/bin/hex service`) successfully opens the internal microphone array via PipeWire ALSA and loads its local Vulkan speech model (`Parakeet Unified English` / `unified-en-0.6b` on `NVIDIA RTX Spark N1X`, ~90 ms finalize latency).

However, setting a dictation hotkey inside Hex's UI (e.g., **`Alt+D`**, stored as `"dictation_hotkey": "option+d"` in `~/.local/share/hex/settings.json`) and holding it **does nothing** until push-to-talk press/release bindings are added to `~/.config/hypr/bindings.lua`.

---

## Root Cause

There are two separate Wayland / Hyprland interactions involved:

### 1. `xdg-desktop-portal-hyprland` Does Not Auto-Bind Keys for `GlobalShortcuts`

On Wayland, applications cannot capture global keyboard input directly. Hex registers its shortcuts through `org.freedesktop.portal.GlobalShortcuts`, which `xdg-desktop-portal-hyprland` (`1.4.1`) exposes to Hyprland via the `hyprland_global_shortcut_v1` protocol:

```text
$ hyprctl globalshortcuts
anomaly.hex:dictation-ALT-d -> Dictation
anomaly.hex:paste-last-CTRL-ALT-v -> Paste last dictation
anomaly.hex:capture-F9 -> Annotate
```

By design in Hyprland, registering a `GlobalShortcuts` ID does **not** bind any physical key chord; `xdg-desktop-portal-hyprland` returns an empty `trigger_description` because Hyprland requires explicit keybindings in its own configuration. Consequently, `hex.service` logs:

```text
INFO hex::linux::linux_input: the desktop assigns this Hex shortcut's key itself; Hex cannot confirm it name=Dictation
```

and `hex status --json` reports `"trigger": null` for every shortcut.

### 2. Hyprland's `global` Dispatcher Cannot Drive Hold-to-Dictate in `hex`

Even if `ALT + D` were bound to `hl.dsp.global("anomaly.hex:dictation-ALT-d")`, `hex-bin 3.3.0-1` only subscribes to the `Activated` (key-press) D-Bus signal on `org.freedesktop.portal.GlobalShortcuts` and does **not** subscribe to `Deactivated` (key-release). Triggering the portal shortcut starts dictation on press and never stops it on release.

Instead, Hex exposes synchronous CLI control commands over its owner-only socket (`~/.local/share/hex/service.sock`) specifically for compositor keybindings:

- **Press:** `hex dictation start`
- **Release:** `hex dictation stop`

---

## Fix (`~/.config/hypr/bindings.lua`)

Add a push-to-talk binding in `~/.config/hypr/bindings.lua` using Omarchy's `o.bind_hold` helper:

```lua
-- Hold Alt+D to dictate with Hex.
if o.cmd_present("hex") then
  o.bind_hold("ALT + D", "Start dictation (push-to-talk)", "hex dictation start", "Stop dictation (push-to-talk)", "hex dictation stop")
end
```

### Why `o.bind_hold` Instead of a Plain Release Bind

A plain Hyprland release bind on `ALT + D` fails whenever the user releases `Alt` a few milliseconds before `D` (because the active modifier mask is `0` rather than `ALT` when `D` comes up), or if another key or mouse button is released during the hold.

Omarchy's `o.bind_hold` (`/usr/share/omarchy/default/hypr/helpers.lua`) tracks `held` state in Lua on `ALT + D` press and configures the `D` release bind with `release = true`, `transparent = true`, `ignore_mods = true`, and `non_consuming = true`:

- Releasing `D` reliably triggers `hex dictation stop` regardless of whether `Alt` or `D` is released first.
- Normal typing of `d` and other chords (`SHIFT + ALT + D`, `SUPER + CTRL + D`, `SUPER + CTRL + ALT + D`) remain completely unaffected.
- Guarding with `o.cmd_present("hex")` avoids holding `ALT + D` if `hex-bin` is uninstalled.

### Optional Additional Hex Bindings & Settings

If you also want Hex's secondary shortcuts or terminal paste behavior:

```lua
if o.cmd_present("hex") then
  o.bind_hold("ALT + D", "Start dictation (push-to-talk)", "hex dictation start", "Stop dictation (push-to-talk)", "hex dictation stop")
  o.bind("CTRL + ALT + V", "Paste last dictation", "hex paste-last")
  o.bind("F9", "Hex Annotate", "hex annotate")
end
```

- **Wayland automatic paste dependencies:** Ensure `wl-clipboard` (`wl-copy`) and `wtype` are installed (`pacman -S --needed wl-clipboard wtype`), as Hyprland does not implement `org.freedesktop.portal.RemoteDesktop`.
- **Pasting into terminals (`Ctrl+Shift+V`):** By default Hex pastes via `Ctrl+V`. To send `Ctrl+Shift+V` for terminals that ignore `Ctrl+V`, enable Terminal paste:
  ```bash
  hex settings set paste_with_shift true
  ```

---

## Verification

Reload Hyprland and confirm zero config errors and active push-to-talk bindings:

```bash
hyprctl reload
hyprctl configerrors
omarchy menu keybindings --print | grep -i dictation
```

Expected output:

```text
ALT + D                             → Start dictation (push-to-talk)
ALT + D                             → Stop dictation (push-to-talk)
```

End-to-end CLI smoke test against the running `hex.service`:

```bash
hex dictation start && sleep 1 && hex dictation stop --print
```
