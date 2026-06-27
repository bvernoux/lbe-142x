<!--
SPDX-License-Identifier: MIT
Copyright (c) 2024-2026 Benjamin Vernoux
-->
# LBE-1425 config protocol (bcdDevice 1.10) — USB capture evidence

Reverse-engineered from a `usbmon` capture of the vendor Windows tool driving
an LBE-1425 (PID `0x2269`), 2026-06-26, plus live cross-checking on the device.

## Transport

Composite CDC+HID (IAD). All config goes over **HID Feature reports on
interface 2** (`wIndex == 0x0002`), 60-byte payload (`wLength == 60`,
== `LBE_REPORT_SIZE`):

- write: `SET_REPORT` — `bmRequestType 0x21`, `bRequest 0x09`, `wValue 0x0300`
  (Feature, **report-ID 0**)
- read:  `GET_REPORT` — `bmRequestType 0xA1`, `bRequest 0x01`, `wValue 0x0300`

The **command opcode is payload byte 0**; the firmware reads it from the data,
not from the HID report-ID. (This tool's 1421 path sends the opcode in *both*
the report-ID and byte 0, so it interoperates fine.)

GPS NMEA is on the CDC ACM data interface; the vendor sets it to 9600 8N1
(`SET_LINE_CODING 80 25 00 00 00 00 08`).

## Command opcodes (payload byte 0)

Confirmed identical to the LBE-1421 (`include/lbe_common.h`):

| opcode | command   | payload                              | observed            |
|--------|-----------|--------------------------------------|---------------------|
| `0x01` | EN_OUT    | `[1]` = 0x00 off / 0x03 both on      | off, on             |
| `0x06` | SET_F1    | u32 LE freq at **byte 5**            | 10 000 000 Hz       |
| `0x0A` | SET_F2    | u32 LE freq at byte 5                | 27 000 000 Hz       |
| `0x0B` | SET_PLL   | `[1]` = 0 PLL / 1 FLL                | both                |
| `0x0C` | SET_PPS   | `[1]` = 0 off / 1 on                 | both (see note)     |
| `0x0D` | SET_PWR1  | `[1]` = 0 normal / 1 low             | both                |
| `0x0E` | SET_PWR2  | `[1]` = 0 normal / 1 low             | both                |

LBE-1425-specific (not in the 1421 map), identified by live USB capture:

| opcode | command          | payload `[1]`                | status |
|--------|------------------|------------------------------|--------|
| `0x0F` | SET_NMEA (NMEA out enable) | 0 off / 1 on        | **confirmed** |
| `0x03` | SET_GNSS (constellation mask) | bitmask, see below | **confirmed** |
| `0x04` | SET_DYNMODEL (u-blox CFG-NAV5 dynModel) | u-blox value | **confirmed** |
| `0x08` | telemetry poll (GUI), sel byte at `[6]` | `{07,22,35}` | read channel |

### `0x04` SET_DYNMODEL (payload byte 1 = u-blox dynModel)

The value is the u-blox `CFG-NAV5` dynamic platform model, passed through verbatim.
The vendor UI exposes three; the field accepts the full u-blox enum:

| value | model        | in UI |
|-------|--------------|-------|
| 0     | Portable     | yes   |
| 2     | Stationary   | yes   |
| 8     | Airborne <4g | yes (as "Airborne") |

(other u-blox values: 3 Pedestrian, 4 Automotive, 5 Sea, 6 Airborne<1g, 7 Airborne<2g)

### `0x03` SET_GNSS bitmask (payload byte 1)

The mask is `bit = 1 << u-blox gnssId`:

| bit | mask | constellation |
|-----|------|---------------|
| 0   | 0x01 | GPS           |
| 1   | 0x02 | SBAS          |
| 2   | 0x04 | Galileo       |
| 3   | 0x08 | BeiDou        |
| 4   | 0x10 | IMES          |
| 5   | 0x20 | QZSS          |
| 6   | 0x40 | GLONASS       |

GPS/SBAS/Gal/BeiDou/GLONASS decoded from the vendor UI sweep; **QZSS (0x20)
confirmed** by setting `--gnss 0x67` vs `0x47` and reading back UBX-CFG-GNSS
(QZSS appears/disappears accordingly) — even though the vendor UI has no QZSS
control. IMES (0x10) follows the same `1<<gnssId` mapping (not separately
exercised). e.g. `0x47` = GPS+SBAS+Galileo+GLONASS; `0x67` adds QZSS.

