/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef GNSS_VIEW_H
#define GNSS_VIEW_H

#include <stdint.h>

/* PVT (position/velocity/time) snapshot. Populated from UBX NAV-PVT on
 * the Mini, or from NMEA GGA/RMC on the 1421/1423/1425. */
struct gnss_pvt {
	int      valid;
	uint16_t year;
	uint8_t  month, day, hour, min, sec;
	uint8_t  fix_type;    /* 0=no fix, 2=2D, 3=3D (same as UBX codes) */
	uint8_t  time_valid;  /* 1 if the UTC date/time fields are trustworthy */
	uint8_t  num_sv;
	int32_t  lon_1e7;
	int32_t  lat_1e7;
	int32_t  hmsl_mm;
};

struct gnss_sv {
	uint8_t gnss_id;   /* 0=GPS, 1=SBAS, 2=Gal, 3=BDS, 5=QZSS, 6=GLO, 7=NavIC */
	uint8_t svid;
	uint8_t cno;       /* dB-Hz */
	int8_t  elev;      /* deg */
	int16_t azim;      /* deg */
	uint8_t used;      /* 1 if used in nav solution */
};

struct gnss_svinfo {
	int     valid;
	uint8_t num_ch;
	struct gnss_sv sats[64];
};

/* Single-char label per constellation: G/S/E/C/I/J/R/N. */
char gnss_letter(uint8_t gnss_id);

/* Paint a full monitor screen. `title` is shown in the banner. If
 * `frac_ms` is in [0, 999] it is appended to the UTC line as ".fff"
 * for a sub-second display; pass -1 to omit. */
void gnss_draw(const char *title,
               const struct gnss_pvt *pvt,
               const struct gnss_svinfo *sv,
               int frac_ms);

#endif
