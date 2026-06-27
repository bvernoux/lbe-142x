/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_common.h"
#include "lbe_model.h"
#include "lbe_transport.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Legacy convention used by all LBE firmware revisions we have seen
 * on Linux: the opcode doubles as the HID Report ID. SET_REPORT goes
 * with wValue=(0x03<<8 | opcode) and USB payload = {opcode, args, ...}.
 * Status reads use the dedicated status Report ID 0x4B. */
#define LBE_STATUS_REPORT_ID 0x4B

static int send_cmd(struct lbe_transport *t, uint8_t opcode,
                    const uint8_t *args, size_t n) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	buf[0] = opcode;
	if (args && n) {
		size_t room = LBE_REPORT_SIZE - 1;
		memcpy(&buf[1], args, n > room ? room : n);
	}
	return lbe_transport_feat_set(t, opcode, buf);
}

static int m1420_get_status(struct lbe_transport *t, struct lbe_status *s) {
	uint8_t buf[LBE_REPORT_SIZE] = {0};
	if (lbe_transport_feat_get(t, LBE_STATUS_REPORT_ID, buf) < 0) return -1;

	/* buf[0] is the Report ID echo (0x4B); the firmware's real fields
	 * start at buf[1]. */
	s->raw_status     = buf[1];
	s->frequency1     = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
	s->frequency2     = 0;
	s->fll_enabled    = buf[18] != 0;
	s->out1_power_low = buf[10] != 0;
	s->out2_power_low = 0;
	s->out1_drive_ma  = 0;
	s->pll_locked     = (s->raw_status & LBE_PLL_LOCK_BIT) != 0;
	s->antenna_ok     = (s->raw_status & LBE_ANT_OK_BIT) != 0;
	s->pps_enabled    = 0;
	/* Legacy behaviour: 1420 firmware does not mirror the outputs-
	 * enable bit back in the feature report, so callers always see
	 * "enabled". */
	s->outputs_enabled = 1;
	return 0;
}

static int m1420_set_frequency(struct lbe_transport *t, int output, uint32_t hz) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	return send_cmd(t, LBE_1420_SET_F1, args, sizeof args);
}

static int m1420_set_frequency_temp(struct lbe_transport *t, int output, uint32_t hz) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t args[4] = {
		(uint8_t)(hz),
		(uint8_t)(hz >> 8),
		(uint8_t)(hz >> 16),
		(uint8_t)(hz >> 24),
	};
	return send_cmd(t, LBE_1420_SET_F1_TEMP, args, sizeof args);
}

static int m1420_set_outputs_enable(struct lbe_transport *t, int enable) {
	uint8_t arg = enable ? 0x01 : 0x00;
	return send_cmd(t, LBE_142X_EN_OUT, &arg, 1);
}

static int m1420_blink_leds(struct lbe_transport *t) {
	return send_cmd(t, LBE_142X_BLINK_OUT, NULL, 0);
}

static int m1420_set_pll_mode(struct lbe_transport *t, int fll_mode) {
	uint8_t arg = fll_mode ? 0x01 : 0x00;
	return send_cmd(t, LBE_142X_SET_PLL, &arg, 1);
}

static int m1420_set_1pps(struct lbe_transport *t, int enable) {
	(void)t; (void)enable;
	fprintf(stderr, "1PPS control is only supported on LBE-1421/1423/1425\n");
	return -1;
}

static int m1420_set_power_level(struct lbe_transport *t, int output, int low) {
	if (output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}
	uint8_t arg = low ? 0x01 : 0x00;
	return send_cmd(t, LBE_1420_SET_PWR1, &arg, 1);
}

const struct lbe_model_ops lbe_ops_1420 = {
	.name               = "1420",
	.max_freq           = LBE_1420_MAX_FREQ,
	.init               = NULL,
	.get_status         = m1420_get_status,
	.set_frequency      = m1420_set_frequency,
	.set_frequency_temp = m1420_set_frequency_temp,
	.set_outputs_enable = m1420_set_outputs_enable,
	.blink_leds         = m1420_blink_leds,
	.set_pll_mode       = m1420_set_pll_mode,
	.set_1pps           = m1420_set_1pps,
	.set_power_level    = m1420_set_power_level,
};
