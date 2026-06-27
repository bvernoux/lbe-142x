/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifdef __linux__

#include "lbe_transport.h"

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

#ifndef HIDIOCSFEATURE
#define HIDIOCSFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x06, len)
#define HIDIOCGFEATURE(len)    _IOC(_IOC_WRITE|_IOC_READ, 'H', 0x07, len)
#endif

/* The first HIDIOCxFEATURE ioctls right after opening /dev/hidrawN can
 * race with the kernel HID stack / device firmware finishing their
 * post-enumerate handshake and return -EPIPE (USB control-pipe STALL),
 * -EAGAIN, or -EBUSY. The Windows transport already handles this via
 * ctrl_transfer_retry(); mirror the same 3-try / 20 ms backoff here. */
#define LBE_CTRL_RETRY_MAX      3
#define LBE_CTRL_RETRY_SLEEP_US 20000

static int ioctl_retry(int fd, unsigned long req, void *arg) {
	int rc = -1;
	for (int attempt = 0; attempt < LBE_CTRL_RETRY_MAX; attempt++) {
		rc = ioctl(fd, req, arg);
		if (rc >= 0) return rc;
		if (errno != EPIPE && errno != EAGAIN && errno != EBUSY
		    && errno != EINTR) break;
		/* Skip the backoff on the last iteration: it would be wasted
		 * wall-clock, and if usleep got interrupted by a signal it
		 * would clobber errno before the caller's perror sees it. */
		if (attempt + 1 < LBE_CTRL_RETRY_MAX)
			usleep(LBE_CTRL_RETRY_SLEEP_US);
	}
	return rc;
}

struct lbe_transport {
	int fd;
	char serial[64];
};

/* Read the USB iSerial for a hidraw node from sysfs. The hidraw class symlink
 * points at the HID device; its grandparent is the USB device, which exposes
 * a `serial` attribute. e.g. /sys/class/hidraw/hidraw0/device/../../serial */
static void read_serial(const char *hidraw_name, char *out, size_t n) {
	out[0] = '\0';
	char path[PATH_MAX];
	snprintf(path, sizeof path,
	         "/sys/class/hidraw/%s/device/../../serial", hidraw_name);
	FILE *f = fopen(path, "r");
	if (!f) return;
	if (fgets(out, (int)n, f)) {
		size_t len = strlen(out);
		while (len && (out[len-1] == '\n' || out[len-1] == '\r'))
			out[--len] = '\0';
	}
	fclose(f);
}

static int probe_hidraw(const char *path, uint16_t vid, const uint16_t *pids,
                        uint16_t *out_pid) {
	int fd = open(path, O_RDWR);
	if (fd < 0) return -1;

	struct hidraw_devinfo info;
	if (ioctl(fd, HIDIOCGRAWINFO, &info) < 0 || info.vendor != vid) {
		close(fd);
		return -1;
	}
	for (const uint16_t *p = pids; *p; p++) {
		if (info.product == *p) {
			if (out_pid) *out_pid = (uint16_t)info.product;
			return fd;
		}
	}
	close(fd);
	return -1;
}

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
	DIR *dir = opendir("/dev");
	if (!dir) { perror("opendir /dev"); return NULL; }

	/* If preferred_pid is set, limit the probe to that single PID. */
	uint16_t single[2] = {preferred_pid, 0};
	const uint16_t *probe_pids = preferred_pid ? single : pids;

	struct dirent *ent;
	char path[PATH_MAX];
	int matches[8];
	uint16_t match_pids[8];
	char match_names[8][256];
	int match_count = 0;
	while ((ent = readdir(dir)) != NULL && match_count < 8) {
		if (strncmp(ent->d_name, "hidraw", 6) != 0) continue;
		snprintf(path, sizeof path, "/dev/%s", ent->d_name);
		uint16_t pid = 0;
		int fd = probe_hidraw(path, vid, probe_pids, &pid);
		if (fd < 0) continue;
		matches[match_count] = fd;
		match_pids[match_count] = pid;
		snprintf(match_names[match_count], sizeof match_names[0], "%s", ent->d_name);
		match_count++;
	}
	closedir(dir);

	if (match_count == 0) {
		if (preferred_pid)
			fprintf(stderr, "No device with VID %04x PID %04x found\n",
			        vid, preferred_pid);
		else
			fprintf(stderr, "LBE device not found\n");
		return NULL;
	}
	if (match_count > 1 && !preferred_pid) {
		fprintf(stderr, "Multiple LBE devices found -- select one with --pid 0xNNNN:\n");
		for (int k = 0; k < match_count; k++) {
			fprintf(stderr, "  %s  (PID 0x%04x)\n",
			        pid_to_name(match_pids[k]), match_pids[k]);
			close(matches[k]);
		}
		return NULL;
	}

	/* Keep the first match, close any siblings (there won't be any when
	 * preferred_pid filtered the list to one). */
	for (int k = 1; k < match_count; k++) close(matches[k]);

	struct lbe_transport *t = calloc(1, sizeof *t);
	if (!t) { close(matches[0]); return NULL; }
	t->fd = matches[0];
	read_serial(match_names[0], t->serial, sizeof t->serial);
	if (out_pid) *out_pid = match_pids[0];
	return t;
}

