/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "lbe_device.h"
#include "lbe_common.h"
#include "lbe_transport.h"
#include "lbe_model.h"

#include <stdio.h>
#include <stdlib.h>

struct lbe_device {
	struct lbe_transport *transport;
	enum lbe_model model;
	uint16_t pid;
	const struct lbe_model_ops *ops;
};

struct lbe_device *lbe_open_device(uint16_t preferred_pid) {
	static const uint16_t pids[] = {
		PID_LBE_1420, PID_LBE_1421, PID_LBE_1423, PID_LBE_1425,
		PID_LBE_MINI, 0
	};
	uint16_t pid = 0;
	struct lbe_transport *t = lbe_transport_open(VID_LBE, pids,
	                                              preferred_pid, &pid);
	if (!t) return NULL;  /* transport has already printed a reason */

	struct lbe_device *dev = calloc(1, sizeof *dev);
	if (!dev) { lbe_transport_close(t); return NULL; }
	dev->transport = t;
	dev->pid = pid;

	switch (pid) {
	case PID_LBE_1420:
		dev->model = LBE_1420;
		dev->ops = &lbe_ops_1420;
		break;
	case PID_LBE_MINI:
		dev->model = LBE_MINI;
		dev->ops = &lbe_ops_mini;
		break;
	case PID_LBE_1425:
		/* 1425 = the 1421 dual-output protocol plus GNSS / dynamic-model
		 * / NMEA-output commands (lbe_ops_1425). Per-output frequency
		 * caps are enforced in lbe_max_freq() via the PID. See
		 * docs/reverse/LBE-1425-config-v1.10.md. */
		dev->model = LBE_1421_DUALOUT;
		dev->ops = &lbe_ops_1425;
		break;
	case PID_LBE_1423:
	case PID_LBE_1421:
	default:
		/* 1423 shares the 1421 wire format until we capture evidence
		 * of a difference. */
		dev->model = LBE_1421_DUALOUT;
		dev->ops = &lbe_ops_1421;
		break;
	}

	if (dev->ops->init) dev->ops->init(dev->transport);
	return dev;
}

void lbe_close_device(struct lbe_device *dev) {
	if (!dev) return;
	lbe_transport_close(dev->transport);
	free(dev);
}

enum lbe_model lbe_get_model(struct lbe_device *dev) {
	return dev->model;
}

uint16_t lbe_get_pid(struct lbe_device *dev) {
	return dev->pid;
}

int lbe_get_serial(struct lbe_device *dev, char *out, size_t n) {
	return lbe_transport_serial(dev->transport, out, n);
}

uint32_t lbe_max_freq(struct lbe_device *dev, int output) {
	/* The 1425 is the only model with asymmetric per-output limits; every
	 * other model is symmetric, so fall back to the ops vtable's max_freq. */
	if (dev->pid == PID_LBE_1425)
		return output == 1 ? LBE_1425_OUT1_MAX_FREQ : LBE_1425_OUT2_MAX_FREQ;
	return dev->ops->max_freq;
}

int lbe_get_device_status(struct lbe_device *dev, struct lbe_status *s) {
	return dev->ops->get_status(dev->transport, s);
}

int lbe_set_frequency(struct lbe_device *dev, int output, uint32_t hz) {
	return dev->ops->set_frequency(dev->transport, output, hz);
}

int lbe_set_frequency_temp(struct lbe_device *dev, int output, uint32_t hz) {
	return dev->ops->set_frequency_temp(dev->transport, output, hz);
}

int lbe_set_outputs_enable(struct lbe_device *dev, int enable) {
	return dev->ops->set_outputs_enable(dev->transport, enable);
}

int lbe_blink_leds(struct lbe_device *dev) {
	return dev->ops->blink_leds(dev->transport);
}

int lbe_set_pll_mode(struct lbe_device *dev, int fll_mode) {
	return dev->ops->set_pll_mode(dev->transport, fll_mode);
}

int lbe_set_1pps(struct lbe_device *dev, int enable) {
	return dev->ops->set_1pps(dev->transport, enable);
}

int lbe_set_power_level(struct lbe_device *dev, int output, int low) {
	return dev->ops->set_power_level(dev->transport, output, low);
}

int lbe_set_drive_ma(struct lbe_device *dev, unsigned ma) {
	if (!dev->ops->set_drive_ma) {
		fprintf(stderr, "--drive is not supported on this model\n");
		return -1;
	}
	return dev->ops->set_drive_ma(dev->transport, ma);
}

int lbe_set_gnss(struct lbe_device *dev, uint8_t mask) {
	if (!dev->ops->set_gnss) {
		fprintf(stderr, "--gnss is not supported on this model\n");
		return -1;
	}
	/* BeiDou is mutually exclusive with the GPS/SBAS/Galileo group. */
	uint8_t group = LBE_1425_GNSS_GPS | LBE_1425_GNSS_SBAS | LBE_1425_GNSS_GALILEO;
	if ((mask & LBE_1425_GNSS_BEIDOU) && (mask & group)) {
		fprintf(stderr, "Invalid GNSS mask 0x%02X: BeiDou (0x08) cannot be "
		        "combined with GPS/SBAS/Galileo\n", mask);
		return -1;
	}
	return dev->ops->set_gnss(dev->transport, mask);
}

int lbe_set_dynmodel(struct lbe_device *dev, uint8_t model) {
	if (!dev->ops->set_dynmodel) {
		fprintf(stderr, "--dynmodel is not supported on this model\n");
		return -1;
	}
	return dev->ops->set_dynmodel(dev->transport, model);
}

int lbe_set_nmea(struct lbe_device *dev, int enable) {
	if (!dev->ops->set_nmea) {
		fprintf(stderr, "--nmea is not supported on this model\n");
		return -1;
	}
	return dev->ops->set_nmea(dev->transport, enable);
}

int lbe_monitor(struct lbe_device *dev) {
	if (!dev->ops->monitor) {
		fprintf(stderr, "--monitor is not supported on this model\n");
		return -1;
	}
	return dev->ops->monitor(dev->transport);
}

int lbe_diag(struct lbe_device *dev) {
	if (!dev->ops->diag) {
		fprintf(stderr, "--diag is not supported on this model\n");
		return -1;
	}
	return dev->ops->diag(dev->transport);
}

int lbe_gps_info(struct lbe_device *dev) {
	if (!dev->ops->gps_info) {
		fprintf(stderr, "--gps-info is not supported on this model\n");
		return -1;
	}
	return dev->ops->gps_info(dev->transport);
}

int lbe_rawdump(struct lbe_device *dev, uint8_t ep, int duration_ms) {
	lbe_transport_claim(dev->transport);
	int elapsed = 0;
	int frames = 0;
	int total_bytes = 0;
	printf("Dumping interrupt-IN from EP 0x%02X for %d ms...\n",
	       ep, duration_ms);
	while (elapsed < duration_ms) {
		uint8_t buf[64];
		int got = lbe_transport_read_input(dev->transport, ep, buf,
		                                    sizeof buf, 200);
		elapsed += 200;
		if (got <= 0) continue;
		frames++;
		total_bytes += got;
		printf("[%4d] ", frames);
		for (int i = 0; i < got && i < 32; i++) printf("%02X ", buf[i]);
		if (got > 32) printf("... ");
		printf("| ");
		for (int i = 0; i < got && i < 32; i++) {
			unsigned char c = buf[i];
			putchar((c >= 0x20 && c < 0x7F) ? (char)c : '.');
		}
		printf("\n");
	}
	printf("Total: %d frames, %d bytes in %d ms\n",
	       frames, total_bytes, duration_ms);
	return 0;
}
