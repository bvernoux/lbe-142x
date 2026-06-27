/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_MODEL_H
#define LBE_MODEL_H

#include <stdint.h>
#include "lbe_device.h"
#include "lbe_transport.h"

/* Each model implements this vtable and exports it as a const
 * symbol. lbe_device.c maps the USB PID to an ops pointer at open
 * time. Unsupported operations for a model (e.g. 1PPS on 1420) may
 * be NULL -- lbe_device.c then returns -1 and an error message
 * without dispatching. */
struct lbe_model_ops {
	const char *name;
	uint32_t max_freq;

	/* Optional device init called once after transport open. */
	int (*init)(struct lbe_transport *t);

	int (*get_status)(struct lbe_transport *t, struct lbe_status *s);
	int (*set_frequency)(struct lbe_transport *t, int output, uint32_t hz);
	int (*set_frequency_temp)(struct lbe_transport *t, int output, uint32_t hz);
	int (*set_outputs_enable)(struct lbe_transport *t, int enable);
	int (*blink_leds)(struct lbe_transport *t);
	int (*set_pll_mode)(struct lbe_transport *t, int fll_mode);
	int (*set_1pps)(struct lbe_transport *t, int enable);
	int (*set_power_level)(struct lbe_transport *t, int output, int low);

	/* Mini only: set OUT1 drive strength to one of 8/16/24/32 mA. NULL
	 * on 1420/1421 which use the bool --pwr1/--pwr2 API instead. */
	int (*set_drive_ma)(struct lbe_transport *t, unsigned ma);

	/* Interactive GPS monitor (Ctrl-C to exit). Mini parses UBX from HID;
	 * 1421/1423/1425 parse NMEA from the CDC port. NULL means "unsupported". */
	int (*monitor)(struct lbe_transport *t);

	/* One-shot UBX-MON-VER poll (u-blox SW/HW version strings). Mini
	 * only; NULL elsewhere. Prints to stdout, returns 0 on success. */
	int (*gps_info)(struct lbe_transport *t);

	/* LBE-1425 only (NULL elsewhere). GNSS constellation enable bitmask,
	 * u-blox dynamic platform model, and NMEA-output enable. */
	int (*set_gnss)(struct lbe_transport *t, uint8_t mask);
	int (*set_dynmodel)(struct lbe_transport *t, uint8_t model);
	int (*set_nmea)(struct lbe_transport *t, int enable);

	/* LBE-1425 only: live UBX diagnostics monitor (NAV-PVT/SAT/CLOCK from
	 * the EP 0x83 stream). NULL elsewhere. Loops until killed. */
	int (*diag)(struct lbe_transport *t);
};

extern const struct lbe_model_ops lbe_ops_1420;
extern const struct lbe_model_ops lbe_ops_1421;
extern const struct lbe_model_ops lbe_ops_1425;
extern const struct lbe_model_ops lbe_ops_mini;

#endif
