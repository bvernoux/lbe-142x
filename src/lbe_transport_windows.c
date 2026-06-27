/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifdef _WIN32

#include "lbe_transport.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define LBE_CTRL_TIMEOUT_MS 5000

/* On Windows, the first control transfers right after libusb_open /
 * libusb_claim_interface can race with the HID stack finishing its own
 * post-enumerate handshake and return LIBUSB_ERROR_IO or ERROR_PIPE.
 * Retry a small number of times with a short backoff before surfacing
 * the error to the user. */
#define LBE_CTRL_RETRY_MAX     3
#define LBE_CTRL_RETRY_SLEEP_MS 20

static int ctrl_transfer_retry(libusb_device_handle *h, uint8_t bmReq,
                                uint8_t bRequest, uint16_t wValue,
                                uint16_t wIndex, uint8_t *buf, uint16_t len,
                                unsigned timeout_ms) {
	int rc = 0;
	for (int attempt = 0; attempt < LBE_CTRL_RETRY_MAX; attempt++) {
		rc = libusb_control_transfer(h, bmReq, bRequest, wValue, wIndex,
		                              buf, len, timeout_ms);
		if (rc >= 0) return rc;
		if (rc != LIBUSB_ERROR_IO && rc != LIBUSB_ERROR_PIPE) break;
		/* Skip the backoff on the last iteration -- it would be wasted
		 * wall-clock on the terminal-failure path. */
		if (attempt + 1 < LBE_CTRL_RETRY_MAX)
			Sleep(LBE_CTRL_RETRY_SLEEP_MS);
	}
	return rc;
}

#ifndef LIBUSB_REQUEST_GET_REPORT
#define LIBUSB_REQUEST_GET_REPORT 0x01
#endif
#ifndef LIBUSB_REQUEST_SET_REPORT
#define LIBUSB_REQUEST_SET_REPORT 0x09
#endif
#ifndef LIBUSB_REPORT_TYPE_FEATURE
#define LIBUSB_REPORT_TYPE_FEATURE 0x03
#endif

struct lbe_transport {
	libusb_device_handle *handle;
	int interface_claimed;
	char serial[64];
};

static int libusb_ref = 0;

static const char *pid_to_name(uint16_t pid) {
	switch (pid) {
	case 0x2443: return "LBE-1420";
	case 0x2444: return "LBE-1421";
	case 0x226f: return "LBE-1423";
	case 0x2269: return "LBE-1425";
	case 0x2211: return "LBE-Mini";
	default:     return "LBE-???";
	}
}

