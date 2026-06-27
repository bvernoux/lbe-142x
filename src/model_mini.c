/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_common.h"
#include "lbe_model.h"
#include "lbe_transport.h"
#include "lbe_platform.h"
#include "gnss_view.h"
#include "ubx.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MINI_INPUT_REPORT_SIZE 64
#define MINI_INTERRUPT_IN_EP   0x81

/* Mini has no Report ID in its HID descriptor -- every command uses
 * wValue=0x0300 and places the opcode in data[0]. */
static int mini_cmd(struct lbe_transport *t, uint8_t op,
                    const uint8_t *args, size_t n) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	buf[0] = op;
	if (args && n) {
		size_t room = LBE_REPORT_SIZE - 1;
		memcpy(&buf[1], args, n > room ? room : n);
	}
	return lbe_transport_feat_set(t, 0, buf);
}

/* Solve the Si5351 divider chain for `f_out`.
 * f_out = f_in * N2_HS * N2_LS / (N3 * N1_HS * NC1_LS).
 * Constraints: N2_HS, N1_HS in [4,11]; N2_LS, NC1_LS in [2,2^20] even
 * (NC1_LS may also be 1); N3 in [1,2^19]. f_in fixed at 97600 Hz --
 * the factory default confirmed by reading the feature report after
 * a firmware flash. Returns 0 on success, -1 if no valid divider set
 * was found. */
static int mini_solve_pll(uint32_t f_out,
                          uint32_t *fin_out, uint32_t *n3_out,
                          uint32_t *n2hs_out, uint32_t *n2ls_out,
                          uint32_t *n1hs_out, uint32_t *nc1_out) {
	const uint32_t f_in = 97600;
	uint64_t a = f_out, b = f_in;
	while (b) { uint64_t tmp = b; b = a % b; a = tmp; }
	uint64_t p = f_out / a;
	uint64_t q = f_in / a;

	/* Two passes: first prefer a VCO near the Si5351's 5-6.5 GHz
	 * native band; if no divider set fits there, accept any valid
	 * one. */
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t k = 1; k <= 4096; k++) {
			uint64_t M = (uint64_t)k * p;
			uint64_t D = (uint64_t)k * q;
			if (M > (uint64_t)11 * (1ULL << 20)) break;
			if (D > (uint64_t)11 * (1ULL << 20) * (1ULL << 19)) break;

			uint64_t f_osc = (uint64_t)f_in * M;
			if (pass == 0 && (f_osc < 5000000000ULL || f_osc > 6500000000ULL))
				continue;

			for (unsigned nh = 11; nh >= 4; nh--) {
				if (M % nh) continue;
				uint64_t n2ls = M / nh;
				if (n2ls < 2 || n2ls > (1ULL << 20) || (n2ls & 1)) continue;

				for (unsigned nh1 = 11; nh1 >= 4; nh1--) {
					if (D % nh1) continue;
					uint64_t nc1 = D / nh1;
					int ok = (nc1 == 1) ||
					         (nc1 >= 2 && nc1 <= (1ULL << 20) && (nc1 & 1) == 0);
					if (!ok) continue;

					*fin_out  = f_in;
					*n3_out   = 1;
					*n2hs_out = (uint32_t)nh;
					*n2ls_out = (uint32_t)n2ls;
					*n1hs_out = (uint32_t)nh1;
					*nc1_out  = (uint32_t)nc1;
					return 0;
				}
			}
		}
	}
	return -1;
}

/* Vendor bootstrap so the interrupt-IN endpoint streams NAV-PVT.
 * Captured from the Windows vendor tool's pcap, with NAV-SAT (01 35)
 * substituted for the legacy NAV-SVINFO (01 30). The u-blox 8 in this
 * Mini reports PROTVER 18.00, which supports NAV-SAT; it carries the
 * gnssId per SV and a richer quality/used flag word. */
