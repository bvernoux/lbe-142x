/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#include "nmea.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void nmea_state_init(struct nmea_state *s) {
	memset(s, 0, sizeof *s);
}

int nmea_checksum_ok(const char *line) {
	if (!line || line[0] != '$') return 0;
	uint8_t ck = 0;
	const char *p = line + 1;
	while (*p && *p != '*') { ck ^= (uint8_t)*p; p++; }
	if (*p != '*' || !p[1] || !p[2]) return 0;
	char hex[3] = {p[1], p[2], 0};
	unsigned expected = (unsigned)strtoul(hex, NULL, 16);
	return expected == ck;
}

/* Split a NMEA body on commas into up to max_fields pointers. Returns
 * the number of fields. Modifies `buf` in place (inserts NULs). The
 * leading `$` and trailing `*CS` are expected to already be stripped. */
static int split_fields(char *buf, char **fields, int max_fields) {
	int n = 0;
	char *p = buf;
	fields[n++] = p;
	while (*p && n < max_fields) {
		if (*p == ',') {
			*p = '\0';
			fields[n++] = p + 1;
		}
		p++;
	}
	return n;
}

static uint8_t talker_to_gnss_id(const char *talker) {
	if (talker[0] == 'G' && talker[1] == 'P') return 0;  /* GPS */
	if (talker[0] == 'G' && talker[1] == 'L') return 6;  /* GLONASS */
	if (talker[0] == 'G' && talker[1] == 'A') return 2;  /* Galileo */
	if (talker[0] == 'G' && talker[1] == 'B') return 3;  /* BeiDou */
	if (talker[0] == 'B' && talker[1] == 'D') return 3;  /* BeiDou alt */
	if (talker[0] == 'G' && talker[1] == 'Q') return 5;  /* QZSS */
	if (talker[0] == 'G' && talker[1] == 'N') return 0;  /* mixed GNSS -> default to GPS bin */
	return 0;
}

/* Parse NMEA lat/lon: "DDMM.MMMM" + hemisphere -> int32_t * 1e7. */
static int32_t parse_lat_lon(const char *raw, const char *hemi) {
	if (!raw || !raw[0]) return 0;
	double val = atof(raw);
	double degs = (int)(val / 100);
	double mins = val - degs * 100;
	double result = degs + mins / 60.0;
	if (hemi && (hemi[0] == 'S' || hemi[0] == 'W')) result = -result;
	return (int32_t)(result * 1.0e7);
}

/* Parse hhmmss(.sss) time into hour/min/sec. */
static void parse_utc(const char *raw, uint8_t *h, uint8_t *m, uint8_t *s) {
	if (!raw || strlen(raw) < 6) return;
	char buf[3] = {0};
	buf[0] = raw[0]; buf[1] = raw[1]; *h = (uint8_t)atoi(buf);
	buf[0] = raw[2]; buf[1] = raw[3]; *m = (uint8_t)atoi(buf);
	buf[0] = raw[4]; buf[1] = raw[5]; *s = (uint8_t)atoi(buf);
}

/* Parse ddmmyy date. */
static void parse_date(const char *raw, uint16_t *y, uint8_t *mo, uint8_t *d) {
	if (!raw || strlen(raw) < 6) return;
	char buf[3] = {0};
	buf[0] = raw[0]; buf[1] = raw[1]; *d  = (uint8_t)atoi(buf);
	buf[0] = raw[2]; buf[1] = raw[3]; *mo = (uint8_t)atoi(buf);
	buf[0] = raw[4]; buf[1] = raw[5]; *y  = (uint16_t)(2000 + atoi(buf));
}

static int handle_gga(char **f, int nf, struct gnss_pvt *pvt) {
	if (nf < 10) return 0;
	parse_utc(f[1], &pvt->hour, &pvt->min, &pvt->sec);
	pvt->lat_1e7 = parse_lat_lon(f[2], f[3]);
	pvt->lon_1e7 = parse_lat_lon(f[4], f[5]);
	int quality = atoi(f[6]);
	pvt->fix_type = (uint8_t)(quality > 0 ? 3 : 0);
	pvt->num_sv   = (uint8_t)atoi(f[7]);
	pvt->hmsl_mm  = (int32_t)(atof(f[9]) * 1000.0);
	/* GGA arrives every cycle, so it owns clearing time_valid on fix loss;
	 * RMC (which carries the date) confirms it. */
	pvt->time_valid = (quality > 0);
	pvt->valid    = 1;
	return NMEA_GOT_PVT;
}

static int handle_rmc(char **f, int nf, struct gnss_pvt *pvt) {
	if (nf < 10) return 0;
	if (f[2][0] == 'V') return 0;  /* void/invalid fix */
	parse_utc(f[1], &pvt->hour, &pvt->min, &pvt->sec);
	pvt->lat_1e7 = parse_lat_lon(f[3], f[4]);
	pvt->lon_1e7 = parse_lat_lon(f[5], f[6]);
	parse_date(f[9], &pvt->year, &pvt->month, &pvt->day);
	pvt->time_valid = 1;   /* status 'A' -> date+time trustworthy */
	pvt->valid = 1;
	return NMEA_GOT_PVT;
}