struct lbe_transport *lbe_transport_open(uint16_t vid,
                                          const uint16_t *pids,
                                          uint16_t preferred_pid,
                                          uint16_t *out_pid) {
	if (!libusb_ref) {
		int rc = libusb_init(NULL);
		if (rc < 0) {
			fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
			return NULL;
		}
	}
	libusb_ref++;

	libusb_device **devs;
	ssize_t cnt = libusb_get_device_list(NULL, &devs);
	if (cnt < 0) {
		fprintf(stderr, "libusb_get_device_list: %s\n", libusb_error_name((int)cnt));
		if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
		return NULL;
	}

	/* Pass 1: enumerate matches. If preferred_pid is set, only devices
	 * with that exact PID count. Otherwise any PID in the supported list
	 * counts -- if more than one matches we list them and refuse to open. */
	ssize_t match_idx[8];
	uint16_t match_pid[8];
	int match_count = 0;
	for (ssize_t i = 0; i < cnt && match_count < 8; i++) {
		struct libusb_device_descriptor desc;
		if (libusb_get_device_descriptor(devs[i], &desc) < 0) continue;
		if (desc.idVendor != vid) continue;
		if (preferred_pid) {
			if (desc.idProduct != preferred_pid) continue;
		} else {
			int ok = 0;
			for (const uint16_t *p = pids; *p; p++)
				if (desc.idProduct == *p) { ok = 1; break; }
			if (!ok) continue;
		}
		match_idx[match_count] = i;
		match_pid[match_count] = desc.idProduct;
		match_count++;
	}

	if (match_count == 0) {
		if (preferred_pid)
			fprintf(stderr, "No device with VID %04x PID %04x found\n",
			        vid, preferred_pid);
		else
			fprintf(stderr, "LBE device not found\n");
		libusb_free_device_list(devs, 1);
		if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
		return NULL;
	}
	if (match_count > 1 && !preferred_pid) {
		fprintf(stderr, "Multiple LBE devices found -- select one with --pid 0xNNNN:\n");
		for (int k = 0; k < match_count; k++)
			fprintf(stderr, "  %s  (PID 0x%04x)\n",
			        pid_to_name(match_pid[k]), match_pid[k]);
		libusb_free_device_list(devs, 1);
		if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
		return NULL;
	}

	libusb_device_handle *h = NULL;
	int rc = libusb_open(devs[match_idx[0]], &h);
	if (rc < 0) {
		fprintf(stderr, "libusb_open: %s\n", libusb_error_name(rc));
		libusb_free_device_list(devs, 1);
		if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
		return NULL;
	}
	struct lbe_transport *t = calloc(1, sizeof *t);
	if (!t) {
		libusb_close(h);
		libusb_free_device_list(devs, 1);
		if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
		return NULL;
	}
	t->handle = h;
	t->interface_claimed = 0;
	/* Best-effort USB iSerial read (non-fatal if it fails). */
	struct libusb_device_descriptor desc;
	if (libusb_get_device_descriptor(devs[match_idx[0]], &desc) == 0
	    && desc.iSerialNumber) {
		libusb_get_string_descriptor_ascii(h, desc.iSerialNumber,
		                                   (unsigned char *)t->serial,
		                                   (int)sizeof t->serial);
	}
	if (out_pid) *out_pid = match_pid[0];
	libusb_free_device_list(devs, 1);
	return t;
}

int lbe_transport_serial(struct lbe_transport *t, char *out, size_t n) {
	if (!t || !t->serial[0]) { if (n) out[0] = '\0'; return -1; }
	snprintf(out, n, "%s", t->serial);
	return 0;
}

void lbe_transport_close(struct lbe_transport *t) {
	if (!t) return;
	if (t->interface_claimed) libusb_release_interface(t->handle, 0);
	libusb_close(t->handle);
	free(t);
	if (libusb_ref > 0 && --libusb_ref == 0) libusb_exit(NULL);
}

/* libusb on Windows (WinUSB backend) reads/writes the FULL HID max-packet
 * buffer for feature transfers -- 64 bytes on these devices -- even when
 * wLength < 64. Passing a 60-byte stack buffer makes it read 4 bytes past
 * the end of the caller's buffer, which MSVC /GS detects as a stack cookie
 * corruption (STATUS_STACK_BUFFER_OVERRUN / 0xC0000409). Copy into a local
 * 64-byte buffer first and pass that to libusb. wLength stays at
 * LBE_REPORT_SIZE so the wire traffic matches the vendor GUI. */
#define LBE_WIN_XFER_BUF 64
#define LBE_WIN_RX_STAGE 256

int lbe_transport_feat_set(struct lbe_transport *t, uint8_t report_id,
                            const uint8_t *data) {
	uint8_t xfer[LBE_WIN_XFER_BUF] = {0};
	memcpy(xfer, data, LBE_REPORT_SIZE);
	uint16_t wValue = ((uint16_t)LIBUSB_REPORT_TYPE_FEATURE << 8) | report_id;
	uint8_t bmRequestType = (uint8_t)LIBUSB_ENDPOINT_OUT
	                      | (uint8_t)LIBUSB_REQUEST_TYPE_CLASS
	                      | (uint8_t)LIBUSB_RECIPIENT_INTERFACE;
	int rc = ctrl_transfer_retry(t->handle, bmRequestType,
			LIBUSB_REQUEST_SET_REPORT, wValue, 0x0000,
			xfer, LBE_REPORT_SIZE,
			LBE_CTRL_TIMEOUT_MS);
	if (rc < 0) {
		fprintf(stderr, "SET_REPORT id=0x%02X: %s\n",
		        report_id, libusb_error_name(rc));
		return -1;
	}
	return 0;
}

