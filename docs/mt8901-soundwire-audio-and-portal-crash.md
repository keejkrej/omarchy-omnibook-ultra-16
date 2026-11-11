# MT8901 SoundWire Audio UCM Dispatch Bug & `xdg-desktop-portal` `SIGABRT` Cascade

## Summary

On the **HP OmniBook Ultra Laptop 16-bk0xxx** (`HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A`, NVIDIA N1X / `GB20B`, board `8E6A`) running Omarchy `4.5.0-3` (`omarchy-settings 4.5.0-3`, `linux-omarchy 7.3-3.1`), the entire built-in audio stack failed at session startup:

1. **No speakers, headphone jack, or microphone in PipeWire:** `alsa_card.platform-mtk_sdw_mc` (`card 1: mt8901-soundwire`) stayed on `Active Profile: off`, leaving PipeWire with only a `Dummy Output` sink and zero audio input sources.
2. **Repeated `xdg-desktop-portal` `SIGABRT` core dumps:** When the background voice-dictation daemon **`hex`** (`hex-bin 3.3.0-1`, `/usr/bin/hex service`) ran without any available audio capture source, it entered a tight retry loop testing dozens of channel/sample-format combinations per second. Each short-lived audio thread requested realtime scheduling from `/usr/lib/xdg-desktop-portal` (`org.freedesktop.portal.Realtime`), exposing a concurrent heap-corruption bug (`malloc(): unaligned tcache chunk detected`) that crashed `xdg-desktop-portal` with `SIGABRT` every ~2 minutes.

Fixing the ALSA UCM2 profile selection restores quad internal speakers, the 3.5mm headphone/headset jack, and the internal digital microphone array — and immediately stops `hex`'s retry storm and the `xdg-desktop-portal` crashes.

---

## 1. Hardware & SoundWire Topology on HP Board `8E6A`

The N1X audio subsystem exposes two ALSA cards:

- **Card 0 (`NVIDIA`, `snd_hda_acpi`, `NVDA2014:00`)**: HDMI / DisplayPort audio controller.
- **Card 1 (`mt8901-soundwire`, `mt8901_acp_sdw_legacy_mach`, `NVDA9022:00` + `NVDA9100:00`)**:
  - `CardDriver`: `mt8901-soundwir` (truncated from `mt8901-soundwire` by the 15-char ALSA driver-name limit)
  - `CardLongName`: `HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A`
  - `alsa.components`: `cfg-amp:4 spk:cs35l56 hs:cs42l43 mic:cs42l43-dmic`

SoundWire bus enumeration (`/sys/bus/soundwire/devices/`) shows a **split Link 0 + Link 1** topology:

| SoundWire Device | Bus : Link | Manufacturer : Part | Role |
| :--- | :--- | :--- | :--- |
| `sdw:0:0:01fa:3556:01:0` | `0:0` | Cirrus `0x01fa:0x3556` | Speaker Amplifier `AMP1` (`CS35L56`) |
| `sdw:0:0:01fa:3556:01:1` | `0:0` | Cirrus `0x01fa:0x3556` | Speaker Amplifier `AMP2` (`CS35L56`) |
| `sdw:0:0:01fa:3556:01:2` | `0:0` | Cirrus `0x01fa:0x3556` | Speaker Amplifier `AMP3` (`CS35L56`) |
| `sdw:0:0:01fa:3556:01:3` | `0:0` | Cirrus `0x01fa:0x3556` | Speaker Amplifier `AMP4` (`CS35L56`) |
| `sdw:0:1:01fa:4243:01` | `0:1` | Cirrus `0x01fa:0x4243` | Headset Jack Codec + DMICs (`CS42L43`) |

> **Note on `CS35L56` DSP firmware:** At boot, all four `cs35l56` amplifiers boot their built-in Halo DSP ROM (`DSP1: Firmware: 1a00d6 vendor: 0x2 v3.4.4, 37 algorithms`) and log `FIRMWARE_MISSING` / `Calibration disabled due to missing firmware controls` because HP-specific `cs35l56` `.wmfw`/`.bin` tuning files for `8E6A` are not yet in `linux-firmware-cirrus`. The amplifiers still pass audio cleanly in uncalibrated hardware mode (`AMP1..4 Speaker Volume = 0.00 dB`), so missing tuning firmware is **not** what broke audio output or input.

