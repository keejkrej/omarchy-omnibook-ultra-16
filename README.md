# HP OmniBook Ultra 16-bk0xxx (NVIDIA N1X) on Omarchy Linux

Hardware documentation, kernel diagnostics, and issue tracking for running **Omarchy Linux (`aarch64`)** on the **HP OmniBook Ultra Laptop 16-bk0xxx** powered by the **NVIDIA N1X (`GB20B` / RTX Spark N1X)** platform.

## Hardware Overview

| Component | Details |
| :--- | :--- |
| **System Product** | `HP OmniBook Ultra Laptop 16-bk0xxx` |
| **Product SKU** | `B84X7UA#ABA` |
| **Motherboard / Board ID** | HP `8E6A` (rev `56.68`, part `E55S6EA#ABD`) |
| **BIOS / UEFI** | AMI `F.06` (`09/25/2025` / `09/29/2026`), EC Firmware `6.19` |
| **SoC / CPU** | ARMv8 `aarch64` 20-core (2 clusters, `cppc_cpufreq`) |
| **GPU** | NVIDIA `GB20B` \[RTX Spark N1X\] (`10de:2e03`), driver `nvidia 615.78.08` |
| **Display** | 15.9" Samsung `0x4257` OLED `2880x1800` (`eDP-1`) |
| **Wi-Fi / Bluetooth** | MediaTek MT7925 Filogic 360 Wi-Fi 7 (`14c3:7925`, `mt7925e`) + BT 5.4 (`0e8d:8c38`, `btusb`) |
| **Audio** | SoundWire (`mt8901-soundwire`) with Cirrus Logic `cs42l43` + 4x `cs35l56` amplifiers |
| **Storage** | 1 TB KIOXIA NVMe SSD (`HP01A390`) |
| **OS / Kernel Tested** | Omarchy `4.5.0-3`, `linux-omarchy 7.3-3.1` (`7.3.0-3.1-omarchy`) |

## Subsystem Status Matrix

| Subsystem | Status | Notes |
| :--- | :--- | :--- |
| **Display / GPU (`nvidia`)** | Working | `2880x1800` eDP-1 active on Hyprland via `nvidia-open-dkms 615.78.08`; NBCI `_DSM` subfunction `0x10` fails due to `\_SB.FFA0.RBUF` |
| **Wi-Fi 7 & Bluetooth** | Working | MediaTek `mt7925e` and `btusb` work out of the box |
| **NVMe Storage** | Working | PCIe Gen4 x4 NVMe with LUKS + Btrfs |
| **Camera** | Working | HP 5MP USB Camera (`30c9:00fb`, `uvcvideo`) |
| **Audio (`mt8901-soundwire`)** | **Working (Fixed via [`audio/`](audio/))** | User-space ALSA UCM2 override works around `If` fallthrough in `mt8901-soundwir.conf` so `HiFi-cs35l56x4-cs42l43.conf` activates quad speakers, 3.5mm jack, and internal DMICs (`cs35l56` runs uncalibrated DSP1 ROM `v3.4.4` pending HP tuning `.bin` files) |
| **Lid Close (Panel Off & `_LID`)** | **Working (Fixed via [`kmod/`](kmod/))** | Hardware panel off works natively; `_LID` state (`open`/`closed`) works with `omnibook_ffa_fix` |
| **Physical Battery Charging** | Working (Hardware) | Autonomous EC + charger IC manages USB-C PD negotiation and CC/CV battery charging in hardware |
| **Battery & AC Status (`BAT0`, `ADP1`)** | **Working (Fixed via [`kmod/`](kmod/))** | `omnibook_ffa_fix` patches `\_SB.FFA0.RBUF` and redirects `\_SB.BAT0._BST` -> `UPBS()`; exposes `BAT0` (99.3 Wh) and `ADP1` to `upower` and `omarchy-shell` |
| **Thermal Zone (`TZ01`) & Fan (`FAN0`)** | **Working (Fixed via [`kmod/`](kmod/))** | `TZ01` registers as `thermal_zone1` (`acpitz`) and `FAN0` reports RPM once `\_SB.FFA0.RBUF` is patched |
| **USB-C UCSI (`USBC000:00`)** | **Working (Fixed via [`kmod/`](kmod/))** | `ucsi_acpi` binds `typec port0` & `port1` (`usb4_port1`, `ucsi-source-psy-USBC000:001`/`002`) once `\_SB.FFA0.RBUF` is patched |
| **EC Notifications (`ARML0002:00`)** | **Working (Fixed via [`kmod/`](kmod/))** | `\_SB.FFA0._DSM` EC event notifications succeed once `\_SB.FFA0.RBUF` is patched |

## Detailed Documentation & Fixes

- [**`audio/install.sh`**](audio/install.sh) ([`audio/ucm2/`](audio/ucm2/)) — User-space ALSA UCM2 override (`ALSA_CONFIG_UCM2`) for `HP-HPOmniBookUltraLaptop16_bk0xxx-8E6A` that selects `HiFi-cs35l56x4-cs42l43.conf` and clears stale boot `alsactl` switches, restoring speakers, headphone jack, and internal microphone array.
- [**MT8901 SoundWire Audio UCM Dispatch Bug & `xdg-desktop-portal` `SIGABRT` Cascade**](docs/mt8901-soundwire-audio-and-portal-crash.md) — Root-cause analysis of why `mt8901-soundwir.conf` dropped the `HiFi` profile on HP board `8E6A` and how `hex` polling a missing microphone triggered heap-corruption `SIGABRT` crashes in `xdg-desktop-portal`.
- [**`kmod/omnibook_ffa_fix.c`**](kmod/omnibook_ffa_fix.c) ([`kmod/install.sh`](kmod/install.sh)) — Out-of-tree kernel module (with DKMS support) that patches `\_SB.FFA0.RBUF` (`CreateField` bit length `0x0190 -> 0x0130`) and `\_SB.BAT0._BST` (`UPBS () Return (PBST)`) in-memory and re-enumerates `BAT0`, `ADP1`, `TZ01`, `USC1`, and `FAN0` without rebooting.
- [**ACPI FF-A EC (`\_SB.FFA0.RBUF`) `AE_AML_BUFFER_LIMIT` Root Cause Analysis**](docs/acpi-ffa-ec-rbuf-buffer-limit.md) — Complete diagnosis of why battery, AC adapter, thermal, fan, and USB-C UCSI ACPI methods fail on `linux-omarchy` `7.3-3.1`.
- **Upstream Issue**: [`omacom/omarchy#18112`](https://github.com/omacom/omarchy/issues/18112)
