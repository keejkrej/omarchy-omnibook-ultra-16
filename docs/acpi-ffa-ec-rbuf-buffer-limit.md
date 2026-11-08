# ACPI FF-A EC (`\_SB.FFA0.RBUF`) `AE_AML_BUFFER_LIMIT` Root Cause Analysis

## Summary

On the **HP OmniBook Ultra Laptop 16-bk0xxx** (NVIDIA N1X / `GB20B`, board `8E6A`, BIOS `F.06`) running Omarchy `4.5.0-3` (`linux-omarchy 7.3-3.1`, kernel `7.3.0-3.1-omarchy`), the battery icon and charging status are completely absent from `omarchy-shell` because `/sys/class/power_supply/` is empty.

Every ACPI Embedded Controller (EC) transaction routed through the ARM FF-A EC bridge (`\_SB.FFA0`, `_HID` `ARML0002`) aborts inside the AML helper method `\_SB.FFA0.RBUF` with:

```text
ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
```

Because `\_SB.FFA0.RBUF` is the common AML helper used by all EC-backed ACPI devices on this laptop, its failure cascades across **9 distinct ACPI subsystems**.

---

## 1. User-Space to Kernel Symptom Chain

1. **Omarchy Status Bar (`omarchy-shell`)**:
   - `~/.config/omarchy/shell.json` enables the `"power"` bar item (`PowerBarItem.qml` / `Panel.qml`).
   - `/usr/share/omarchy/shell/plugins/panels/power/Model.js` polls `/usr/bin/omarchy-battery-status` (which queries `upower -e` for `/battery_` devices) and `UPower.displayDevice.isLaptopBattery`.
   - When `isLaptopBattery` is `false` and `omarchy-battery-status` returns `{}` (empty JSON), `PowerBarItem.qml` hides the battery percentage and charging indicator, showing only the static power-profile icon.
2. **UPower (`upower -d`)**:
   - Reports no `battery_*` or `line_power_*` devices and `on-battery: no`.
3. **Linux Power Supply Class (`/sys/class/power_supply/`)**:
   - Directory is completely empty.
4. **ACPI Device Nodes (`/sys/bus/acpi/devices/`)**:
   - `PNP0C0A:00` (`\_SB.BAT0`, Control Method Battery): exists in the ACPI namespace, but reading `/sys/bus/acpi/devices/PNP0C0A:00/status` returns `Input/output error` (`-EIO`) because `\_SB.BAT0._STA` aborts. Without a valid `_STA` indicating battery presence (`0x1f`), `drivers/acpi/battery.c` never binds.
   - `ACPI0003:00` (`\_SB.ADP1`, AC Adapter): exists in the ACPI namespace, but `\_SB.ADP1._PSR` aborts during probe (`ACPI: \_SB_.ADP1: Error reading AC Adapter state: AE_AML_BUFFER_LIMIT`), so `drivers/acpi/ac.c` fails to register a power supply device.
   - `ARML0002:00` (`\_SB.FFA0`, ARM FF-A EC bridge): bound to the `nvidia-ffa` driver (`drivers/platform/arm64/nvidia-ffa-ec.c` from `1010-n1x-ffa-ec.patch`).

---

## 2. Exact Root Cause in `\_SB.FFA0.RBUF`

### How EC Communication Works on NVIDIA N1X (`ARML0002` / `MSFT000C`)

On NVIDIA N1X / GB10 platforms, the OS does not talk to the Embedded Controller over legacy I/O ports (`0x62`/`0x66`). Instead, ACPI AML methods communicate with EC services running in an ARM TrustZone Secure Partition (SP) via **ARM FF-A (`FFA_MSG_SEND_DIRECT_REQ2`)** over an `FFixedHW` (`0x7F`) OperationRegion on `\_SB.FFA0`:

