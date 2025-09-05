#ifdef _WIN32

#include "lbe_device.h"
#include "lbe_common.h"
#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define REPORT_SIZE (64)
#define LIBUSB_CONTROL_TRANSFER_TIMEOUT_MS (5000)

// Fallback definitions for constants that might be missing
#ifndef LIBUSB_REQUEST_GET_REPORT
#define LIBUSB_REQUEST_GET_REPORT 0x01
#endif

#ifndef LIBUSB_REQUEST_SET_REPORT
#define LIBUSB_REQUEST_SET_REPORT 0x09
#endif

#ifndef LIBUSB_REPORT_TYPE_FEATURE
#define LIBUSB_REPORT_TYPE_FEATURE 0x03
#endif

struct lbe_device {
	libusb_device_handle *handle;
	uint16_t product_id;
	enum lbe_model model;
};

struct lbe_device* lbe_open_device(void) {
	struct lbe_device* dev = malloc(sizeof(struct lbe_device));
	if (!dev) return NULL;

	libusb_device **devs;
	ssize_t cnt;
	int ret;

	ret = libusb_init(NULL);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize libusb: %s\n", libusb_error_name(ret));
		free(dev);
		return NULL;
	}

	cnt = libusb_get_device_list(NULL, &devs);
	if (cnt < 0) {
		fprintf(stderr, "Failed to get device list: %s\n", libusb_error_name((int)cnt));
		free(dev);
		return NULL;
	}

	for (ssize_t i = 0; i < cnt; i++) {
		struct libusb_device_descriptor desc;
		libusb_device *device = devs[i];

		if (libusb_get_device_descriptor(device, &desc) < 0)
			continue;

		if (desc.idVendor == VID_LBE && (desc.idProduct == PID_LBE_1420 || desc.idProduct == PID_LBE_1421)) {
			ret = libusb_open(device, &dev->handle);
			if (ret < 0) {
				fprintf(stderr, "Failed to open device: %s\n", libusb_error_name(ret));
				continue;
			}
			dev->product_id = desc.idProduct;
			dev->model = (desc.idProduct == PID_LBE_1420) ? LBE_1420 : LBE_1421_DUALOUT;
			libusb_free_device_list(devs, 1);
			return dev;
		}
	}

	fprintf(stderr, "LBE-142x device not found\n");
	libusb_free_device_list(devs, 1);
	free(dev);
	return NULL;
}

void lbe_close_device(struct lbe_device* dev) {
	if (dev) {
		libusb_close(dev->handle);
		libusb_exit(NULL);
		free(dev);
	}
}

enum lbe_model lbe_get_model(struct lbe_device* dev) {
	return dev->model;
}

/* Helper function for feature reports */
static int send_feature_report(struct lbe_device* dev, const uint8_t* report, size_t length) {
	if (length > UINT16_MAX) {
		fprintf(stderr, "Feature report too long\n");
		return -1;
	}

	int ret = libusb_control_transfer(dev->handle,
				LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
				LIBUSB_REQUEST_SET_REPORT,
				(LIBUSB_REPORT_TYPE_FEATURE << 8) | report[0],
				0,
				(unsigned char*)report,
				(uint16_t)length,
				LIBUSB_CONTROL_TRANSFER_TIMEOUT_MS);

	if (ret < 0) {
		fprintf(stderr, "Failed to send feature report: %s\n", libusb_error_name(ret));
		return -1;
	}
	return 0;
}

int lbe_get_device_status(struct lbe_device* dev, struct lbe_status* status) {
	uint8_t buf[REPORT_SIZE] = {0};
	int ret;

	buf[0] = 0x4B; // Report Number
	ret = libusb_control_transfer(dev->handle,
				LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE,
				LIBUSB_REQUEST_GET_REPORT,
				(LIBUSB_REPORT_TYPE_FEATURE << 8) | buf[0],
				0,
				buf,
				REPORT_SIZE,
				LIBUSB_CONTROL_TRANSFER_TIMEOUT_MS);

	if (ret < 0) {
		fprintf(stderr, "Failed to get feature report: %s\n", libusb_error_name(ret));
		return -1;
	}

	status->raw_status = buf[2];
	status->frequency1 = buf[7] | (buf[8] << 8) | (buf[9] << 16) | (buf[10] << 24);
	status->frequency2 = buf[15] | (buf[16] << 8) | (buf[17] << 16) | (buf[18] << 24);
	
	status->outputs_enabled = (status->raw_status & (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT)) == (LBE_OUT1_EN_BIT | LBE_OUT2_EN_BIT);
	status->fll_enabled = buf[19] != 0;

	status->pll_locked = (status->raw_status & LBE_PLL_LOCK_BIT) != 0;
	// Additional status information for LBE-1421
	if (dev->model == LBE_1421_DUALOUT) {
		status->antenna_ok = (status->raw_status & LBE_ANT_OK_BIT) != 0;
		status->pps_enabled = (status->raw_status & LBE_PPS_EN_BIT) != 0;
		status->out1_power_low = buf[20] != 0;
		status->out2_power_low = buf[21] != 0;
	} else {
		status->pll_locked = 0;
		status->antenna_ok = (status->raw_status & LBE_ANT_OK_BIT) != 0;
		status->pps_enabled = 0;
		status->out1_power_low = buf[20] != 0;
		status->out2_power_low = 0;
		status->outputs_enabled = 1; // Seems to be always on even with legit soft
	}

/*
	printf("Raw report dump:\n");
	for (int i = 0; i < REPORT_SIZE; i++) {
		printf("%02X ", buf[i]);
		if ((i + 1) % 16 == 0) printf("\n");
	}
	printf("\n");
*/
	
	return 0;
}