**Valid-mask constraint** (u-blox concurrent-GNSS rule, enforced by the UI/firmware):
the GPS/SBAS/Galileo group (`0x01|0x02|0x04`) is **mutually exclusive** with
BeiDou (`0x08`) — you get one group or the other, not both. GLONASS (`0x40`) may
be combined with either, with no restriction. A `--gnss` CLI flag should reject
masks that set BeiDou together with any of GPS/SBAS/Galileo.

**Opcode space mapped.** Every documented opcode `0x01`-`0x0F` is accounted for
above. The LBE-1420-only `0x07` (SET_PWR1) is **ignored** on the 1425 (no
status change for either arg). A sweep of `0x10`-`0x2F` (send each opcode, diff
the full status report) produced **no status change** for any -- so there are no
undocumented status-affecting opcodes. (Caveat: this only observes status-echoed
effects; a purely physical/GPS-side opcode would not show, but the one such
channel, the `0x08` UBX wrap, is already known.)

`0x08` is issued repeatedly by the GUI (3 distinct sub-selectors cycling) and is
almost certainly how the vendor app reads the extended "increased stability"
telemetry. The `[6]` byte (0x07/0x22/0x35) likely selects what to read.

Note (SET_PPS): enabling PPS in the vendor UI forces OUT1 into PPS-only mode and
shows a 10 MHz default, but the **only** USB command emitted is `0x0C 01` (and
`0x0C 00` to disable) -- the OUT1 reconfiguration is firmware-internal or
UI-side, not a separate command. This tool's `--pps` already sends exactly that,
so it fully replicates the vendor behaviour. (Confirmed by live USB capture.)

## Status read (GET_REPORT, report-ID 0, 60 bytes)

Same layout as the 1421 status report for the documented fields:

| offset | field           | value seen      |
|--------|-----------------|-----------------|
| 0      | report-id echo  | `0x01`          |
| 1      | status bits     | `0x7F`/`0xF7`   |
| 6      | OUT1 freq u32LE | 10 000 000      |
| 14     | OUT2 freq u32LE | 27 000 000      |
| 18     | FLL mode        | 0               |
| 19     | OUT1 power low  | 0               |
| 20     | OUT2 power low  | 0               |
| 21     | GNSS mask       | tracks `--gnss` |
| 22     | dynModel        | `0x02` Stationary |
| 23     | antenna current | mA (see below)  |
| 24     | NMEA output enable | `0x00`/`0x01` |
| 25..   | padding         | `FF…`           |

Status bits @1 match `lbe_common.h` (GPS/PLL/ANT/LED/OUT1/OUT2/PPS). The tail
bytes (21-24), once thought to be opaque telemetry, decode by watching them
change live as config and antenna state vary:

- **byte 21 = GNSS constellation mask** — directly confirmed by sweeping
  `--gnss` (0x01→GPS, 0x40→GLONASS, 0x21→GPS+QZSS, 0x47→…), reads back exactly.
- **byte 22 = u-blox dynamic model** — directly confirmed by sweeping
  `--dynmodel` (0=Portable, 2=Stationary, 3=Pedestrian, 4=Automotive, 8=Airborne).
  Both are shown in `--status` (`GNSS:` / `Dynamic model:` lines).
- **byte 23 = measured antenna bias current (mA).** 0 with no antenna; ~5 mA on
  one patch, ~11–12 mA (jittering ±1, ADC noise) on another. This gives true
  antenna-presence detection the single ANT *short* bit can't: 0 = open/none,
  nonzero = connected, short = ANT bit clears. Surfaced in `--status` as
  `Antenna: Not connected (0 mA)` / `OK (N mA)` / `Short Circuit`.
- **byte 24 = NMEA output enable** — confirmed by toggling `--nmea` (`0`→`0x00`,
  `1`→`0x01`). The `0x00` in the original vendor capture was just NMEA off.

The full tail is decoded: it's a live config + status echo {GNSS mask, dynModel,
antenna current, NMEA enable}, not the opaque telemetry first assumed. `--status`
surfaces all of it on the 1425.

## Diagnostics channel: UBX over EP 0x83 (confirmed)

The HID interrupt-IN endpoint **EP 0x83** carries a **u-blox UBX binary stream** —
the vendor GUI's "diagnostics" view (including the live per-satellite CNR
histogram). It is HID-framed: each 64-byte interrupt transfer is
`[seq byte][0x3E = 62][62 bytes of payload]`; concatenating the 62-byte payloads
reassembles a continuous UBX stream. De-framed, a sample capture yields 251
valid UBX messages with **zero checksum failures**:

| UBX msg   | class/id | carries |
|-----------|----------|---------|
| NAV-CLOCK | 01 22    | clock bias (ns), drift (ns/s), time/freq accuracy — the disciplining/stability telemetry |
| NAV-SAT   | 01 35    | per-SV CNR — the live histogram in the UI |
| NAV-PVT   | 01 07    | position / velocity / time / fix |
| ACK-ACK   | 05 01    | command acknowledgements |

This is the same UBX content the LBE-Mini streams over HID, so the existing UBX
parser (`src/model_mini.c`) and CNR renderer (`src/gnss_view.c`) can be reused
for a 1425 diagnostics monitor after stripping the 2-byte `[seq][len]` frame
header.

The `0x08` writes (`08 06 01 08 00 01 <sel> 0A`) are CFG-MSG wraps: the `<sel>`
byte is the u-blox NAV message id to enable — `0x07`=NAV-PVT, `0x22`=NAV-CLOCK,
`0x35`=NAV-SAT, i.e. exactly the three NAV messages seen in the stream. The
device forwards these to the u-blox as `CFG-MSG`/`CFG-GNSS`/`CFG-NAV5`/`CFG-TP5`
(seen as the ACK-ACK payloads 06-01 / 06-3E / 06-24 / 06-31), which also
confirms `--gnss`→CFG-GNSS and `--dynmodel`→CFG-NAV5 from the GPS side.

**Stream fully characterized:** of the EP 0x83 payload bytes, 77% are valid UBX
(NAV-PVT/SAT/CLOCK + ACK-ACK, all decoded) and ~22% are `0xFF` inter-message
padding (correctly skipped on resync); the remainder is a negligible
frame-boundary fragment. No hidden or unhandled message types.

## GNSS module (UBX-MON-VER, via --gps-info)

The 1425 forwards UBX polls/`CFG-MSG` to its GNSS module, so `--gps-info` reads:

```
SW: ROM CORE 3.01 (107888)   HW: 00080000   FWVER=SPG 3.01   PROTVER=18.00
GPS;GLO;GAL;BDS  SBAS;IMES;QZSS
```

→ a **u-blox M8** (HW `0x00080000`), ROM-based (NEO-M8N/M8M-class), Standard
Precision GNSS firmware (not the M8T timing variant), protocol 18.00. This
explains the GNSS behaviour: the BeiDou-vs-GPS/SBAS/Galileo exclusion is the M8
3-concurrent-GNSS limitation, and NAV-SAT / 92-byte NAV-PVT are protocol-15+.

**Antenna status:** `UBX-MON-HW` reports `antStatus = DONTKNOW` — the board does
not wire the antenna to the u-blox supervisor, so MON-HW is a dead end. But the
LBE MCU measures the antenna **bias current** itself and reports it in the status
feature report at **byte 23** (mA) -- see the status-read section. That gives
real presence detection (0 mA = open/none, nonzero = connected, over-current
short clears the ANT bit), which is what `--status` now uses. So accurate
antenna state *is* obtainable in software after all -- just from the LBE report,
not from the u-blox.

## Factory reset

The vendor "factory reset" button is a **client-side macro** — it emits a
sequence of already-known commands, no dedicated opcode. It reveals the factory
defaults: GNSS `0x47` (GPS+SBAS+Galileo+GLONASS), dynModel `0x02` (Stationary),
OUT1=OUT2=10 MHz, PLL mode, PPS off, NMEA off, both outputs normal power.

## Status / TODO

- Core 1421 command + status protocol: **confirmed**, works in this tool.
- `0x03` SET_GNSS, `0x04` SET_DYNMODEL, `0x0F` SET_NMEA: **confirmed and
  implemented** (`--gnss` / `--dynmodel` / `--nmea`).
- `0x08` poll + EP 0x83 stream: **fully decoded, implemented, confirmed on
  hardware** as `--diag` (NAV-SAT CNR histogram + NAV-CLOCK disciplining line;
  live: 3D fix, 14 SVs, bias ~-0.5 ms, drift -12 ns/s, tAcc 4 ns, fAcc 244 ps/s).
  The `0x08` selector = NAV message id (PVT/CLOCK/SAT); stream free-runs anyway.
- Status tail bytes 21-24 (`67 02 05 00`): **constant** across the capture, so
  not dynamic telemetry — a fixed firmware/build/capability field. Cosmetic;
  not surfaced. The live disciplining telemetry lives in NAV-CLOCK instead.

No functional open items remain in the USB stream or diagnostics: every control
request is either a decoded config command or standard USB/CDC/HID housekeeping,
and the EP 0x83 stream is 100% accounted for (UBX + padding).
