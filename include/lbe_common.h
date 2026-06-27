/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_COMMON_H
#define LBE_COMMON_H

// Device definitions
#define VID_LBE 0x1dd2
#define PID_LBE_1420 0x2443
#define PID_LBE_1421 0x2444 // LBE-1421 Dual Output
#define PID_LBE_1423 0x226f // LBE-1423 differential pps
#define PID_LBE_1425 0x2269 // LBE-1425 increased-stability dual output (CDC+HID composite)
#define PID_LBE_MINI 0x2211 // Mini Precision GPS Reference Clock

/* Device status bits */
#define LBE_GPS_LOCK_BIT  (1 << 0)
#define LBE_PLL_LOCK_BIT  (1 << 1)
#define LBE_ANT_OK_BIT    (1 << 2)
#define LBE_LED1_BIT      (1 << 3)
#define LBE_LED2_BIT      (1 << 4)
#define LBE_OUT1_EN_BIT   (1 << 5)
#define LBE_OUT2_EN_BIT   (1 << 6)
#define LBE_PPS_EN_BIT    (1 << 7)

/* Command codes */
#define LBE_142X_EN_OUT      0x01
#define LBE_142X_BLINK_OUT   0x02
#define LBE_1421_SET_F1_TEMP 0x05
#define LBE_1421_SET_F1      0x06
#define LBE_1421_SET_F2_TEMP 0x09
#define LBE_1421_SET_F2      0x0A
#define LBE_142X_SET_PLL     0x0B
#define LBE_1421_SET_PPS     0x0C
#define LBE_1421_SET_PWR1    0x0D
#define LBE_1421_SET_PWR2    0x0E

/* Compatibility for LBE-1420 */
#define LBE_1420_SET_F1_TEMP 0x03
#define LBE_1420_SET_F1      0x04
#define LBE_1420_SET_PWR1    0x07

/* LBE-1425-specific opcodes (reverse-engineered from the vendor tool; see
 * docs/reverse/LBE-1425-config-v1.10.md). They reuse the same SET_REPORT
 * feature-report transport as the 1421 (opcode in payload byte 0) and collide
 * numerically with the 1420 freq opcodes above, so they are only ever issued
 * to a 1425 (via lbe_ops_1425). Arg is payload byte 1. */
#define LBE_1425_SET_GNSS     0x03  /* GNSS constellation enable bitmask */
#define LBE_1425_SET_DYNMODEL 0x04  /* u-blox CFG-NAV5 dynamic platform model */
#define LBE_1425_SET_NMEA     0x0F  /* NMEA output enable (0/1) */

/* LBE-1425 GNSS bitmask bits (LBE_1425_SET_GNSS arg). BeiDou is mutually
 * exclusive with the GPS/SBAS/Galileo group; GLONASS is unrestricted. */
#define LBE_1425_GNSS_GPS     0x01
#define LBE_1425_GNSS_SBAS    0x02
#define LBE_1425_GNSS_GALILEO 0x04
#define LBE_1425_GNSS_BEIDOU  0x08
#define LBE_1425_GNSS_IMES    0x10  /* mask bit = 1 << u-blox gnssId */
#define LBE_1425_GNSS_QZSS    0x20  /* confirmed via CFG-GNSS readback */
#define LBE_1425_GNSS_GLONASS 0x40

/* Mini-specific opcodes (differ from 1420/1421 at the same opcode numbers).
 * LBE_MINI_SET_PLL shares its opcode with LBE_1420_SET_F1 but carries a full
 * Si5351C divider-chain program (fin, N3, N2_HS, N2_LS, N1_HS, NC1_LS, NC2_LS,
 * SKEW, BW) instead of a raw frequency. LBE_MINI_SET_DRIVE collides with
 * LBE_1420_SET_F1_TEMP; on Mini the payload is a 2-bit output-drive forward
 * index (0=8 mA, 1=16 mA, 2=24 mA, 3=32 mA factory default). Readback in
 * the feature report at f[1] uses the same encoding; see
 * docs/reverse/LBE-Mini-config-v1.10.md for the USBPcap evidence. */
#define LBE_MINI_SET_DRIVE   0x03
#define LBE_MINI_SET_PLL     0x04
#define LBE_MINI_UBX_WRAP    0x08
#define LBE_MINI_NAV_STREAM  0x0A

/* Max supported frequency in Hz */
#define LBE_1420_MAX_FREQ 1600000000UL
#define LBE_1421_MAX_FREQ 1400000000UL
#define LBE_MINI_MAX_FREQ 810000000UL
/* LBE-1425 has asymmetric per-output limits: OUT1 <= 800 MHz (the 1PPS
 * output), OUT2 <= 1.4 GHz. Enforced per output via lbe_max_freq(). */
#define LBE_1425_OUT1_MAX_FREQ 800000000UL
#define LBE_1425_OUT2_MAX_FREQ 1400000000UL

#endif // LBE_COMMON_H