---

## 2. Root Cause #1 (Audio Stack): Sequential `If` Fallthrough in `mt8901-soundwir.conf`

`omarchy-settings 4.5.0-3` ships a board-validated UCM profile specifically for this topology at:

- `/usr/share/alsa/ucm2/MediaTek/mt8901-soundwir/HiFi-cs35l56x4-cs42l43.conf` (`4x CS35L56 on link 0 and CS42L43 on link 1`)

However, the top-level dispatcher `/usr/share/alsa/ucm2/MediaTek/mt8901-soundwir/mt8901-soundwir.conf` uses independent top-level `If.<name>` blocks without nesting later checks inside `False { ... }`:

```alsaucm
# CS35L56 on link 0: 4x CS35L56 (link 0) + CS42L43 (link 1)
If.cs35l56x4_cs42l43 {
	Condition {
		Type Path
		Mode exist
		Path "/sys/bus/soundwire/devices/sdw:0:0:01fa:3556:01:0"
	}
	True {
		SectionUseCase."HiFi" {
			File "/MediaTek/mt8901-soundwir/HiFi-cs35l56x4-cs42l43.conf"
			Comment "Play HiFi quality music"
		}
	}
}
...
# otherwise a CS42L43 on link 1: CS42L43 + 2x CS35L56, all on link 1
If.cs42l43_cs35l56x2 {
	Condition {
		Type Path
		Mode exist
		Path "/sys/bus/soundwire/devices/sdw:0:1:01fa:4243:01"
	}
	True {
		SectionUseCase."HiFi" {
			File "/MediaTek/mt8901-soundwir/HiFi-cs42l43-cs35l56x2.conf"
			Comment "Play HiFi quality music"
		}
	}
}
```

### Failure Sequence

1. In ALSA UCM Syntax 4, consecutive `If.<id>` blocks are **not** `else if` — every block whose `Condition` matches runs its `True` branch in order.
2. On the HP OmniBook Ultra 16 (`8E6A`), **both** `/sys/bus/soundwire/devices/sdw:0:0:01fa:3556:01:0` (link 0 amps) **and** `/sys/bus/soundwire/devices/sdw:0:1:01fa:4243:01` (link 1 codec) exist.
3. `If.cs35l56x4_cs42l43` sets `SectionUseCase."HiFi".File` to `HiFi-cs35l56x4-cs42l43.conf`, and then `If.cs42l43_cs35l56x2` immediately **overwrites** `SectionUseCase."HiFi".File` with `HiFi-cs42l43-cs35l56x2.conf` (an all-link-1 2-amp layout).
4. When WirePlumber / PipeWire ACP probes the `HiFi` verb (`spa-acp-tool -vvvv -c 1 info`), `HiFi-cs42l43-cs35l56x2.conf` routes `DL0` (`I032`/`I033`) to `O152`/`O153` -> `PDI3` (`SDW1_Playback1`, link-1 amplifiers) instead of `O150`/`O151` -> `PDI2..4` (link-0 amplifiers).
5. Because no speaker amplifiers exist on SoundWire link 1 on this board, the ASoC frontend `DL0_FE` (`hw:1,0`) has no connected backend DAI:
   ```text
   kernel: DL0_FE: ASoC: no backend DAIs enabled for DL0_FE, possibly missing ALSA mixer-based routing or UCM profile
   ```
   and `spa-acp-tool` logs:
   ```text
   Error opening PCM device _ucm0001.hw:mt8901soundwire,0: Invalid argument
   Profile 'HiFi (Headphones, Headset, Speaker)' mapping 'HiFi: Speaker: sink': output PCM open failed
   Profile HiFi (Headphones, Headset, Speaker) not supported
   Profile 'HiFi (Headphones, Mic, Speaker)' mapping 'HiFi: Speaker: sink': output PCM open failed
   Profile HiFi (Headphones, Mic, Speaker) not supported
   ```
