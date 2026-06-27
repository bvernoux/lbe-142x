/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_common.h"
#include "lbe_model.h"
#include "lbe_transport.h"
#include "lbe_serial.h"
#include "lbe_platform.h"
#include "nmea.h"
#include "gnss_view.h"
#include "ubx.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same wire convention as LBE-1420: opcode doubles as the HID Report ID
 * and the USB payload is {opcode, args, ...}. Status reads use the
 * dedicated status Report ID 0x4B. The 1421 layout differs from the
 * 1420 layout in the status report (frequency2, per-output power) and
 * in the frequency commands (u32 at buf[5..8] rather than buf[1..4]). */
#define LBE_STATUS_REPORT_ID 0x4B

static int send_cmd(struct lbe_transport *t, uint8_t opcode,
                    const uint8_t *args, size_t args_offset, size_t n) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	buf[0] = opcode;
	if (args && n) {
		size_t room = LBE_REPORT_SIZE - args_offset;
		memcpy(&buf[args_offset], args, n > room ? room : n);
	}
	return lbe_transport_feat_set(t, opcode, buf);
}

static int m1421_get_status(struct lbe_transport *t, struct lbe_status *s) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};

	/* A valid status always carries a non-zero OUT1 frequency (min 1 Hz,
	 * realistically MHz). An all-zero report is a transient garbage read --
	 * seen e.g. when an antenna unplug briefly shorts and stuns the MCU,
	 * which would otherwise show as a bogus "Short Circuit / 0 Hz". Retry
	 * once before giving up rather than reporting the false state. */
	for (int attempt = 0; attempt < 2; attempt++) {
		memset(buf, 0, sizeof buf);
		if (lbe_transport_feat_get(t, LBE_STATUS_REPORT_ID, buf) < 0) return -1;
		uint32_t f1 = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
		if (f1 != 0) break;
		if (attempt == 0) lbe_sleep_ms(20);
		else return -1;   /* persistently implausible -- treat as failed read */
	}

	memcpy(s->raw, buf, LBE_REPORT_SIZE);   /* keep the raw report for inspection */
	s->raw_status     = buf[1];
	s->frequency1     = buf[6]  | (buf[7]  << 8) | (buf[8]  << 16) | (buf[9]  << 24);
	s->frequency2     = buf[14] | (buf[15] << 8) | (buf[16] << 16) | (buf[17] << 24);
	s->fll_enabled    = buf[18] != 0;
	s->out1_power_low = buf[19] != 0;
	s->out2_power_low = buf[20] != 0;
	s->antenna_current_ma = buf[23];   /* LBE-1425 antenna bias current (mA) */
	s->out1_drive_ma  = 0;
	s->outputs_enabled = (s->raw_status & (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT))
	                  == (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT);
	s->pll_locked = (s->raw_status & LBE_PLL_LOCK_BIT) != 0;
	s->antenna_ok = (s->raw_status & LBE_ANT_OK_BIT) != 0;
	s->pps_enabled = (s->raw_status & LBE_PPS_EN_BIT) != 0;
	return 0;
}

static int m1421_set_frequency_op(struct lbe_transport *t, uint8_t opcode, uint32_t hz) {
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	/* 1421 places the u32 at byte offset 5 of the payload, not 1. */
	return send_cmd(t, opcode, args, 5, sizeof args);
}

static int m1421_set_frequency(struct lbe_transport *t, int output, uint32_t hz) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_F1;
	else if (output == 2) opcode = LBE_1421_SET_F2;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	return m1421_set_frequency_op(t, opcode, hz);
}

static int m1421_set_frequency_temp(struct lbe_transport *t, int output, uint32_t hz) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_F1_TEMP;
	else if (output == 2) opcode = LBE_1421_SET_F2_TEMP;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	return m1421_set_frequency_op(t, opcode, hz);
}

static int m1421_set_outputs_enable(struct lbe_transport *t, int enable) {
	/* 0x03 = both outputs on; 0x00 = both off. */
	uint8_t arg = enable ? 0x03 : 0x00;
	return send_cmd(t, LBE_142X_EN_OUT, &arg, 1, 1);
}

