#ifdef __linux__

#include "lbe_device.h"
#include "lbe_common.h"
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/select.h>

#define REPORT_SIZE 60

#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#define HIDIOCGFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)
#endif

struct lbe_device {
	int fd;
	struct hidraw_devinfo raw_info;
	enum lbe_model model;
};

static int is_lbe_device(const char *path) {
	int fd;
	struct hidraw_devinfo info;

	fd = open(path, O_RDWR);
	if (fd < 0) return 0;

	if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0) {
		close(fd);
		return 0;
	}

	close(fd);
	return (info.vendor == VID_LBE &&
	        (info.product == PID_LBE_1420 ||
	         info.product == PID_LBE_1421 ||
	         info.product == PID_LBE_1423 ||
	         info.product == PID_LBE_MINI));
}

/* Mini speaks the same HID wire format as the vendor Windows tool:
 * wValue must be 0x0300 with the opcode in the payload. Linux hidraw
 * puts buf[0] into wValue's low byte, so buf[0] stays 0 and the
 * opcode goes at buf[1]. Required for opcodes 0x08 and 0x0A; other
 * opcodes also work under the legacy buf[0]=opcode path but are sent
 * this way for consistency. */
static int mini_set_feat(int fd, uint8_t op, const uint8_t *args, size_t n) {
	uint8_t buf[REPORT_SIZE] = {0};
	buf[1] = op;
	if (args && n) memcpy(&buf[2], args, n);
	if (ioctl(fd, HIDIOCSFEATURE(REPORT_SIZE), buf) < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}
	return 0;
}

/* Solve the Si5351-style PLL divider chain for a given target output
 * frequency. Formula from the Leo Bodnar hardware description and the
 * hamarituc/lbgpsdo reference: f_out = fin * N2_HS * N2_LS /
 * (N3 * N1_HS * NC1_LS). Device constraints: N2_HS, N1_HS in [4, 11];
 * N2_LS, NC1_LS in [2, 2^20] and even, or exactly 1 for NC1_LS; N3 in
 * [1, 2^19]. Fin is fixed at 97,600 Hz here, matching the vendor tool's
 * default; that value produces clean integer solutions for 10 MHz and
 * most multiples of common LO frequencies. Returns 0 on success, -1
 * if no valid solution was found. */