static void mini_enable_gps_stream(struct lbe_transport *t) {
	static const uint8_t sat[]    = {0x06, 0x01, 0x08, 0x00, 0x01, 0x35, 0x14};
	static const uint8_t clock_[] = {0x06, 0x01, 0x08, 0x00, 0x01, 0x22, 0x14};
	static const uint8_t pvt[]    = {0x06, 0x01, 0x08, 0x00, 0x01, 0x07, 0x0A};
	static const uint8_t refresh  = 0x04;
	uint8_t drain[LBE_REPORT_SIZE];

	mini_cmd(t, LBE_MINI_NAV_STREAM, &refresh, 1);
	(void)lbe_transport_feat_get_quiet(t, 0, drain);
	(void)lbe_transport_feat_get_quiet(t, 0, drain);
	mini_cmd(t, LBE_MINI_UBX_WRAP, sat,    sizeof sat);
	mini_cmd(t, LBE_MINI_UBX_WRAP, clock_, sizeof clock_);
	mini_cmd(t, LBE_MINI_UBX_WRAP, pvt,    sizeof pvt);
}

/* Forward declaration -- defined below, next to mini_monitor, but also
 * used by mini_sample_nav (called from mini_get_status). */
static void mini_sample_nav(struct lbe_transport *t,
                             int *fix, int *pll_hw_locked,
                             int *gps_signal_ok, uint8_t *signal_loss);

static int mini_init(struct lbe_transport *t) {
	lbe_transport_claim(t);
	mini_enable_gps_stream(t);
	return 0;
}

static int mini_get_status(struct lbe_transport *t, struct lbe_status *s) {
	uint8_t f[LBE_REPORT_SIZE];
	if (lbe_transport_feat_get(t, 0, f) < 0) return -1;

	/* Feature report, verified against the vendor v1.17 UI:
	 *   f[0]       outputs enable (0=off, non-zero=on; vendor writes 3)
	 *   f[1]       drive strength forward index: 0=8mA, 1=16mA,
	 *              2=24mA, 3=32mA (= Si5351C register field)
	 *   f[2..4]    fin, 3-byte LE
	 *   f[5..7]    N3-1
	 *   f[8]       N2_HS-4
	 *   f[9..11]   N2_LS-1
	 *   f[12]      N1_HS-4
	 *   f[13..15]  NC1_LS-1
	 *   f[20]      BW
	 * f_out = fin * N2_HS * N2_LS / (N3 * N1_HS * NC1_LS). */
	uint32_t fin  = (uint32_t)(f[2] | (f[3] << 8) | (f[4] << 16));
	uint32_t n3   = (uint32_t)(f[5] | (f[6] << 8) | (f[7] << 16)) + 1u;
	uint32_t n2hs = (uint32_t)f[8] + 4u;
	uint32_t n2ls = (uint32_t)(f[9] | (f[10] << 8) | (f[11] << 16)) + 1u;
	uint32_t n1hs = (uint32_t)f[12] + 4u;
	uint32_t nc1  = (uint32_t)(f[13] | (f[14] << 8) | (f[15] << 16)) + 1u;
	uint64_t den  = (uint64_t)n3 * n1hs * nc1;
	s->frequency1 = den ?
	    (uint32_t)(((uint64_t)fin * n2hs * n2ls) / den) : 0;
	s->frequency2       = 0;
	s->outputs_enabled  = (f[0] != 0);
	s->out1_drive_ma    = (uint8_t)(((f[1] & 0x03) + 1) * 8);
	s->out1_power_low   = (f[1] == 0);
	s->out2_power_low   = 0;
	s->fll_enabled      = 0;
	s->pps_enabled      = 0;
	s->antenna_ok       = 0;

	s->raw_status = 0;
	if (s->outputs_enabled) s->raw_status |= LBE_OUT1_EN_BIT;

	/* PLL Lock, GPS signal and signal-loss-count come from r[0..1] of the
	 * interrupt-IN stream (see mini_sample_nav). "GPS Lock" falls back to
	 * UBX NAV-PVT fixType >= 2 when the status byte reports no signal. */
	int fix, pll_hw, gps_sig;
	uint8_t sig_loss;
	mini_sample_nav(t, &fix, &pll_hw, &gps_sig, &sig_loss);
	s->pll_locked         = pll_hw;
	s->signal_loss_count  = sig_loss;
	if (pll_hw)                 s->raw_status |= LBE_PLL_LOCK_BIT;
	if (gps_sig || fix >= 2)    s->raw_status |= LBE_GPS_LOCK_BIT;
	return 0;
}