static int m1421_blink_leds(struct lbe_transport *t) {
	return send_cmd(t, LBE_142X_BLINK_OUT, NULL, 1, 0);
}

static int m1421_set_pll_mode(struct lbe_transport *t, int fll_mode) {
	uint8_t arg = fll_mode ? 0x01 : 0x00;
	return send_cmd(t, LBE_142X_SET_PLL, &arg, 1, 1);
}

static int m1421_set_1pps(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x01 : 0x00;
	return send_cmd(t, LBE_1421_SET_PPS, &arg, 1, 1);
}

static int m1421_set_power_level(struct lbe_transport *t, int output, int low) {
	uint8_t opcode;
	if (output == 1)      opcode = LBE_1421_SET_PWR1;
	else if (output == 2) opcode = LBE_1421_SET_PWR2;
	else { fprintf(stderr, "Invalid output selection\n"); return -1; }
	uint8_t arg = low ? 0x01 : 0x00;
	return send_cmd(t, opcode, &arg, 1, 1);
}

/* --- LBE-1425-only commands ---------------------------------------------
 * Same SET_REPORT feature-report transport as the rest of the 1421 family
 * (opcode in payload byte 0, arg in byte 1). Confirmed against a vendor-tool
 * USB capture; see docs/reverse/LBE-1425-config-v1.10.md. Wired into
 * lbe_ops_1425 only, so 1421/1423 never emit these opcodes (which would mean
 * something else on those models). */
static int m1425_set_gnss(struct lbe_transport *t, uint8_t mask) {
	return send_cmd(t, LBE_1425_SET_GNSS, &mask, 1, 1);
}

static int m1425_set_dynmodel(struct lbe_transport *t, uint8_t model) {
	return send_cmd(t, LBE_1425_SET_DYNMODEL, &model, 1, 1);
}

static int m1425_set_nmea(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x01 : 0x00;
	return send_cmd(t, LBE_1425_SET_NMEA, &arg, 1, 1);
}

/* Live chronometer-style NMEA + 1PPS monitor. The 1421 streams GPS
 * data over USB CDC (not HID) and drives the u-blox 1PPS onto the
 * CDC DCD line. Port selection: LBE_PORT env var first, else first
 * COM/ttyACM* whose stream starts with "$G".
 *
 * Design: read NMEA with a short timeout (20 ms) so we poll DCD at
 * ~50 Hz. Redraw at ~20 Hz, interpolating sub-second UTC from
 * (now - last_edge). A ring of recent intervals feeds jitter stats. */
#define PPS_HISTORY     30
#define REDRAW_PERIOD   50   /* ms between screen redraws (~20 Hz) */
#define READ_TIMEOUT    20   /* ms; caps DCD poll period */

struct pps_tracker {
	int      dcd_prev;
	uint32_t edge_ms;           /* lbe_millis of last rising edge */
	uint32_t edges;
	uint32_t hist[PPS_HISTORY]; /* recent intervals in ms, circular */
	uint32_t hist_n;            /* total intervals pushed (>= edges-1) */
	uint32_t last_nmea_ms;      /* last NMEA arrival (for NMEA->edge latency) */
	uint32_t nmea_to_edge_ms;   /* latency of most recent edge */
};

static void pps_on_edge(struct pps_tracker *p, uint32_t now) {
	if (p->edges > 0) {
		p->hist[p->hist_n % PPS_HISTORY] = now - p->edge_ms;
		p->hist_n++;
	}
	if (p->last_nmea_ms) p->nmea_to_edge_ms = now - p->last_nmea_ms;
	p->edge_ms = now;
	p->edges++;
}

static void pps_stats(const struct pps_tracker *p,
                      uint32_t *avg, uint32_t *min, uint32_t *max,
                      uint32_t *samples) {
	uint32_t n = p->hist_n < PPS_HISTORY ? p->hist_n : PPS_HISTORY;
	*samples = n;
	if (n == 0) { *avg = *min = *max = 0; return; }
	uint32_t mn = UINT32_MAX, mx = 0, sum = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t v = p->hist[i];
		if (v < mn) mn = v;
		if (v > mx) mx = v;
		sum += v;
	}
	*avg = sum / n;
	*min = mn;
	*max = mx;
}