static int feat_get_impl(struct lbe_transport *t, uint8_t report_id,
                         uint8_t *data, int verbose) {
	uint8_t xfer[LBE_WIN_XFER_BUF] = {0};
	uint16_t wValue = ((uint16_t)LIBUSB_REPORT_TYPE_FEATURE << 8) | report_id;
	uint8_t bmRequestType = (uint8_t)LIBUSB_ENDPOINT_IN
	                      | (uint8_t)LIBUSB_REQUEST_TYPE_CLASS
	                      | (uint8_t)LIBUSB_RECIPIENT_INTERFACE;
	int rc = ctrl_transfer_retry(t->handle, bmRequestType,
			LIBUSB_REQUEST_GET_REPORT, wValue, 0x0000,
			xfer, LBE_WIN_XFER_BUF,
			LBE_CTRL_TIMEOUT_MS);
	if (rc < 0) {
		if (verbose) fprintf(stderr, "GET_REPORT id=0x%02X: %s\n",
		                      report_id, libusb_error_name(rc));
		return -1;
	}
	/* For numbered-RID devices (LBE-1420/1421/1423/1425, RID 0x4B and opcode
	 * RIDs), WinUSB returns one extra prefix byte ahead of the firmware's
	 * RID echo that Linux hidraw does not surface. Skip it so the byte
	 * offsets the model code uses match between platforms. Mini (implicit
	 * RID 0) has no such prefix. */
	size_t skip = (report_id != 0) ? 1u : 0u;
	memcpy(data, xfer + skip, LBE_REPORT_SIZE);
	return 0;
}

int lbe_transport_feat_get(struct lbe_transport *t, uint8_t report_id,
                            uint8_t *data) {
	return feat_get_impl(t, report_id, data, 1);
}

int lbe_transport_feat_get_quiet(struct lbe_transport *t, uint8_t report_id,
                                  uint8_t *data) {
	return feat_get_impl(t, report_id, data, 0);
}

int lbe_transport_claim(struct lbe_transport *t) {
	if (!t || t->interface_claimed) return 0;
	int rc = libusb_claim_interface(t->handle, 0);
	if (rc == 0) { t->interface_claimed = 1; return 0; }
	fprintf(stderr, "libusb_claim_interface(0): %s\n", libusb_error_name(rc));
	return -1;
}

void lbe_transport_release(struct lbe_transport *t) {
	if (!t || !t->interface_claimed) return;
	libusb_release_interface(t->handle, 0);
	t->interface_claimed = 0;
}

int lbe_transport_read_input(struct lbe_transport *t, uint8_t ep,
                              uint8_t *buf, size_t len, int timeout_ms) {
	/* Same overrun concern as feat_set/feat_get: libusb's WinUSB backend
	 * can write past `length` into the user buffer on an interrupt-IN
	 * transfer. MSVC /GS catches it (STATUS_STACK_BUFFER_OVERRUN). Even a
	 * 64-byte local matching the Mini's wMaxPacketSize=64 isn't enough,
	 * so stage into an oversized buffer and truncate to `len` on copy-out. */
	uint8_t xfer[LBE_WIN_RX_STAGE] = {0};
	int ask = (int)(len > sizeof xfer ? sizeof xfer : len);
	int transferred = 0;
	int rc = libusb_interrupt_transfer(t->handle, ep, xfer, ask,
	                                    &transferred, timeout_ms);
	if (rc == LIBUSB_ERROR_TIMEOUT) return 0;
	if (rc < 0) return -1;
	if ((size_t)transferred > len) transferred = (int)len;
	memcpy(buf, xfer, (size_t)transferred);
	return transferred;
}

#endif
