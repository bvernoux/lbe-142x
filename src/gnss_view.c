/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "gnss_view.h"

#include <stdio.h>
#include <stdint.h>

char gnss_letter(uint8_t gnss) {
	switch (gnss) {
	case 0: return 'G';
	case 1: return 'S';
	case 2: return 'E';
	case 3: return 'C';
	case 4: return 'I';
	case 5: return 'J';
	case 6: return 'R';
	case 7: return 'N';
	default: return '?';
	}
}

/* Home cursor + erase-to-end-of-line per row. Avoids the full-screen
 * clear flash that `\033[2J` causes at high redraw rates. A final
 * `\033[J` erases anything below the last drawn row. */
#define CLR_EOL "\033[K"

void gnss_draw(const char *title,
               const struct gnss_pvt *pvt,
               const struct gnss_svinfo *sv,
               int frac_ms) {
	static const char *fix_name[] = {"No fix", "DR only", "2D", "3D", "GNSS+DR", "Time"};
	const char *fx = pvt->fix_type < 6 ? fix_name[pvt->fix_type] : "?";

	printf("\033[H");
	printf("%s   (Ctrl-C to exit)" CLR_EOL "\n", title);
	printf("========================================" CLR_EOL "\n" CLR_EOL "\n");
	if (!pvt->time_valid) {
		printf("UTC:  ---------- --:--:-- (no valid time)" CLR_EOL "\n");
	} else if (frac_ms >= 0 && frac_ms <= 999) {
		printf("UTC:  %04u-%02u-%02u %02u:%02u:%02u.%03d" CLR_EOL "\n",
		       pvt->year, pvt->month, pvt->day,
		       pvt->hour, pvt->min, pvt->sec, frac_ms);
	} else {
		printf("UTC:  %04u-%02u-%02u %02u:%02u:%02u" CLR_EOL "\n",
		       pvt->year, pvt->month, pvt->day,
		       pvt->hour, pvt->min, pvt->sec);
	}
	printf("Lat:  %+14.7f deg" CLR_EOL "\n", pvt->lat_1e7 * 1e-7);
	printf("Lon:  %+14.7f deg" CLR_EOL "\n", pvt->lon_1e7 * 1e-7);
	printf("Alt:  %+10.3f m (MSL)" CLR_EOL "\n", pvt->hmsl_mm * 1e-3);
	printf("Fix:  %-8s  Sats used: %u" CLR_EOL "\n" CLR_EOL "\n",
	       fx, pvt->num_sv);

	if (sv->valid) {
		printf("Satellites  (G=GPS R=GLO E=Gal C=BDS S=SBAS J=QZSS, "
		       "bar = 2 dB, * = used):" CLR_EOL "\n");
		for (uint8_t i = 0; i < sv->num_ch; i++) {
			uint8_t cno = sv->sats[i].cno;
			if (cno == 0 && !sv->sats[i].used) continue;
			int bars = cno / 2;
			if (bars > 25) bars = 25;
			printf("  %c%-3u  ", gnss_letter(sv->sats[i].gnss_id),
			       sv->sats[i].svid);
			for (int b = 0; b < bars; b++) putchar('#');
			for (int b = bars; b < 25; b++) putchar(' ');
			printf("  %2u dB  %c" CLR_EOL "\n", cno,
			       sv->sats[i].used ? '*' : ' ');
		}
	} else {
		printf("Waiting for satellite data..." CLR_EOL "\n");
	}
}