static int m1421_monitor(struct lbe_transport *t) {
	(void)t;
	char port[64] = {0};
	if (!lbe_getenv("LBE_PORT", port, sizeof port) &&
	    lbe_serial_find_nmea(port, sizeof port) < 0) {
		fprintf(stderr, "No NMEA serial port found. Use --port COMxx (Windows) "
		                "or --port /dev/ttyACM0 (Linux).\n");
		return -1;
	}

	struct lbe_serial *s = lbe_serial_open(port);
	if (!s) return -1;

	/* main.c passes the concrete model ("1425 dual output", ...) via env;
	 * fall back to the family name when run standalone. */
	char model_name[64];
	char title[96];
	if (!lbe_getenv("LBE_MODEL_NAME", model_name, sizeof model_name))
		snprintf(title, sizeof title, "LBE-142x GPS Monitor");
	else
		snprintf(title, sizeof title, "LBE-%s GPS Monitor", model_name);

	struct gnss_pvt pvt = {0};
	struct gnss_svinfo sv = {0};
	struct nmea_state st;
	nmea_state_init(&st);
	struct pps_tracker pps = {0};
	pps.dcd_prev = lbe_serial_get_dcd(s);

	lbe_enable_vt();
	printf("\033[2J\033[H");
	printf("Opening %s and waiting for NMEA...\n", port);
	fflush(stdout);

	char dbg_buf[2];
	int debug = lbe_getenv("LBE_NMEA_DEBUG", dbg_buf, sizeof dbg_buf);

	int lines = 0;
	int bad_ck = 0;
	uint32_t last_redraw = 0;

	for (;;) {
		int dcd = lbe_serial_get_dcd(s);
		if (dcd >= 0 && pps.dcd_prev == 0 && dcd == 1) {
			pps_on_edge(&pps, lbe_millis());
		}
		if (dcd >= 0) pps.dcd_prev = dcd;

		char line[256];
		int r = lbe_serial_readline(s, line, sizeof line, READ_TIMEOUT);
		if (r < 0) { fprintf(stderr, "serial read failed\n"); break; }
		if (r > 0) {
			lines++;
			pps.last_nmea_ms = lbe_millis();
			if (debug) fprintf(stderr, "%s\n", line);
			const char *star = strchr(line, '*');
			if (star && !nmea_checksum_ok(line)) {
				bad_ck++;
			} else {
				(void)nmea_feed(&st, line, &pvt, &sv);
			}
		}

		uint32_t now = lbe_millis();
		if (now - last_redraw < REDRAW_PERIOD) continue;
		last_redraw = now;

		int frac_ms = -1;
		if (pps.edges > 0) {
			uint32_t f = now - pps.edge_ms;
			if (f > 999) f = 999;  /* cap if PPS stalls */
			frac_ms = (int)f;
		}
		gnss_draw(title, &pvt, &sv, frac_ms);

		if (pps.edges == 0) {
			printf("\nPPS:    waiting for DCD edge...\033[K\n");
		} else {
			uint32_t avg, mn, mx, n;
			pps_stats(&pps, &avg, &mn, &mx, &n);
			if (n == 0) {
				printf("\nPPS:    edges=%" PRIu32 "  waiting for interval samples..."
				       "\033[K\n", pps.edges);
			} else {
				printf("\nPPS:    edges=%" PRIu32 "  avg=%" PRIu32 " ms  "
				       "min=%" PRIu32 "  max=%" PRIu32 "  (last %" PRIu32
				       ")  NMEA->edge=%" PRIu32 " ms\033[K\n",
				       pps.edges, avg, mn, mx, n, pps.nmea_to_edge_ms);
			}
		}
		printf("Stream: lines=%d bad_ck=%d  port=%s\033[K\n\033[J",
		       lines, bad_ck, port);
		fflush(stdout);
	}

	lbe_serial_close(s);
	return 0;
}