static int mini_set_frequency(struct lbe_transport *t, int output, uint32_t hz) {
	if (output != 1) {
		fprintf(stderr, "Mini only supports output 1\n");
		return -1;
	}
	uint32_t fin = 0, n3 = 0, n2hs = 0, n2ls = 0, n1hs = 0, nc1 = 0;
	if (mini_solve_pll(hz, &fin, &n3, &n2hs, &n2ls, &n1hs, &nc1) < 0) {
		fprintf(stderr, "Mini: no valid PLL divider chain for %u Hz\n", hz);
		return -1;
	}
	/* Opcode 0x04 payload (19 bytes):
	 *   [0..2]   fin (3-byte LE)
	 *   [3..5]   N3-1
	 *   [6]      N2_HS-4
	 *   [7..9]   N2_LS-1
	 *   [10]     N1_HS-4
	 *   [11..13] NC1_LS-1
	 *   [14..16] NC2_LS-1 (= NC1_LS on the single-output Mini)
	 *   [17]     SKEW
	 *   [18]     BW */
	uint8_t p[19] = {0};
	p[0]  = fin & 0xFF;
	p[1]  = (fin >> 8) & 0xFF;
	p[2]  = (fin >> 16) & 0xFF;
	uint32_t n3m = n3 - 1;
	p[3]  = n3m & 0xFF;
	p[4]  = (n3m >> 8) & 0xFF;
	p[5]  = (n3m >> 16) & 0xFF;
	p[6]  = (uint8_t)(n2hs - 4);
	uint32_t n2lsm = n2ls - 1;
	p[7]  = n2lsm & 0xFF;
	p[8]  = (n2lsm >> 8) & 0xFF;
	p[9]  = (n2lsm >> 16) & 0xFF;
	p[10] = (uint8_t)(n1hs - 4);
	uint32_t nc1m = nc1 - 1;
	p[11] = nc1m & 0xFF;
	p[12] = (nc1m >> 8) & 0xFF;
	p[13] = (nc1m >> 16) & 0xFF;
	p[14] = nc1m & 0xFF;
	p[15] = (nc1m >> 8) & 0xFF;
	p[16] = (nc1m >> 16) & 0xFF;
	p[17] = 0;
	p[18] = 9;
	return mini_cmd(t, LBE_MINI_SET_PLL, p, sizeof p);
}

static int mini_set_frequency_temp(struct lbe_transport *t, int output,
                                    uint32_t hz) {
	(void)t; (void)output; (void)hz;
	fprintf(stderr, "Temporary frequency is not supported on Mini\n");
	return -1;
}

static int mini_set_outputs_enable(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x03 : 0x00;
	return mini_cmd(t, LBE_142X_EN_OUT, &arg, 1);
}

static int mini_blink_leds(struct lbe_transport *t) {
	/* Vendor GUI semantics (v1.17, USBPcap-confirmed):
	 *   0x02 0x01 -> start LED blinking (latched, continues until stop)
	 *   0x02 0x00 -> stop LED blinking
	 * The GUI's button is a toggle. For the CLI we emulate the advertised
	 * "3 seconds" behaviour: start, wait, stop. */
	uint8_t on  = 0x01;
	uint8_t off = 0x00;
	int rc = mini_cmd(t, LBE_142X_BLINK_OUT, &on, 1);
	if (rc < 0) return rc;
	lbe_sleep_ms(3000);
	return mini_cmd(t, LBE_142X_BLINK_OUT, &off, 1);
}

static int mini_set_pll_mode(struct lbe_transport *t, int fll_mode) {
	(void)t; (void)fll_mode;
	fprintf(stderr, "PLL/FLL mode is not supported on Mini\n");
	return -1;
}

static int mini_set_1pps(struct lbe_transport *t, int enable) {
	(void)t; (void)enable;
	fprintf(stderr, "1PPS control is not supported on Mini\n");
	return -1;
}

/* UBX NAV-PVT / NAV-SAT decoding and the Fletcher-8 checksum now live in the
 * shared ubx.c (also used by the 1425 diagnostics monitor). The Mini keeps
 * its own framing/reassembly (below) and calls ubx_parse_* / ubx_checksum_ok.
 * Shared PVT/SVinfo types come from gnss_view.h so the 1421 NMEA path can feed
 * the same renderer. */

