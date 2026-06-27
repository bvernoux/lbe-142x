/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef UBX_H
#define UBX_H

#include <stdint.h>
#include <stddef.h>
#include "gnss_view.h"

/* Shared u-blox UBX decoding. The LBE-Mini streams UBX out of its HID
 * interrupt endpoint; the LBE-1425 streams the same messages on its EP 0x83
 * diagnostics endpoint (behind a different HID frame header). Each model
 * strips its own framing and feeds the raw UBX bytes here. */

/* UBX NAV-CLOCK (class 0x01, id 0x22): receiver clock solution. This is the
 * disciplining/stability telemetry the LBE-1425 "diagnostics" view shows. */
struct ubx_clock {
	int      valid;
	int32_t  clkb_ns;    /* clock bias (ns) */
	int32_t  clkd_nsps;  /* clock drift (ns/s) */
	uint32_t tacc_ns;    /* time accuracy estimate (ns) */
	uint32_t facc_ps;    /* frequency accuracy estimate (ps/s) */
};

/* Fletcher-8 checksum over a full UBX frame (sync..payload..ck), total bytes.
 * Returns 1 if the two trailing checksum bytes match. */
int ubx_checksum_ok(const uint8_t *msg, size_t total);

/* Decode individual UBX payloads (pointer past the 6-byte header, payload
 * length n). No-ops if n is too short. */
void ubx_parse_pvt(const uint8_t *p, int n, struct gnss_pvt *pvt);
void ubx_parse_sat(const uint8_t *p, int n, struct gnss_svinfo *sv);
void ubx_parse_clock(const uint8_t *p, int n, struct ubx_clock *clk);

/* Append `in` (in_len already-de-framed UBX bytes) to the reassembly buffer
 * `buf` (capacity buf_cap, current length *buf_len), then extract every
 * complete checksum-valid UBX message, dispatching NAV-PVT/NAV-SAT/NAV-CLOCK
 * into the provided structs (any may be NULL). Consumed bytes are removed from
 * the front of buf. Returns the number of NAV-PVT updates applied (so a caller
 * can trigger a redraw). On buffer overflow the buffer is reset. */
int ubx_consume(uint8_t *buf, size_t *buf_len, size_t buf_cap,
                const uint8_t *in, size_t in_len,
                struct gnss_pvt *pvt, struct gnss_svinfo *sv,
                struct ubx_clock *clk);

#endif