/* --- LBE-1425 UBX diagnostics monitor --------------------------------------
 * The 1425 streams u-blox UBX (NAV-PVT / NAV-SAT / NAV-CLOCK) out of its HID
 * interrupt-IN endpoint 0x83, each 64-byte frame headed by [seq][len] where
 * len (typically 0x3E = 62) is the UBX-payload byte count and a len of 0 marks
 * a keepalive frame. We strip that 2-byte header and feed the rest to the
 * shared UBX reassembler, rendering the same CNR view as the Mini plus a
 * NAV-CLOCK disciplining line. See docs/reverse/LBE-1425-config-v1.10.md. */
#define LBE_1425_DIAG_EP   0x83
#define LBE_1425_DIAG_FRM  64

/* Best-effort: replay the three UBX-poll requests the vendor GUI issues so a
 * device that only streams when polled starts sending. Harmless if the stream
 * is already free-running. */
static void m1425_diag_poll(struct lbe_transport *t) {
	static const uint8_t sels[3] = {0x35, 0x22, 0x07};
	for (int i = 0; i < 3; i++) {
		uint8_t args[7] = {0x06, 0x01, 0x08, 0x00, 0x01, sels[i], 0x0A};
		send_cmd(t, LBE_MINI_UBX_WRAP, args, 1, sizeof args);
	}
}

static int m1425_diag(struct lbe_transport *t) {
	lbe_transport_claim(t);

	struct gnss_pvt    pvt = {0};
	struct gnss_svinfo sv  = {0};
	struct ubx_clock   clk = {0};
	uint8_t buf[1024];
	size_t  buf_len = 0;

	lbe_enable_vt();
	printf("\033[2J\033[H");
	printf("Requesting UBX diagnostics stream...\n");
	fflush(stdout);

	m1425_diag_poll(t);
	uint32_t last_poll = lbe_millis();

	for (;;) {
		uint8_t r[LBE_1425_DIAG_FRM];
		int got = lbe_transport_read_input(t, LBE_1425_DIAG_EP, r, sizeof r, 500);
		if (got < 0) break;

		/* Re-poll ~once a second in case the stream needs keeping alive. */
		uint32_t now = lbe_millis();
		if (now - last_poll > 1000) { m1425_diag_poll(t); last_poll = now; }

		/* Frame = [seq][len][payload]; len 0 is a keepalive. */
		if (got >= 3 && r[1] != 0) {
			size_t payload = r[1];
			if (payload > (size_t)(got - 2)) payload = (size_t)(got - 2);
			if (ubx_consume(buf, &buf_len, sizeof buf, r + 2, payload,
			                &pvt, &sv, &clk) > 0) {
				gnss_draw("LBE-1425 GPS Diagnostics", &pvt, &sv, -1);
				if (clk.valid) {
					/* u-blox reports 0xFFFFFFFF when an accuracy is unknown
					 * (e.g. before a fix) -- show n/a rather than 4294967295. */
					char ta[20], fa[20];
					if (clk.tacc_ns == UINT32_MAX) snprintf(ta, sizeof ta, "n/a");
					else snprintf(ta, sizeof ta, "%u ns", clk.tacc_ns);
					if (clk.facc_ps == UINT32_MAX) snprintf(fa, sizeof fa, "n/a");
					else snprintf(fa, sizeof fa, "%u ps/s", clk.facc_ps);
					printf("\nClock:  bias=%+d ns  drift=%+d ns/s  "
					       "tAcc=%s  fAcc=%s\033[K\n",
					       clk.clkb_ns, clk.clkd_nsps, ta, fa);
				} else {
					printf("\nClock:  (no solution yet)\033[K\n");
				}
				/* Always erase below the last row so a shrinking satellite
				 * list (e.g. when signal drops) doesn't leave stale lines. */
				printf("\033[J");
				fflush(stdout);
			}
		}
	}

	lbe_transport_release(t);
	return 0;
}

/* --- LBE-1425 GPS info / true antenna status (experimental) ----------------
 * The feature-report ANT bit only flags a *short*; a disconnected antenna
 * reads "OK". The u-blox knows the real state via UBX-MON-HW (M8) or MON-RF
 * (M9/M10) antStatus (OK/OPEN/SHORT). We poll MON-VER (one-shot) for the
 * module version, and enable MON-HW + MON-RF (CFG-MSG, the same wrap --diag
 * uses) to read antStatus, then turn them back off. */
