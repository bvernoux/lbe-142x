/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "ubx.h"
#include <string.h>

int ubx_checksum_ok(const uint8_t *msg, size_t total) {
	if (total < 8) return 0;
	uint8_t ca = 0, cb = 0;
	for (size_t k = 2; k < total - 2; k++) {
		ca = (uint8_t)(ca + msg[k]);
		cb = (uint8_t)(cb + ca);
	}
	return ca == msg[total - 2] && cb == msg[total - 1];
}

void ubx_parse_pvt(const uint8_t *p, int n, struct gnss_pvt *pvt) {
	if (n < 92) return;
	pvt->year     = (uint16_t)(p[4] | (p[5] << 8));
	pvt->month    = p[6];
	pvt->day      = p[7];
	pvt->hour     = p[8];
	pvt->min      = p[9];
	pvt->sec      = p[10];
	pvt->fix_type = p[20];
	/* NAV-PVT valid flags (p[11]): bit0 validDate, bit1 validTime. Without
	 * a fix the receiver emits a placeholder date/time -- flag it untrusted
	 * so the renderer doesn't show e.g. a 2015 epoch as if it were real. */
	pvt->time_valid = ((p[11] & 0x03) == 0x03);
	pvt->num_sv   = p[23];
	pvt->lon_1e7  = (int32_t)((uint32_t)p[24] | ((uint32_t)p[25] << 8)
	                        | ((uint32_t)p[26] << 16) | ((uint32_t)p[27] << 24));
	pvt->lat_1e7  = (int32_t)((uint32_t)p[28] | ((uint32_t)p[29] << 8)
	                        | ((uint32_t)p[30] << 16) | ((uint32_t)p[31] << 24));
	pvt->hmsl_mm  = (int32_t)((uint32_t)p[36] | ((uint32_t)p[37] << 8)
	                        | ((uint32_t)p[38] << 16) | ((uint32_t)p[39] << 24));
	pvt->valid = 1;
}

/* NAV-SAT payload (UBX PROTVER 18+):
 *   p[0..3]  iTOW (u32 LE)
 *   p[4]     version (0x01)
 *   p[5]     numSvs
 *   p[6..7]  reserved
 *   p[8+12*i .. +11] per-SV: gnssId, svId, cno, elev(i8), azim(i16),
 *                    prRes(i16), flags(u32) -- flags bit 3 = svUsed */
void ubx_parse_sat(const uint8_t *p, int n, struct gnss_svinfo *sv) {
	if (n < 8) return;
	uint8_t num = p[5];
	if ((int)(8 + num * 12) > n) num = (uint8_t)((n - 8) / 12);
	if (num > 64) num = 64;
	sv->num_ch = num;
	for (uint8_t i = 0; i < num; i++) {
		const uint8_t *s = &p[8 + i * 12];
		uint32_t flags = (uint32_t)s[8]
		              | ((uint32_t)s[9]  << 8)
		              | ((uint32_t)s[10] << 16)
		              | ((uint32_t)s[11] << 24);
		sv->sats[i].gnss_id = s[0];
		sv->sats[i].svid    = s[1];
		sv->sats[i].cno     = s[2];
		sv->sats[i].elev    = (int8_t)s[3];
		sv->sats[i].azim    = (int16_t)((uint16_t)s[4] | ((uint16_t)s[5] << 8));
		sv->sats[i].used    = (uint8_t)((flags >> 3) & 0x01u);
	}
	sv->valid = 1;
}

/* NAV-CLOCK payload (20 bytes): iTOW(u32) clkB(i32 ns) clkD(i32 ns/s)
 * tAcc(u32 ns) fAcc(u32 ps/s). */
void ubx_parse_clock(const uint8_t *p, int n, struct ubx_clock *clk) {
	if (n < 20) return;
	clk->clkb_ns   = (int32_t)((uint32_t)p[4]  | ((uint32_t)p[5]  << 8)
	                         | ((uint32_t)p[6]  << 16) | ((uint32_t)p[7]  << 24));
	clk->clkd_nsps = (int32_t)((uint32_t)p[8]  | ((uint32_t)p[9]  << 8)
	                         | ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24));
	clk->tacc_ns   = (uint32_t)p[12] | ((uint32_t)p[13] << 8)
	               | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
	clk->facc_ps   = (uint32_t)p[16] | ((uint32_t)p[17] << 8)
	               | ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);
	clk->valid = 1;
}

int ubx_consume(uint8_t *buf, size_t *buf_len, size_t buf_cap,
                const uint8_t *in, size_t in_len,
                struct gnss_pvt *pvt, struct gnss_svinfo *sv,
                struct ubx_clock *clk) {
	if (in_len == 0) return 0;
	if (*buf_len + in_len > buf_cap)
		*buf_len = 0;                      /* overflow: drop stale partial */
	if (in_len > buf_cap) in_len = buf_cap;
	memcpy(buf + *buf_len, in, in_len);
	*buf_len += in_len;

	int pvt_updates = 0;
	size_t i = 0;
	while (i + 8 <= *buf_len) {
		if (buf[i] != 0xB5 || buf[i + 1] != 0x62) { i++; continue; }
		uint16_t ubx_len = (uint16_t)(buf[i + 4] | (buf[i + 5] << 8));
		if (ubx_len > 512) { i++; continue; }
		size_t total = (size_t)8 + ubx_len;
		if (i + total > *buf_len) break;   /* message spans into next frame */
		if (!ubx_checksum_ok(&buf[i], total)) { i++; continue; }

		uint8_t cls = buf[i + 2], id = buf[i + 3];
		const uint8_t *p = &buf[i + 6];
		if (cls == 0x01 && id == 0x07 && pvt) {
			ubx_parse_pvt(p, ubx_len, pvt);
			if (pvt->valid) pvt_updates++;
		} else if (cls == 0x01 && id == 0x35 && sv) {
			ubx_parse_sat(p, ubx_len, sv);
		} else if (cls == 0x01 && id == 0x22 && clk) {
			ubx_parse_clock(p, ubx_len, clk);
		}
		i += total;
	}
	if (i > 0) {
		memmove(buf, buf + i, *buf_len - i);
		*buf_len -= i;
	}
	return pvt_updates;
}