6. PipeWire ACP drops the entire `HiFi` UCM verb, leaving only `off` and `pro-audio` profiles with `Active Profile: off`.

---

## 3. Root Cause #2 (Cascade Crash): `xdg-desktop-portal` `SIGABRT`

### Symptom

With no audio capture source in PipeWire and `hex-bin 3.3.0-1` (`/usr/bin/hex service`) running in the background, `/usr/lib/xdg-desktop-portal` (`xdg-desktop-portal 1.22.1-2`) repeatedly aborted (`SIGABRT`, `SI_TKILL`) roughly every two minutes (`PID 1525` at `13:12:57`, `PID 63676` at `13:14:52`):

```text
xdg-desktop-portal[1525]: malloc(): unaligned tcache chunk detected
xdg-desktop-portal[63676]: malloc(): unaligned tcache chunk detected
```

### Causal Chain & Core Dump Analysis

1. **Tight microphone polling in `hex`:** Because PipeWire exposed no microphone source, `hex` continuously retried opening `Default Audio Device` across dozens of channel counts (`1..12` channels), sample formats (`F32`, `I16`), and buffer sizes (`Default`, `Fixed(960)`, `Fixed(4800)`) every second, consuming ~69% of a CPU core.
2. **Flood of failing `org.freedesktop.portal.Realtime` requests:** Each audio stream open attempt spawned a short-lived audio thread in `hex` (`PID 47356`) that requested realtime thread priority via `xdg-desktop-portal`'s Realtime portal, then immediately exited when opening the stream failed. `xdg-desktop-portal` logged dozens of errors per second:
   ```text
   /usr/lib/xdg-desktop-portal[63676]: Realtime error: Could not map tid 67041 of pid 47356: Process ids could not be found: 67041
   /usr/lib/xdg-desktop-portal[63676]: Realtime error: Could not get pidns for pid 47356: pidns required but no pidfd provided
   /usr/lib/xdg-desktop-portal[67299]: Failed to parse 'status::NSpid:': Numerical result out of range
   ```
3. **Concurrent Realtime portal workers & `tcache` corruption:** Inspecting the `systemd-coredump` image for `PID 63676` showed:
   - **Crashing thread (`TID 63681`, `gdbus`):** Aborted inside `malloc()` <- `g_malloc()` <- `g_variant_new_dict_entry()` <- `g_dbus_message_new_from_blob()` when glibc detected an unaligned `tcache` freelist pointer.
   - **In-flight worker threads (`TIDs 63685`, `64846`, `66367`):** Three pooled worker threads were simultaneously executing the exact same code path in `/usr/lib/xdg-desktop-portal` (`+0xfb0c4` -> `+0xfab40` -> `+0x118450`), whose string references (`"realtime"`, `"(ttu)"`) correspond to the `org.freedesktop.portal.Realtime` (`rtkit` D-Bus proxy) handler.
   - Under concurrent failing lookups of vanishing `/proc/<pid>/task/<tid>/status` entries, heap corruption in `xdg-desktop-portal` poisoned glibc's allocator freelist until the next `g_malloc()` on the `gdbus` reader thread triggered `abort()`.

### Component Attribution

| Component | Bug / Role | Upstream |
| :--- | :--- | :--- |
| `omarchy-settings` (`mt8901-soundwir.conf`) | Sequential `If` fallthrough selects wrong `HiFi-cs42l43-cs35l56x2.conf` profile on `cs35l56x4_cs42l43` boards, breaking audio | Omarchy (`omacom/omarchy`) |
| `xdg-desktop-portal` (`1.22.1-2`) | Heap corruption (`unaligned tcache chunk`) under concurrent failing `org.freedesktop.portal.Realtime` D-Bus calls | Flatpak (`flatpak/xdg-desktop-portal`) |
| `hex-bin` (`3.3.0-1`) | Tight format-probing retry loop without backoff when no capture device exists | Anomaly (`anomalyco/hex`) |