static const char *ant_status_name(uint8_t s) {
	switch (s) {
	case 0: return "INIT"; case 1: return "DONTKNOW"; case 2: return "OK";
	case 3: return "SHORT"; case 4: return "OPEN"; default: return "?";
	}
}
static const char *ant_power_name(uint8_t p) {
	return p == 0 ? "off" : p == 1 ? "on" : p == 2 ? "DONTKNOW" : "?";
}

static const char *gnssid_name(uint8_t g) {
	switch (g) {
	case 0: return "GPS"; case 1: return "SBAS"; case 2: return "Galileo";
	case 3: return "BeiDou"; case 4: return "IMES"; case 5: return "QZSS";
	case 6: return "GLONASS"; default: return "?";
	}
}

/* Wrap a UBX command for the device's 0x08 UBX-wrap opcode: the firmware adds
 * the B5 62 sync + checksum, we hand it {class, id, len_lo, len_hi, payload}. */
static void m1425_ubx_wrap(struct lbe_transport *t, const uint8_t *w, size_t n) {
	send_cmd(t, LBE_MINI_UBX_WRAP, w, 1, n);
}

static int m1425_gps_info(struct lbe_transport *t) {
	lbe_transport_claim(t);
	/* MON-VER poll, and enable MON-HW (0A 09) + MON-RF (0A 38) at rate 1 via
	 * CFG-MSG (06 01, 8-byte payload: msgClass, msgID, rate[6]). */
	const uint8_t ver_poll[]  = {0x0A, 0x04, 0x00, 0x00};
	const uint8_t gnss_poll[] = {0x06, 0x3E, 0x00, 0x00};   /* poll CFG-GNSS */
	const uint8_t en_hw[]    = {0x06,0x01,0x08,0x00, 0x0A,0x09,0x01,0,0,0,0,0};
	const uint8_t en_rf[]    = {0x06,0x01,0x08,0x00, 0x0A,0x38,0x01,0,0,0,0,0};
	m1425_ubx_wrap(t, ver_poll, sizeof ver_poll);
	m1425_ubx_wrap(t, gnss_poll, sizeof gnss_poll);
	m1425_ubx_wrap(t, en_hw, sizeof en_hw);
	m1425_ubx_wrap(t, en_rf, sizeof en_rf);

	printf("u-blox GPS module:\n");
	uint8_t buf[2048];
	size_t  buf_len = 0;
	int got_ver = 0, got_ant = 0, got_gnss = 0;
	for (int iter = 0; iter < 120 && !(got_ver && got_ant && got_gnss); iter++) {
		uint8_t r[LBE_1425_DIAG_FRM];
		int n = lbe_transport_read_input(t, LBE_1425_DIAG_EP, r, sizeof r, 50);
		if (n < 3 || r[1] == 0) continue;
		size_t payload = r[1];
		if (payload > (size_t)(n - 2)) payload = (size_t)(n - 2);
		if (buf_len + payload > sizeof buf) buf_len = 0;
		memcpy(buf + buf_len, r + 2, payload);
		buf_len += payload;

		size_t i = 0;
		while (i + 8 <= buf_len) {
			if (buf[i] != 0xB5 || buf[i + 1] != 0x62) { i++; continue; }
			size_t ul = (size_t)(buf[i + 4] | (buf[i + 5] << 8));
			if (ul > 1024) { i++; continue; }
			size_t total = 8 + ul;
			if (i + total > buf_len) break;
			if (!ubx_checksum_ok(&buf[i], total)) { i++; continue; }
			uint8_t cls = buf[i + 2], id = buf[i + 3];
			const uint8_t *p = &buf[i + 6];
			if (cls == 0x0A && id == 0x04 && ul >= 40 && !got_ver) {
				printf("  SW version : %.30s\n", (const char *)p);
				printf("  HW version : %.10s\n", (const char *)(p + 30));
				for (size_t eo = 40, e = 0; eo + 30 <= ul; eo += 30)
					printf("  Extension %zu: %.30s\n", ++e, (const char *)(p + eo));
				got_ver = 1;
			} else if (cls == 0x0A && id == 0x09 && ul >= 22 && !got_ant) {
				printf("  Antenna    : %s (power %s)  [MON-HW]\n",
				       ant_status_name(p[20]), ant_power_name(p[21]));
				got_ant = 1;
			} else if (cls == 0x0A && id == 0x38 && ul >= 4 && !got_ant) {
				for (uint8_t b = 0; b < p[1] && 4 + 24u * b + 4 <= ul; b++)
					printf("  Antenna    : %s (power %s)  [MON-RF block %u]\n",
					       ant_status_name(p[4 + 24 * b + 2]),
					       ant_power_name(p[4 + 24 * b + 3]), b);
				got_ant = 1;
			} else if (cls == 0x06 && id == 0x3E && ul >= 4 && !got_gnss) {
				/* CFG-GNSS: per 8-byte block {gnssId, .., flags(LE32)};
				 * flags bit 0 = enabled. Lists what the u-blox is really
				 * tracking -- incl. QZSS/IMES, which the vendor UI hides. */
				printf("  GNSS enabled:");
				for (uint8_t b = 0; b < p[3] && 4 + 8u * b + 8 <= ul; b++) {
					const uint8_t *blk = p + 4 + 8 * b;
					if (blk[4] & 0x01) printf(" %s", gnssid_name(blk[0]));
				}
				printf("\n");
				got_gnss = 1;
			}
			i += total;
		}
		if (i > 0) { memmove(buf, buf + i, buf_len - i); buf_len -= i; }
	}

	/* Turn the MON-HW / MON-RF streams back off (rate 0). */
	const uint8_t dis_hw[] = {0x06,0x01,0x08,0x00, 0x0A,0x09,0x00,0,0,0,0,0};
	const uint8_t dis_rf[] = {0x06,0x01,0x08,0x00, 0x0A,0x38,0x00,0,0,0,0,0};
	m1425_ubx_wrap(t, dis_hw, sizeof dis_hw);
	m1425_ubx_wrap(t, dis_rf, sizeof dis_rf);
	lbe_transport_release(t);

	if (!got_ver)
		fprintf(stderr, "  (no MON-VER reply -- poll may not be forwarded)\n");
	if (!got_ant)
		fprintf(stderr, "  (no MON-HW/RF reply -- antenna status unavailable)\n");
	if (!got_gnss)
		fprintf(stderr, "  (no CFG-GNSS reply -- constellation list unavailable)\n");
	return (got_ver || got_ant || got_gnss) ? 0 : -1;
}