/* Every 64-byte interrupt-IN frame is framed as:
 *   r[0]      = 0x00       (HID Report ID byte prepended by the Windows
 *                            HID stack when libusb reads the endpoint)
 *   r[1]      = status     (0x04 = PLL locked, 0x84 = PLL unlocked;
 *                            bit 7 = "PLL unlock" flag)
 *   r[2..63]  = UBX byte stream (messages span multiple frames; short
 *                                 messages are padded with 0xFF)
 *
 * Reassembly must append ONLY r[2..got-1] -- earlier code that skipped
 * just r[0] was folding the status byte into the UBX buffer, which
 * corrupted every message that crossed a frame boundary. We rely on
 * 0xB5 0x62 sync + length + Fletcher-8 checksum to cleanly recover
 * messages and skip padding / stale bytes. Returns the number of
 * NAV-PVT updates applied so the caller can trigger a redraw. */
struct mini_ubx_stats {
	uint32_t frames_total;
	uint32_t frames_ubx;   /* r[1] bit 7 set -- frame carries UBX bytes */
	uint32_t frames_ka;    /* bit 7 clear -- keepalive / padding, skipped */
	uint32_t parsed;       /* UBX messages passed checksum and dispatched */
	uint32_t ck_fails;     /* Fletcher-8 mismatches */
	uint32_t resyncs;      /* non-padding garbage bytes scanned past */
	uint32_t pad_skips;    /* 0x00 / 0xFF inter-message padding scanned past */
	uint32_t buf_resets;   /* 1 KB reassembly buffer overflow resets */
};

static int mini_consume_ubx(uint8_t *buf, size_t *buf_len, size_t buf_cap,
                            const uint8_t *frame, int got,
                            struct gnss_pvt *pvt, struct gnss_svinfo *sv,
                            struct mini_ubx_stats *st) {
	if (got < 2) return 0;
	if (st) st->frames_total++;
	/* Bit 7 of the status byte marks UBX-data frames. Keepalive /
	 * padding frames have bit 7 clear and r[2..] is 0xFF/0x00 filler --
	 * appending them to the reassembly buffer corrupts messages that
	 * span two UBX frames. Skip them entirely. */
	if ((frame[1] & 0x80) == 0) {
		if (st) st->frames_ka++;
		return 0;
	}
	if (st) st->frames_ubx++;
	if (got < 3) return 0;

	size_t payload = (size_t)(got - 2);
	if (*buf_len + payload > buf_cap) {
		*buf_len = 0;
		if (st) st->buf_resets++;
	}
	memcpy(buf + *buf_len, frame + 2, payload);
	*buf_len += payload;

	int pvt_updates = 0;
	size_t i = 0;
	while (i + 8 <= *buf_len) {
		if (buf[i] != 0xB5 || buf[i+1] != 0x62) {
			if (st) {
				if (buf[i] == 0x00 || buf[i] == 0xFF) st->pad_skips++;
				else                                  st->resyncs++;
			}
			i++;
			continue;
		}
		uint16_t ubx_len = (uint16_t)(buf[i+4] | (buf[i+5] << 8));
		if (ubx_len > 512) {
			i++;
			if (st) st->resyncs++;
			continue;
		}
		size_t total = (size_t)8 + ubx_len;
		if (i + total > *buf_len) break;

		if (!ubx_checksum_ok(&buf[i], total)) {
			i++;
			if (st) { st->ck_fails++; st->resyncs++; }
			continue;
		}

		uint8_t class_ = buf[i+2];
		uint8_t id     = buf[i+3];
		const uint8_t *p = &buf[i+6];

		if (class_ == 0x01 && id == 0x07) {
			ubx_parse_pvt(p, ubx_len, pvt);
			if (pvt->valid) pvt_updates++;
		} else if (class_ == 0x01 && id == 0x35) {
			ubx_parse_sat(p, ubx_len, sv);
		}
		if (st) st->parsed++;
		i += total;
	}
	if (i > 0) {
		memmove(buf, buf + i, *buf_len - i);
		*buf_len -= i;
	}
	return pvt_updates;
}

