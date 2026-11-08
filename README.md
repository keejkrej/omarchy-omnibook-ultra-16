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
| **Lid Close (Panel Off)** | Working | Hardware/EC turns off panel on lid close (`\_SB.LID0._LID` ACPI query aborts via `RBUF`) |
| **Physical Battery Charging** | Working (Hardware) | Autonomous EC + charger IC manages USB-C PD negotiation and CC/CV battery charging in hardware |
| **Battery & AC Status (`BAT0`, `ADP1`)** | **Broken** | `/sys/class/power_supply/` is empty; `\_SB.BAT0._STA` and `\_SB.ADP1._PSR` abort in `\_SB.FFA0.RBUF` with `AE_AML_BUFFER_LIMIT` |
| **Thermal Zone (`TZ01`) & Fan (`FAN0`)** | **Broken** | `\_TZ.TZ01._TMP` and `\_SB.FAN0.F1LS` abort in `\_SB.FFA0.RBUF` with `AE_AML_BUFFER_LIMIT` |
| **USB-C UCSI (`USBC000:00`)** | **Broken** | `\_SB.USC1._DSM` -> `\_SB.FFA0.EC14` -> `\_SB.FFA0.RBUF` aborts (`ucsi_acpi` probe fails with `-EIO`) |
| **EC Notifications (`ARML0002:00`)** | **Broken** | `\_SB.FFA0._DSM` aborts in `\_SB.FFA0.RBUF` (`nvidia-ffa ARML0002:00: Failed to execute notify`) |

## Detailed Documentation

- [**ACPI FF-A EC (`\_SB.FFA0.RBUF`) `AE_AML_BUFFER_LIMIT` Root Cause Analysis**](docs/acpi-ffa-ec-rbuf-buffer-limit.md) — Complete diagnosis of why battery, AC adapter, thermal, fan, and USB-C UCSI ACPI methods fail on `linux-omarchy` `7.3-3.1`.
- **Upstream Issue**: [`omacom/omarchy#18112`](https://github.com/omacom/omarchy/issues/18112)