const struct lbe_model_ops lbe_ops_1421 = {
	.name               = "1421 dual output",
	.max_freq           = LBE_1421_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1421_get_status,
	.set_frequency      = m1421_set_frequency,
	.set_frequency_temp = m1421_set_frequency_temp,
	.set_outputs_enable = m1421_set_outputs_enable,
	.blink_leds         = m1421_blink_leds,
	.set_pll_mode       = m1421_set_pll_mode,
	.set_1pps           = m1421_set_1pps,
	.set_power_level    = m1421_set_power_level,
	.monitor            = m1421_monitor,
};

/* LBE-1425: the 1421 dual-output protocol verbatim, plus the 1425's extra
 * GNSS / dynamic-model / NMEA-output commands. (Per-output frequency caps
 * are enforced separately in lbe_device.c via the PID.) */
const struct lbe_model_ops lbe_ops_1425 = {
	.name               = "1425 dual output",
	.max_freq           = LBE_1425_OUT2_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1421_get_status,
	.set_frequency      = m1421_set_frequency,
	.set_frequency_temp = m1421_set_frequency_temp,
	.set_outputs_enable = m1421_set_outputs_enable,
	.blink_leds         = m1421_blink_leds,
	.set_pll_mode       = m1421_set_pll_mode,
	.set_1pps           = m1421_set_1pps,
	.set_power_level    = m1421_set_power_level,
	.monitor            = m1421_monitor,
	.set_gnss           = m1425_set_gnss,
	.set_dynmodel       = m1425_set_dynmodel,
	.set_nmea           = m1425_set_nmea,
	.diag               = m1425_diag,
	.gps_info           = m1425_gps_info,
};