/* Remove all SVs with this gnss_id from the working buffer. */
static void purge_gnss(struct nmea_state *s, uint8_t gnss_id) {
	uint8_t w = 0;
	for (uint8_t i = 0; i < s->work_count; i++) {
		if (s->work[i].gnss_id != gnss_id) {
			if (w != i) s->work[w] = s->work[i];
			w++;
		}
	}
	s->work_count = w;
}

static int handle_gsa(char **f, int nf, struct gnss_svinfo *sv) {
	/* $xxGSA,A,3,<PRN1>,<PRN2>,...,<PRN12>,<PDOP>,<HDOP>,<VDOP>*CS
	 * Fields 3..14 are the PRNs currently used in the fix. */
	if (nf < 15) return 0;
	/* Build a set of used PRNs, then mark matching SVs. Note GSA uses
	 * NMEA PRN numbers, not native GNSS slots. GLONASS is offset +64
	 * in NMEA (so GLONASS slot 1 -> PRN 65). This is a coarse match
	 * that assigns "used" on GPS PRNs 1..32 and relies on the GPS-only
	 * stream. If the 1421 ever emits multi-constellation GSA, a more
	 * careful mapping is needed. */
	uint8_t used_prns[12];
	int used_count = 0;
	for (int i = 0; i < 12; i++) {
		if (f[3 + i][0]) used_prns[used_count++] = (uint8_t)atoi(f[3 + i]);
	}
	/* Reset and re-mark. */
	for (uint8_t i = 0; i < sv->num_ch; i++) sv->sats[i].used = 0;
	for (uint8_t i = 0; i < sv->num_ch; i++) {
		for (int k = 0; k < used_count; k++) {
			if (sv->sats[i].svid == used_prns[k] &&
			    sv->sats[i].gnss_id == 0) {  /* GPS only for now */
				sv->sats[i].used = 1;
				break;
			}
		}
	}
	return NMEA_GOT_SV;
}

static int handle_gsv(struct nmea_state *s, const char *talker,
                      char **f, int nf, struct gnss_svinfo *sv) {
	if (nf < 4) return 0;
	uint8_t gnss_id = talker_to_gnss_id(talker);
	if (gnss_id > 7) return 0;

	int total_msgs = atoi(f[1]);
	int msg_num    = atoi(f[2]);
	/* int sv_count   = atoi(f[3]); */

	if (msg_num == 1) {
		purge_gnss(s, gnss_id);
		s->expect_msg[gnss_id] = 1;
		s->total_msg[gnss_id]  = (uint8_t)total_msgs;
	}
	if (msg_num != s->expect_msg[gnss_id]) {
		/* out of order; reset and wait for next msg 1 */
		s->expect_msg[gnss_id] = 0;
		return 0;
	}
	s->expect_msg[gnss_id] = (uint8_t)(msg_num + 1);

	for (int i = 0; i < 4; i++) {
		int base = 4 + i * 4;
		if (base + 3 >= nf) break;
		if (!f[base][0]) continue;  /* blank SV slot */
		if (s->work_count >= 64) break;
		struct gnss_sv *e = &s->work[s->work_count++];
		e->gnss_id = gnss_id;
		e->svid    = (uint8_t)atoi(f[base]);
		e->elev    = (int8_t)atoi(f[base + 1]);
		e->azim    = (int16_t)atoi(f[base + 2]);
		e->cno     = (uint8_t)atoi(f[base + 3]);
		e->used    = 0;  /* GSV doesn't tell us; GGA gives sats-used count */
	}

	if (msg_num == total_msgs) {
		/* Commit working buffer to output. */
		for (uint8_t i = 0; i < s->work_count; i++) sv->sats[i] = s->work[i];
		sv->num_ch = s->work_count;
		sv->valid  = 1;
		return NMEA_GOT_SV;
	}
	return 0;
}

int nmea_feed(struct nmea_state *s, const char *line,
              struct gnss_pvt *pvt, struct gnss_svinfo *sv) {
	if (!line || line[0] != '$') return 0;
	/* Tolerate missing/wrong checksum — u-blox streams are well-formed
	 * but cheap USB-CDC bridges sometimes drop the `*CS` on boundary
	 * frames. Skip only if we can see a `*` but it's wrong. */
	const char *star = strchr(line, '*');
	if (star && !nmea_checksum_ok(line)) return 0;

	char body[256];
	/* strip leading '$' and trailing `*CS` */
	size_t len = strlen(line + 1);
	if (star) len = (size_t)(star - (line + 1));
	if (len >= sizeof body) return 0;
	memcpy(body, line + 1, len);
	body[len] = '\0';

	char *fields[32];
	int nf = split_fields(body, fields, 32);
	if (nf < 1 || strlen(fields[0]) < 5) return 0;

	const char *sentence = fields[0] + 2;  /* skip talker ID */
	const char *talker   = fields[0];      /* "GP", "GN", "GL", ... */

	if (strcmp(sentence, "GGA") == 0) return handle_gga(fields, nf, pvt);
	if (strcmp(sentence, "RMC") == 0) return handle_rmc(fields, nf, pvt);
	if (strcmp(sentence, "GSA") == 0) return handle_gsa(fields, nf, sv);
	if (strcmp(sentence, "GSV") == 0) return handle_gsv(s, talker, fields, nf, sv);
	return 0;
}