/* Sample up to ~3 s of interrupt-IN frames for:
 *  (a) the first UBX NAV-PVT fixType (0=no fix, 2=2D, 3=3D; -1 if none)
 *  (b) the Mini's PLL hardware lock state
 *  (c) the GPS signal-present flag
 *  (d) the Mini's u8 "signal loss count"
 *
 * Interrupt-IN frame layout (libusb view, 64 bytes) -- reverse-engineered
 * from `mini GPS clock configuration.exe` (VB6) at 0x00415cad (signal
 * loss), 0x00415d25 (GPS bit), 0x00415e45 (PLL bit). The VB6 code reads
 * its HID buffer at offsets +1 and +2, where buffer[0] is the implicit
 * Report ID 0 prepended by the Windows HID stack. Stripping that prefix
 * maps buffer[1+N] -> our r[N]:
 *
 *   r[0]    : signal loss count (u8, displayed as "Signal loss count: N")
 *   r[1]    : status bitmap
 *             bit 0 (0x01) set  => "No GPS signal"  (red)
 *             bit 1 (0x02) set  => "No PLL lock"    (red)
 *             bit 7 (0x80) set  => this frame carries UBX stream bytes
 *                                  in r[2..]; 0 = keepalive / padding
 *   r[2..63]: UBX byte stream when (r[1] & 0x80), else padding (0xFF/0x00)
 *
 * The PLL/GPS bits in r[1] are valid on every frame, independent of bit
 * 7 -- we sample them unconditionally. */
static void mini_sample_nav(struct lbe_transport *t,
                             int *fix, int *pll_hw_locked,
                             int *gps_signal_ok, uint8_t *signal_loss) {
	*fix = -1;
	*pll_hw_locked = 1;
	*gps_signal_ok = 0;
	*signal_loss = 0;
	struct gnss_pvt pvt = {0};
	struct gnss_svinfo sv = {0};
	uint8_t buf[1024];
	size_t buf_len = 0;

	for (int i = 0; i < 60; i++) {
		uint8_t r[MINI_INPUT_REPORT_SIZE];
		int got = lbe_transport_read_input(t, MINI_INTERRUPT_IN_EP,
		                                    r, sizeof r, 50);
		if (got < 2) continue;
		*signal_loss = r[0];
		*pll_hw_locked  = !(r[1] & 0x02);
		*gps_signal_ok  = !(r[1] & 0x01);
		mini_consume_ubx(buf, &buf_len, sizeof buf, r, got, &pvt, &sv, NULL);
		if (pvt.valid && *fix < 0) *fix = pvt.fix_type;
	}
}

static int mini_monitor(struct lbe_transport *t) {
	struct gnss_pvt    pvt = {0};
	struct gnss_svinfo sv  = {0};
	struct mini_ubx_stats stats = {0};
	uint8_t buf[1024];
	size_t buf_len = 0;

	lbe_enable_vt();
	printf("\033[2J\033[H");
	printf("Waiting for GPS stream...\n");
	fflush(stdout);

	for (;;) {
		uint8_t r[MINI_INPUT_REPORT_SIZE];
		int got = lbe_transport_read_input(t, MINI_INTERRUPT_IN_EP,
		                                    r, sizeof r, 500);
		if (got < 0) break;
		if (got == 0) continue;
		if (mini_consume_ubx(buf, &buf_len, sizeof buf,
		                      r, got, &pvt, &sv, &stats) > 0) {
			gnss_draw("LBE-Mini GPS Monitor", &pvt, &sv, -1);
			printf("\nStream: frames=%u ubx=%u ka=%u msgs=%u "
			       "ck_fail=%u resync=%u pad=%u ovf=%u\033[K\n\033[J",
			       stats.frames_total, stats.frames_ubx, stats.frames_ka,
			       stats.parsed, stats.ck_fails, stats.resyncs,
			       stats.pad_skips, stats.buf_resets);
			fflush(stdout);
		}
	}
	return 0;
}

/* Send UBX-MON-VER poll (class 0x0A, id 0x04, no payload) through the
 * Mini's UBX wrapper opcode, then read the response from the interrupt-IN
 * stream. The firmware adds the B5 62 sync + checksum itself, so we only
 * hand it {class, id, len_lo, len_hi}. Response layout:
 *   [0..29]   swVersion (30 bytes, null-padded ASCII)
 *   [30..39]  hwVersion (10 bytes, null-padded ASCII)
 *   [40..]    zero-or-more 30-byte extension strings
 * Typical u-blox 7/M8 modules append extensions listing PROTVER, GPS/GLO/
 * BDS/GAL constellations enabled, AID/AOP flags, etc. */