- `linux-omarchy` includes `1010-n1x-ffa-ec.patch` (`drivers/platform/arm64/nvidia-ffa-ec.c` and `drivers/acpi/arm64/ffh.c`), which supports two revisions of the OpenDevicePartnership / ARM DEN0077A EC interface specification:
  1. **Draft Spec (`_HID` `MSFT000C`, FFH offset `4`)**:
     - Request packet header (`FFA_REQ_PACKET`): `status` (1B) + `length` (1B) + `uuid` (16B) = **18 bytes (144 bits)** before `reqdata[]`.
     - Response packet header in draft spec / `ectest.asl` (`FFA_RSP_PACKET`): `status` (1B, bit 0) + `length` (1B, bit 8) + `uuid` (16B, bit 16..143) + `ffa_status` (8B, bit 144..207) = **26 bytes (208 bits)** before `rspdata[]` (`DATA`).
  2. **DEN0077A v1.3 ALP5 Spec (`_HID` `ARML0002`, FFH offset `2`)**:
     - Packet header (`FFA_REQ_PACKET` / `FFA_RSP_PACKET`): `status` (8B, bit 0..63) + `recvid` (8B, bit 64..127) + `uuid` (16B, bit 128..255) = **32 bytes (256 bits)** before `rawdata[]`.

### Decompiled `DSDT.dsl` Analysis (BIOS `F.06`)

Decompiling `/sys/firmware/acpi/tables/DSDT` (`iasl -d DSDT`) reveals **two separate ACPI DSDT bugs** in HP BIOS `F.06`:

#### Bug 1: Unused Out-of-Bounds `DATA` Field in `\_SB.FFA0.RBUF` (`DSDT.dsl` lines 17395–17441)

`\_SB.FFA0` uses standard `ARML0002` (`OperationRegion (AFFH, FFixedHW, 0x02, 0x90)`) with a 32-byte header (`BFX0` at `0x00`, `BFX1` at `0x08`, `BX23` UUID at `0x10`, payload starting at byte `0x20`). However, `Method (RBUF)` contains an unused leftover `CreateField (BUFF, 0xD0, 0x0190, DATA)` definition:

```asl
Method (RBUF, 4, Serialized)
{
    If ((AVAL () == One))
    {
        ERST ()
        Local0 = Acquire (ECMU, 0xFFFF)
        Name (BUFF, Buffer (0x40){})
        CreateQWordField (BUFF, Zero, BFX0)
        CreateQWordField (BUFF, 0x08, BFX1)
        CreateField (BUFF, 0x80, 0x80, BX23)
        CreateByteField (BUFF, 0x20, CMDD)
        CreateDWordField (BUFF, 0x21, ADDR)
        CreateByteField (BUFF, 0x25, DLEN)
        CreateField (BUFF, 0xD0, 0x0190, DATA) // <-- BUG: 208 + 400 = 608 bits > 512 bits (0x40 bytes), and DATA is never referenced!
        If ((Local0 == Zero))
        {
            Local1 = Zero
            While (One)
            {
                If ((Local1 == 0x02))
                {
                    Release (ECMU)
                    Break
                }

                BFX0 = Zero
                BFX1 = Zero
                BX23 = Arg0
                CMDD = Arg1
                ADDR = Arg2
                DLEN = Arg3
                BUFF = FFAC = BUFF /* \_SB_.FFA0.RBUF.BUFF */
                If ((BFX0 == Zero))
                {
                    Release (ECMU)
                    Break
                }

                Sleep (One)
                Local1 += One
            }
        }

        Return (BUFF) /* \_SB_.FFA0.RBUF.BUFF */
    }

    Return (Zero)
}
```

- `DATA` is **never read or written** inside `RBUF`; callers receive `Return (BUFF)` and extract response bytes at offset `0x20` (`CreateByteField (BUFF, 0x20, ...)`).
- Because `0xD0 + 0x0190 = 608 bits > 512 bits`, ACPICA (`acpi_ds_init_buffer_field`) aborts `RBUF` with `AE_AML_BUFFER_LIMIT` before the FF-A transaction (`BUFF = FFAC = BUFF`) ever executes.

#### Bug 2: `\_SB.BAT0._BST` Calls NVIDIA Reference `SREQ` Instead of HP's `UPBS()` (`DSDT.dsl` lines 19979–20301)

In `Device (BAT0)`:
- `_BIF` calls `UPBI()` and `_BIX` calls `UPBX()`, both of which read battery static info from HP's EC partition (`884a63a0-3285-4120-83aa-eec008a0a546`, bound to `arm-ffa-17`) via `^^FFA0.RBUF`.
- Right below `UPBI()` is `Method (UPBS, 0, NotSerialized)` (lines 20213–20301), which reads live battery current (`0x06000383`), remaining capacity (`0x06000385`), voltage (`0x06000387`), and charging state (`0x06000382`) via `^^FFA0.RBUF` on `884a63a0-3285-4120-83aa-eec008a0a546` and populates `PBST[0..3]`.
- However, `Method (_BST)` (lines 19979–19998) was left with NVIDIA's reference `SREQ(Local0, HPBM)` call targeting UUID `25cb5207-ac36-427d-aaef-3aa78877d27e` (which does not exist on the HP OmniBook Ultra 16) and **never calls `UPBS()`**:

```asl
Method (_BST, 0, NotSerialized)  // _BST: Battery Status
{
    Local0 = Buffer (0x08){}
    CreateByteField (Local0, Zero, CMDD)
    CMDD = 0x02
    Local1 = SREQ (Local0, HPBM) // <-- Fails: UUID 25cb5207-ac36-427d-aaef-3aa78877d27e does not exist on HP
    If ((DerefOf (Local1 [Zero]) == Zero))
    {
        ...
    }

    Return (PBST) /* \_SB_.BAT0.PBST */
}
```

---

## 3. All Affected ACPI Methods & Subsystems

Every ACPI method below calls `\_SB.FFA0.RBUF` and aborts with `AE_AML_BUFFER_LIMIT`:

| Subsystem | ACPI Device (`_HID`) | Failing Method Call Chain | Kernel Driver / Error |
| :--- | :--- | :--- | :--- |
| **Battery** | `\_SB.BAT0` (`PNP0C0A:00`) | `\_SB.BAT0._STA` -> `\_SB.FFA0.RBUF` | `ACPI Error: Method execution failed \_SB.BAT0._STA` (`/sys/class/power_supply/BAT0` missing) |
| **AC Adapter** | `\_SB.ADP1` (`ACPI0003:00`) | `\_SB.ADP1._PSR` -> `\_SB.FFA0.RBUF` | `ACPI: \_SB_.ADP1: Error reading AC Adapter state: AE_AML_BUFFER_LIMIT` |
| **Thermal Zone** | `\_TZ.TZ01` (`LNXTHERM:00`) | `\_TZ.TZ01._TMP` -> `\_SB.ECT0.THEM` -> `\_SB.FFA0.RBUF` | `acpi-thermal LNXTHERM:00: probe with driver acpi-thermal failed with error -19` |
| **Fan** | `\_SB.FAN0` (`PNP0C0B:00`) | `\_SB.FAN0._FST` -> `\_SB.FAN0.F1LS` -> `\_SB.FFA0.RBUF` | `ACPI: fan PNP0C0B:00: Cannot read fan status` |
| **USB-C UCSI** | `\_SB.USC1` (`USBC000:00`) | `\_SB.USC1._DSM` -> `\_SB.FFA0.EC14` -> `\_SB.FFA0.RBUF` | `ucsi_acpi USBC000:00: probe with driver ucsi_acpi failed with error -5` |
| **GPU NBCI** | `\_SB.PCIF.RP0.IGPU` | `\_SB.PCIF.RP0.IGPU._DSM` -> `NBCI` -> `\_SB.FFA0.RBUF` | `NVRM: osCallACPI_DSM: Error during 0x3 DSM subfunction 0x10! status=0x59` |
| **EC Notify** | `\_SB.FFA0` (`ARML0002:00`) | `\_SB.FFA0._DSM` -> `\_SB.ECT0.PGED` -> `\_SB.FFA0.RBUF` | `nvidia-ffa ARML0002:00: Failed to execute notify` |
| **Lid Switch** | `\_SB.LID0` (`PNP0C0D:00`) | `\_SB.LID0._LID` -> `\_SB.FFA0.RBUF` | `Aborting method \_SB.LID0._LID` (hardware panel off still works) |
| **ACPI RTC** | `\_SB.RTC` (`ACPI000E:00`) | `\_SB.RTC._GRT` -> `\_SB.FFA0.RBUF` | `Aborting method \_SB.RTC._GRT` |

---

## 4. Does This Impact Physical Battery Charging?