int lbe_transport_serial(struct lbe_transport *t, char *out, size_t n) {
	if (!t || !t->serial[0]) { if (n) out[0] = '\0'; return -1; }
	snprintf(out, n, "%s", t->serial);
	return 0;
}

void lbe_transport_close(struct lbe_transport *t) {
	if (!t) return;
	close(t->fd);
	free(t);
}

/* hidraw uses buf[0] as the Report ID signal: when it's 0 the kernel
 * treats the report as having no ID and skips the first byte of the
 * buffer on both SET and GET (the USB wire then carries exactly
 * LBE_REPORT_SIZE bytes starting from buf[1]). When non-zero, the
 * kernel sends/receives the buffer verbatim. We always work with a
 * (LBE_REPORT_SIZE+1)-byte scratch and copy into/out of the caller's
 * LBE_REPORT_SIZE-byte data area to hide this asymmetry. */
int lbe_transport_feat_set(struct lbe_transport *t, uint8_t report_id,
                            const uint8_t *data) {
	uint8_t buf[LBE_REPORT_SIZE + 1] = {0};
	if (report_id == 0) {
		buf[0] = 0;
		memcpy(&buf[1], data, LBE_REPORT_SIZE);
		if (ioctl_retry(t->fd, HIDIOCSFEATURE(LBE_REPORT_SIZE + 1), buf) < 0) {
			perror("HIDIOCSFEATURE");
			return -1;
		}
	} else {
		memcpy(buf, data, LBE_REPORT_SIZE);
		buf[0] = report_id; /* kernel derives wValue from buf[0] */
		if (ioctl_retry(t->fd, HIDIOCSFEATURE(LBE_REPORT_SIZE), buf) < 0) {
			perror("HIDIOCSFEATURE");
			return -1;
		}
	}
	return 0;
}

static int feat_get_impl(struct lbe_transport *t, uint8_t report_id,
                         uint8_t *data, int verbose) {
	uint8_t buf[LBE_REPORT_SIZE + 1] = {0};
	if (report_id == 0) {
		buf[0] = 0;
		if (ioctl_retry(t->fd, HIDIOCGFEATURE(LBE_REPORT_SIZE + 1), buf) < 0) {
			if (verbose) perror("HIDIOCGFEATURE");
			return -1;
		}
		memcpy(data, &buf[1], LBE_REPORT_SIZE);
	} else {
		buf[0] = report_id;
		if (ioctl_retry(t->fd, HIDIOCGFEATURE(LBE_REPORT_SIZE), buf) < 0) {
			if (verbose) perror("HIDIOCGFEATURE");
			return -1;
		}
		memcpy(data, buf, LBE_REPORT_SIZE);
	}
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

int lbe_transport_claim(struct lbe_transport *t) { (void)t; return 0; }
void lbe_transport_release(struct lbe_transport *t) { (void)t; }

int lbe_transport_read_input(struct lbe_transport *t, uint8_t ep,
                              uint8_t *buf, size_t len, int timeout_ms) {
	(void)ep; /* hidraw reads from whatever IN endpoint the HID stack
	           * associated with the device; ep is only meaningful on
	           * libusb. */
	struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(t->fd, &rfds);
	int rc = select(t->fd + 1, &rfds, NULL, NULL, &tv);
	if (rc < 0) return -1;
	if (rc == 0) return 0;
	ssize_t got = read(t->fd, buf, len);
	return got < 0 ? -1 : (int)got;
}

#endif