static int mini_solve_pll(uint32_t f_out,
                          uint32_t *fin_out, uint32_t *n3_out,
                          uint32_t *n2hs_out, uint32_t *n2ls_out,
                          uint32_t *n1hs_out, uint32_t *nc1_out)
{
	const uint32_t f_in = 97600;
	uint64_t a = f_out, b = f_in;
	while (b) { uint64_t t = b; b = a % b; a = t; }
	uint64_t p = f_out / a;
	uint64_t q = f_in / a;

	/* Two passes: first prefer a VCO frequency near the values seen
	 * on the wire (5 to 6.5 GHz); if no exact divider fit exists in
	 * that band, accept any valid solution. */
	for (int pass = 0; pass < 2; pass++) {
		for (uint32_t k = 1; k <= 4096; k++) {
			uint64_t M = (uint64_t)k * p;
			uint64_t D = (uint64_t)k * q;
			if (M > (uint64_t)11 * (1ULL << 20)) break;
			if (D > (uint64_t)11 * (1ULL << 20) * (1ULL << 19)) break;

			uint64_t f_osc = (uint64_t)f_in * M;
			if (pass == 0 && (f_osc < 5000000000ULL || f_osc > 6500000000ULL))
				continue;

			for (int nh = 11; nh >= 4; nh--) {
				if (M % nh) continue;
				uint64_t n2ls = M / nh;
				if (n2ls < 2 || n2ls > (1ULL << 20) || (n2ls & 1)) continue;

				for (int nh1 = 11; nh1 >= 4; nh1--) {
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

/* The Mini boots with only u-blox CFG-ACKs in its input-report buffer.
 * Three SET_REPORTs with opcode 0x08 wrap UBX-CFG-MSG writes that
 * enable NAV-SVINFO, NAV-CLOCK and NAV-PVT forwarding. Opcode 0x0A
 * must precede them; it also leaves the next two feature reads
 * returning descriptor bytes, which we drain. Captured from the
 * vendor tool pcap. */
static void mini_enable_gps_stream(int fd) {
	static const uint8_t svinfo[] = {0x06, 0x01, 0x08, 0x00, 0x01, 0x30, 0x14};
	static const uint8_t clock_[] = {0x06, 0x01, 0x08, 0x00, 0x01, 0x22, 0x14};
	static const uint8_t pvt[]    = {0x06, 0x01, 0x08, 0x00, 0x01, 0x07, 0x0A};
	uint8_t refresh[] = {0x04};
	uint8_t drain[REPORT_SIZE];
	mini_set_feat(fd, 0x0A, refresh, sizeof refresh);
	drain[0] = 0x4B; (void)ioctl(fd, HIDIOCGFEATURE(REPORT_SIZE), drain);
	drain[0] = 0x4B; (void)ioctl(fd, HIDIOCGFEATURE(REPORT_SIZE), drain);
	mini_set_feat(fd, 0x08, svinfo, sizeof svinfo);
	mini_set_feat(fd, 0x08, clock_, sizeof clock_);
	mini_set_feat(fd, 0x08, pvt, sizeof pvt);
}

/* Scan up to ~1s of input reports and report what was seen.
 * The firmware alternates two input-report variants:
 *   - Status variant: byte[1] bit 7 = 0, byte[2] holds GPS/PLL flags.
 *   - UBX variant:    byte[1] bit 7 = 1, byte[2..] is raw u-blox bytes,
 *                     occasionally carrying UBX-NAV-PVT.
 * From the status variant we read bit 1 of byte[2] as the real GPS
 * disciplined PLL lock. From the UBX variant we pick up fixType for
 * GPS lock. Both fields are returned via pointers; each stays at its
 * "unknown" sentinel if that variant was not seen in the window. */
static void mini_read_input_state(int fd, int *pvt_fix, int *pll_gps_locked) {
	*pvt_fix = -1;
	*pll_gps_locked = -1;
	for (int i = 0; i < 300; i++) {
		struct timeval tv = { 0, 50 * 1000 };
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
		uint8_t r[64];
		if (read(fd, r, sizeof r) != (ssize_t)sizeof r) continue;
		if ((r[1] & 0x80) == 0) {
			if (*pll_gps_locked < 0)
				*pll_gps_locked = (r[2] & 0x02) ? 1 : 0;
		} else if (r[2] == 0xB5 && r[3] == 0x62 &&
		           r[4] == 0x01 && r[5] == 0x07) {
			if (*pvt_fix < 0) *pvt_fix = r[28];
		}
		if (*pvt_fix >= 0 && *pll_gps_locked >= 0) return;
	}
}

struct lbe_device* lbe_open_device(void) {
	struct lbe_device* dev = malloc(sizeof(struct lbe_device));
	if (!dev) return NULL;

	DIR *dir;
	struct dirent *ent;
	char path[PATH_MAX];

	dir = opendir("/dev");
	if (dir == NULL) {
		perror("Failed to open /dev");
		free(dev);
		return NULL;
	}

	while ((ent = readdir(dir)) != NULL) {
		if (strncmp(ent->d_name, "hidraw", 6) == 0) {
			snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
			if (is_lbe_device(path)) {
				dev->fd = open(path, O_RDWR);
				if (dev->fd < 0) {
					perror("Failed to open device");
					free(dev);
					closedir(dir);
					return NULL;
				}
				if (ioctl(dev->fd, HIDIOCGRAWINFO, &dev->raw_info) < 0) {
					perror("HIDIOCGRAWINFO");
					close(dev->fd);
					free(dev);
					closedir(dir);
					return NULL;
				}
				if (dev->raw_info.product == PID_LBE_1420)
					dev->model = LBE_1420;
				else if (dev->raw_info.product == PID_LBE_MINI)
					dev->model = LBE_MINI;
				else
					dev->model = LBE_1421_DUALOUT;
				if (dev->model == LBE_MINI)
					mini_enable_gps_stream(dev->fd);
				closedir(dir);
				return dev;
			}
		}
	}

	fprintf(stderr, "LBE-142x device not found\n");
	closedir(dir);
	free(dev);
	return NULL;
}

void lbe_close_device(struct lbe_device* dev) {
	if (dev) {
		close(dev->fd);
		free(dev);
	}
}

enum lbe_model lbe_get_model(struct lbe_device* dev) {
	return dev->model;
}

int lbe_get_device_status(struct lbe_device* dev, struct lbe_status* status) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	buf[0] = 0x4B; // Report Number
	res = ioctl(dev->fd, HIDIOCGFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCGFEATURE");
		return -1;
	}

	status->raw_status = buf[1];
	if (dev->model == LBE_MINI) {
		/* The feature report mirrors the PLL programming frame. Decode
		 * fin, dividers, and compute f_out = fin * N2_HS * N2_LS /
		 * (N3 * N1_HS * NC1_LS). */
		uint32_t fin  = buf[2] | (buf[3] << 8) | (buf[4] << 16);
		uint32_t n3   = (buf[5] | (buf[6] << 8) | (buf[7] << 16)) + 1;
		uint32_t n2hs = buf[8] + 4;
		uint32_t n2ls = (buf[9] | (buf[10] << 8) | (buf[11] << 16)) + 1;
		uint32_t n1hs = buf[12] + 4;
		uint32_t nc1  = (buf[13] | (buf[14] << 8) | (buf[15] << 16)) + 1;
		uint64_t den = (uint64_t)n3 * n1hs * nc1;
		status->frequency1 = den ?
		    (uint32_t)(((uint64_t)fin * n2hs * n2ls) / den) : 0;
		status->frequency2 = 0;
		/* Feature report bit 1 tracks the internal output PLL, not
		 * the GPS disciplined PLL that the 1420/1421 bit represents.
		 * Pull both real flags from the input report so Mini uses the
		 * same semantics as the other models. */
		int fix = -1, pll = -1;
		mini_read_input_state(dev->fd, &fix, &pll);
		if (fix >= 2) status->raw_status |= LBE_GPS_LOCK_BIT;
		else          status->raw_status &= ~LBE_GPS_LOCK_BIT;
		if (pll == 1) status->raw_status |= LBE_PLL_LOCK_BIT;
		else if (pll == 0) status->raw_status &= ~LBE_PLL_LOCK_BIT;
	} else if (dev->model == LBE_1420) {
		status->frequency1 = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
		status->frequency2 = 0;
	} else { // LBE_1421
		status->frequency1 = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
		status->frequency2 = buf[14] | (buf[15] << 8) | (buf[16] << 16) | (buf[17] << 24);
	}
	status->outputs_enabled = (status->raw_status & (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT)) == (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT);
	status->fll_enabled = buf[18] != 0;
	status->pll_locked = (status->raw_status & LBE_PLL_LOCK_BIT) != 0;

	// Additional status information for LBE-1421
	if (dev->model == LBE_1421_DUALOUT) {
		status->antenna_ok = (status->raw_status & LBE_ANT_OK_BIT) != 0;
		status->pps_enabled = (status->raw_status & LBE_PPS_EN_BIT) != 0;
		status->out1_power_low = buf[19] != 0;
		status->out2_power_low = buf[20] != 0;
	} else {
		status->antenna_ok = (status->raw_status & LBE_ANT_OK_BIT) != 0;
		status->pps_enabled = 0;
		status->out1_power_low = buf[10] != 0;
		status->out2_power_low = 0;
		status->outputs_enabled = 1; // Seems to be always on even with legit soft
	}

	/*printf("Raw report dump:\n");
	for (int i = 0; i < REPORT_SIZE; i++) {
		printf("%02X ", buf[i]);
		if ((i + 1) % 16 == 0) printf("\n");
	}
	printf("\n");*/

	return 0;
}

int lbe_set_frequency(struct lbe_device* dev, int output, uint32_t frequency) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	if ((dev->model == LBE_1420 || dev->model == LBE_MINI) && output != 1) {
		fprintf(stderr, "This model only supports output 1\n");
		return -1;
	}

	if (dev->model == LBE_MINI) {
		/* Mini's opcode 0x04 is a full Si5351-style PLL program:
		 *   buf[2..4]   fin (3-byte LE)
		 *   buf[5..7]   N3 - 1 (3-byte LE)
		 *   buf[8]      N2_HS - 4
		 *   buf[9..11]  N2_LS - 1 (3-byte LE)
		 *   buf[12]     N1_HS - 4
		 *   buf[13..15] NC1_LS - 1 (3-byte LE)
		 *   buf[16..18] NC2_LS - 1 (3-byte LE)
		 *   buf[19]     SKEW
		 *   buf[20]     BWSEL
		 * f_out = fin * N2_HS * N2_LS / (N3 * N1_HS * NC1_LS). */
		uint32_t fin = 0, n3 = 0, n2hs = 0, n2ls = 0, n1hs = 0, nc1 = 0;
		if (mini_solve_pll(frequency, &fin, &n3, &n2hs, &n2ls, &n1hs, &nc1) < 0) {
			fprintf(stderr, "Mini: no valid PLL divider chain for %u Hz\n",
			        frequency);
			return -1;
		}
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
		/* NC2_LS copies NC1_LS (single-output Mini; the vendor tool
		 * does the same in its captured 10 MHz frame). */
		p[14] = nc1m & 0xFF;
		p[15] = (nc1m >> 8) & 0xFF;
		p[16] = (nc1m >> 16) & 0xFF;
		p[17] = 0;  /* SKEW */
		p[18] = 9;  /* BWSEL, matches captured default */
		return mini_set_feat(dev->fd, LBE_1420_SET_F1, p, sizeof p);
	}

	if (dev->model == LBE_1420) {
		buf[0] = LBE_1420_SET_F1;
		buf[1] = (frequency >>  0) & 0xff;
		buf[2] = (frequency >>  8) & 0xff;
		buf[3] = (frequency >> 16) & 0xff;
		buf[4] = (frequency >> 24) & 0xff;
	} else { // LBE_1421
		if (output == 1) {
			buf[0] = LBE_1421_SET_F1;
		} else if (output == 2) {
			buf[0] = LBE_1421_SET_F2;
		} else {
			fprintf(stderr, "Invalid output selection\n");
			return -1;
		}
		buf[5] = (frequency >>  0) & 0xff;
		buf[6] = (frequency >>  8) & 0xff;
		buf[7] = (frequency >> 16) & 0xff;
		buf[8] = (frequency >> 24) & 0xff;
	}

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

int lbe_set_frequency_temp(struct lbe_device* dev, int output, uint32_t frequency) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	/* Mini uses opcode 0x03 for drive strength, not temp freq, and
	 * the 1421 temp opcode 0x06 causes a USB reset. No known temp
	 * freq command for this model. */
	if (dev->model == LBE_MINI) {
		(void)output; (void)frequency;
		fprintf(stderr, "Temporary frequency is not supported on Mini\n");
		return -1;
	}

	if (dev->model == LBE_1420 && output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}

	if (dev->model == LBE_1420) {
		buf[0] = LBE_1420_SET_F1_TEMP;
		buf[1] = (frequency >>  0) & 0xff;
		buf[2] = (frequency >>  8) & 0xff;
		buf[3] = (frequency >> 16) & 0xff;
		buf[4] = (frequency >> 24) & 0xff;
	} else { // LBE_1421
		if (output == 1) {
			buf[0] = LBE_1421_SET_F1_TEMP;
		} else if (output == 2) {
			buf[0] = LBE_1421_SET_F2_TEMP;
		} else {
			fprintf(stderr, "Invalid output selection\n");
			return -1;
		}
		buf[5] = (frequency >>  0) & 0xff;
		buf[6] = (frequency >>  8) & 0xff;
		buf[7] = (frequency >> 16) & 0xff;
		buf[8] = (frequency >> 24) & 0xff;
	}

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

int lbe_set_outputs_enable(struct lbe_device* dev, int enable) {
	/* Vendor tool sends this with wValue=0x0300 and buf[2]=3 for
	 * enable / 0 for disable. Value 3 comes from the UI combo box
	 * index being multiplied by 3 (see sub_4134f0 in the Windows
	 * binary). The legacy buf[0]=opcode form does not latch the
	 * output stage on the Mini, so the signal never reaches REF IN. */
	if (dev->model == LBE_MINI) {
		uint8_t arg = enable ? 0x03 : 0x00;
		if (mini_set_feat(dev->fd, LBE_142X_EN_OUT, &arg, 1) < 0)
			return -1;
		return 0;
	}

	uint8_t buf[REPORT_SIZE] = {0};
	buf[0] = LBE_142X_EN_OUT;
	buf[1] = enable ? (dev->model == LBE_1421_DUALOUT ? 0x03 : 0x01) : 0x00;

	if (ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf) < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}
	return 0;
}

int lbe_blink_leds(struct lbe_device* dev) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	buf[0] = LBE_142X_BLINK_OUT;

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

int lbe_set_pll_mode(struct lbe_device* dev, int fll_mode) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	buf[0] = LBE_142X_SET_PLL;
	buf[1] = fll_mode ? 0x01 : 0x00;

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

int lbe_set_1pps(struct lbe_device* dev, int enable) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	if (dev->model != LBE_1421_DUALOUT) {
		fprintf(stderr, "1PPS control is only supported on LBE-1421\n");
		return -1;
	}

	buf[0] = LBE_1421_SET_PPS;
	buf[1] = enable ? 0x01 : 0x00;

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

int lbe_set_power_level(struct lbe_device* dev, int output, int low_power) {
	uint8_t buf[REPORT_SIZE] = {0};
	int res;
	int cmdpwrlevel = LBE_1421_SET_PWR1;

	if (dev->model == LBE_1420 && output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}

	if (dev->model == LBE_1420) {
		cmdpwrlevel = LBE_1420_SET_PWR1;
	}

	buf[0] = (output == 1) ? cmdpwrlevel : LBE_1421_SET_PWR2;
	buf[1] = low_power ? 0x01 : 0x00;

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
		perror("HIDIOCSFEATURE");
		return -1;
	}

	return 0;
}

#endif // __linux__