- **Physical charging works normally**: USB-C Power Delivery negotiation and Constant-Current / Constant-Voltage (CC/CV) battery charging are controlled autonomously in hardware/firmware by the USB-C PD controller, the Embedded Controller (EC), and the dedicated battery charger IC, using hardware battery pack thermistors and voltage/current sense resistors.
- **What is impacted in Linux**:
  - No battery percentage or charging status in `omarchy-shell` / `upower`.
  - No OS-initiated critical-battery hibernate/shutdown (the hardware BMS/EC still enforces a hard low-voltage cutoff).
  - No OS-level charge threshold control or USB-C UCSI port status reporting via `ucsi_acpi`.
  - EC thermal zone (`TZ01`) and fan telemetry (`FAN0`) are unavailable to Linux (`nvidia-ffa-ec` notifications also fail when fan speed changes).

---

## 5. Kernel Log Excerpts (`journalctl -b -k`)

```text
Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.BAT0._STA due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Method execution failed \_SB.BAT0._STA due to previous error (AE_AML_BUFFER_LIMIT) (20260408/uteval-68)

Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.ADP1._PSR due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI: \_SB_.ADP1: Error reading AC Adapter state: AE_AML_BUFFER_LIMIT

Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.ECT0.THEM due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_TZ.TZ01._TMP due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI: thermal: [Firmware Bug]: No valid trip points!
Nov 08 03:44:44 rtxspark kernel: acpi-thermal LNXTHERM:00: probe with driver acpi-thermal failed with error -19

Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FAN0.F1LS due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FAN0._FST due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI: fan PNP0C0B:00: Cannot read fan status

Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.EC14 due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.USC1._DSM due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI: \_SB_.USC1: failed to evaluate _DSM c298836f-a47c-e411-ad36-631042b5008f rev:1 func:2 (0x300a)
Nov 08 03:44:44 rtxspark kernel: ucsi_acpi USBC000:00: ucsi_acpi_dsm: failed to evaluate _DSM 2
Nov 08 03:44:44 rtxspark kernel: ucsi_acpi USBC000:00: probe with driver ucsi_acpi failed with error -5

Nov 08 03:44:44 rtxspark kernel: ACPI BIOS Error (bug): AE_AML_BUFFER_LIMIT, Field [DATA] at bit offset/length 208/400 exceeds size of target Buffer (512 bits) (20260408/dsopcode-198)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0.RBUF due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.ECT0.PGED due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI Error: Aborting method \_SB.FFA0._DSM due to previous error (AE_AML_BUFFER_LIMIT) (20260408/psparse-543)
Nov 08 03:44:44 rtxspark kernel: ACPI: \_SB_.FFA0: failed to evaluate _DSM 1e548176-2788-3942-8d9d-36be7fe12542 rev:1 func:1 (0x300a)
Nov 08 03:44:44 rtxspark kernel: nvidia-ffa ARML0002:00: Failed to execute notify
```

---

## 6. Live Kernel Module Fix (`kmod/omnibook_ffa_fix.c`)

The [`kmod/`](../kmod/) directory contains an out-of-tree kernel module (`omnibook_ffa_fix.ko`, with DKMS support) that fixes both DSDT bugs at runtime without requiring a custom kernel build or reboot:

1. **Clamps `\_SB.FFA0.RBUF`'s `CreateField` bit length**:
   - Locates `\_SB.FFA0.RBUF`'s `union acpi_operand_object` (`ACPI_TYPE_METHOD`), duplicates its AML bytecode, patches `CreateField (BUFF, 0xD0, 0x0190, DATA)` (`5b 13 42 55 46 46 0a d0 0b 90 01 44 41 54 41`) from `0x0190` (`400` bits) to `0x0130` (`304` bits, fitting cleanly inside the 512-bit `Buffer (0x40)`), and updates `method.aml_start`.
2. **Redirects `\_SB.BAT0._BST` to `UPBS()`**:
   - Replaces `\_SB.BAT0._BST`'s AML body with `55 50 42 53 a4 50 42 53 54` (`UPBS () Return (PBST)`), bypassing the broken `SREQ` call to non-existent UUID `25cb5207-ac36-427d-aaef-3aa78877d27e`.
3. **Re-enumerates and attaches EC-dependent ACPI devices**:
   - Evaluates `\_SB.BAT0._STA` via `acpi_bus_get_status()`, creates the `PNP0C0A:00` platform device via `acpi_create_platform_device()`, and triggers `device_attach()` on `PNP0C0A:00` (`BAT0`), `ACPI0003:00` (`ADP1`), `LNXTHERM:00` (`TZ01`), `USBC000:00` (`USC1`), and `PNP0C0B:00` (`FAN0`).

### Build & Install

```bash
cd kmod
./install.sh
```