static int mini_gps_info(struct lbe_transport *t) {
	static const uint8_t mon_ver_poll[] = {0x0A, 0x04, 0x00, 0x00};
	if (mini_cmd(t, LBE_MINI_UBX_WRAP, mon_ver_poll,
	             sizeof mon_ver_poll) < 0) {
		fprintf(stderr, "Failed to send UBX-MON-VER poll\n");
		return -1;
	}

	uint8_t buf[2048];
	size_t buf_len = 0;
	for (int iter = 0; iter < 200; iter++) {
		uint8_t r[MINI_INPUT_REPORT_SIZE];
		int got = lbe_transport_read_input(t, MINI_INTERRUPT_IN_EP,
		                                    r, sizeof r, 50);
		if (got < 3) continue;
		size_t payload = (size_t)(got - 2);
		if (buf_len + payload > sizeof buf) buf_len = 0;
		memcpy(buf + buf_len, r + 2, payload);
		buf_len += payload;

		size_t i = 0;
		while (i + 8 <= buf_len) {
			if (buf[i] != 0xB5 || buf[i+1] != 0x62) { i++; continue; }
			uint16_t ulen = (uint16_t)(buf[i+4] | (buf[i+5] << 8));
			if (ulen > 1024) { i++; continue; }
			size_t total = (size_t)8 + ulen;
			if (i + total > buf_len) break;
			if (!ubx_checksum_ok(&buf[i], total)) { i++; continue; }

			if (buf[i+2] == 0x0A && buf[i+3] == 0x04 && ulen >= 40) {
				const uint8_t *p = &buf[i+6];
				printf("u-blox GPS module:\n");
				printf("  SW version : %.30s\n", (const char *)p);
				printf("  HW version : %.10s\n", (const char *)(p + 30));
				size_t ext_off = 40;
				int n = 0;
				while (ext_off + 30 <= ulen) {
					printf("  Extension %d: %.30s\n", ++n,
					       (const char *)(p + ext_off));
					ext_off += 30;
				}
				return 0;
			}
			i += total;
		}
		if (i > 0) {
			memmove(buf, buf + i, buf_len - i);
			buf_len -= i;
		}
	}
	fprintf(stderr, "Timed out waiting for UBX-MON-VER response\n");
	return -1;
}

static int mini_set_power_level(struct lbe_transport *t, int output, int low) {
	if (output != 1) {
		fprintf(stderr, "Mini only supports output 1\n");
		return -1;
	}
	/* Forward index: 0=8mA ... 3=32mA (= Si5351C CLK_x DRV field). The
	 * bool --pwr1 API only picks the two extremes: low=1 -> 8 mA,
	 * low=0 -> 32 mA. Use --drive <8|16|24|32> for the middle levels. */
	uint8_t arg = low ? 0x00 : 0x03;
	return mini_cmd(t, LBE_MINI_SET_DRIVE, &arg, 1);
}

static int mini_set_drive_ma(struct lbe_transport *t, unsigned ma) {
	if (ma != 8 && ma != 16 && ma != 24 && ma != 32) {
		fprintf(stderr, "Invalid drive strength: %u mA (allowed: 8, 16, 24, 32)\n", ma);
		return -1;
	}
	uint8_t arg = (uint8_t)((ma / 8u) - 1u);
	return mini_cmd(t, LBE_MINI_SET_DRIVE, &arg, 1);
}

const struct lbe_model_ops lbe_ops_mini = {
	.name               = "Mini",
	.max_freq           = LBE_MINI_MAX_FREQ,
	.init               = mini_init,
	.get_status         = mini_get_status,
	.set_frequency      = mini_set_frequency,
	.set_frequency_temp = mini_set_frequency_temp,
	.set_outputs_enable = mini_set_outputs_enable,
	.blink_leds         = mini_blink_leds,
	.set_pll_mode       = mini_set_pll_mode,
	.set_1pps           = mini_set_1pps,
	.set_power_level    = mini_set_power_level,
	.set_drive_ma       = mini_set_drive_ma,
	.monitor            = mini_monitor,
	.gps_info           = mini_gps_info,
};