int lbe_set_frequency(struct lbe_device* dev, int output, uint32_t frequency) {
	uint8_t buf[REPORT_SIZE] = {0};

	if (dev->model == LBE_1420 && output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}

	buf[0] = 0x4B; // Report ID
	if (dev->model == LBE_1420) {
		buf[1] = LBE_1420_SET_F1;
		buf[2] = (frequency >>  0) & 0xff;
		buf[3] = (frequency >>  8) & 0xff;
		buf[4] = (frequency >> 16) & 0xff;
		buf[5] = (frequency >> 24) & 0xff;
	} else { // LBE_1421_DUALOUT
		if (output == 1) {
			buf[1] = LBE_1421_SET_F1;
		} else if (output == 2) {
			buf[1] = LBE_1421_SET_F2;
		} else {
			fprintf(stderr, "Invalid output selection\n");
			return -1;
		}
		buf[6] = (frequency >>  0) & 0xff;
		buf[7] = (frequency >>  8) & 0xff;
		buf[8] = (frequency >> 16) & 0xff;
		buf[9] = (frequency >> 24) & 0xff;
	}

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_set_frequency_temp(struct lbe_device* dev, int output, uint32_t frequency) {
	uint8_t buf[REPORT_SIZE] = {0};

	buf[0] = 0x4B; // Report ID
	if (dev->model == LBE_1420) {
		buf[1] = LBE_1420_SET_F1_TEMP;
		buf[2] = (frequency >>  0) & 0xff;
		buf[3] = (frequency >>  8) & 0xff;
		buf[4] = (frequency >> 16) & 0xff;
		buf[5] = (frequency >> 24) & 0xff;
	} else { // LBE_1421_DUALOUT
		if (output == 1) {
			buf[1] = LBE_1421_SET_F1_TEMP;
		} else if (output == 2) {
			buf[1] = LBE_1421_SET_F2_TEMP;
		} else {
			fprintf(stderr, "Invalid output selection\n");
			return -1;
		}
		buf[6] = (frequency >>  0) & 0xff;
		buf[7] = (frequency >>  8) & 0xff;
		buf[8] = (frequency >> 16) & 0xff;
		buf[9] = (frequency >> 24) & 0xff;
	}

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_set_outputs_enable(struct lbe_device* dev, int enable) {
	uint8_t buf[REPORT_SIZE] = {0};

	buf[0] = 0x4B; // Report ID
	buf[1] = LBE_142X_EN_OUT;
	buf[2] = enable ? (dev->model == LBE_1421_DUALOUT ? 0x03 : 0x01) : 0x00;

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_blink_leds(struct lbe_device* dev) {
	uint8_t buf[REPORT_SIZE] = {0};

	buf[0] = 0x4B; // Report ID
	buf[1] = LBE_142X_BLINK_OUT;

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_set_pll_mode(struct lbe_device* dev, int fll_mode) {
	uint8_t buf[REPORT_SIZE] = {0};

	buf[0] = 0x4B; // Report ID
	buf[1] = LBE_142X_SET_PLL;
	buf[2] = fll_mode ? 0x01 : 0x00;

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_set_1pps(struct lbe_device* dev, int enable) {
	uint8_t buf[REPORT_SIZE] = {0};

	if (dev->model != LBE_1421_DUALOUT) {
		fprintf(stderr, "1PPS control is only supported on LBE-1421\n");
		return -1;
	}

	buf[0] = 0x4B; // Report ID
	buf[1] = LBE_1421_SET_PPS;
	buf[2] = enable ? 0x01 : 0x00;

	return send_feature_report(dev, buf, REPORT_SIZE);
}

int lbe_set_power_level(struct lbe_device* dev, int output, int low_power) {
	uint8_t buf[REPORT_SIZE] = {0};
	uint8_t cmdpwrlevel = LBE_1421_SET_PWR1; // code remains the same than lbe 1241 on Windows?

	if (dev->model == LBE_1420 && output != 1) {
		fprintf(stderr, "LBE-1420 only supports output 1\n");
		return -1;
	}

	buf[0] = 0x4B; // Report ID
	buf[1] = (output == 1) ? cmdpwrlevel : LBE_1421_SET_PWR2;
	buf[2] = low_power ? 0x01 : 0x00;

	return send_feature_report(dev, buf, REPORT_SIZE);
}

#endif // _WIN32