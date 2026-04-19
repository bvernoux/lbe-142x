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
	return ioctl(fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
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
		status->frequency1 = buf[2] | (buf[3] << 8) | (buf[4] << 16) | (buf[5] << 24);
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

	if (dev->model == LBE_1420 || dev->model == LBE_MINI) {
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
	uint8_t buf[REPORT_SIZE] = {0};
	int res;

	buf[0] = LBE_142X_EN_OUT;
	buf[1] = enable ? (dev->model == LBE_1421_DUALOUT ? 0x03 : 0x01) : 0x00;

	res = ioctl(dev->fd, HIDIOCSFEATURE(REPORT_SIZE), buf);
	if (res < 0) {
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