---

## 4. User-Space Fix (`audio/`)

Because `/usr/share/alsa/ucm2/ucm.conf` resolves `conf.d/${CardDriver}/${CardLongName}.conf` (`UseCasePath.confd1`) **before** `conf.d/${CardDriver}/${CardDriver}.conf` (`UseCasePath.confd2`), we can fix the profile selection cleanly in `~/.config/` without root privileges or modifying package-owned files in `/usr/share/`:

1. [`audio/ucm2/conf.d/mt8901-soundwir/HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A.conf`](../audio/ucm2/conf.d/mt8901-soundwir/HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A.conf) matches `CardLongName` directly and loads `HiFi-hp-8e6a.conf`.
2. [`audio/ucm2/conf.d/mt8901-soundwir/HiFi-hp-8e6a.conf`](../audio/ucm2/conf.d/mt8901-soundwir/HiFi-hp-8e6a.conf) resets the 5 wrong link-1/DP7 mixer controls (`O152 I032 Switch`, `O153 I033 Switch`, `PDI13 Mux`, `O020 I180 Switch`, `O021 I181 Switch`) in case root `alsactl` ran the stock system `mt8901-soundwir.conf` at boot, then includes the stock `/MediaTek/mt8901-soundwir/HiFi-cs35l56x4-cs42l43.conf`.
3. `ALSA_CONFIG_UCM2=%h/.config/alsa/ucm2` drop-ins for `wireplumber.service`, `pipewire.service`, and `environment.d` ensure PipeWire/WirePlumber and user-session ALSA tools always use the overlay across reboots and `omarchy update` runs.

### Installation

```bash
./audio/install.sh
```

To remove the user-space override (e.g., once `omarchy-settings` fixes `mt8901-soundwir.conf` upstream):

```bash
./audio/install.sh --uninstall
```

### Upstream Fix for `mt8901-soundwir.conf`

In `/usr/share/alsa/ucm2/MediaTek/mt8901-soundwir/mt8901-soundwir.conf`, nest `If.cs42l43_cs35l56x4` inside the `False` block of `If.cs35l56x4_cs42l43`, and nest `If.cs42l43_cs35l56x2` inside the `False` block of `If.cs42l43_cs35l56x4` so that `sdw:0:1:01fa:4243:01` is only matched as a fallback when no link-0 `CS35L56` (and no 4-amp link-1 `CS35L56`) exists.

---

## 5. Verified Results

After running `./audio/install.sh`:

```text
Audio
 ├─ Devices:
 │      54. Built-in Audio                      [alsa]
 │      55. Built-in Audio                      [alsa]
 │
 ├─ Sinks:
 │      63. Built-in Audio Headphone jack       [vol: 1.00]
 │  *   64. Built-in Audio Internal speakers    [vol: 0.40]
 │
 ├─ Sources:
 │  *   65. Built-in Audio Internal microphone array [vol: 1.00]
 │
 └─ Streams:
        75. PipeWire ALSA [hex]
             76. input_FL        < Internal microphone array:capture_FL [active]
             77. input_FR        < Internal microphone array:capture_FR [active]
```

And in the journal:

```text
rtkit-daemon[1205]: Successfully made thread 94695 of process 47356 owned by '1000' RT at priority 20.
hex[47356]: INFO hex::audio: dictation microphone reopened device=Default Audio Device
```

> **Next step for Hex dictation:** Because `xdg-desktop-portal-hyprland` does not auto-bind physical keys for `GlobalShortcuts` (and `hex` only subscribes to press events on the portal), hold-to-dictate shortcuts such as `Alt+D` must be bound in `~/.config/hypr/bindings.lua` using `o.bind_hold`. See [**Hex Voice Dictation (`hex-bin`) Hold-to-Dictate Shortcut on Hyprland / Omarchy**](hex-dictation-hyprland-shortcuts.md).
