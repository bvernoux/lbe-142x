/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_TRANSPORT_H
#define LBE_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#define LBE_REPORT_SIZE 60

/* Opaque transport handle. The concrete struct lives in the
 * platform-specific transport .c file. */
struct lbe_transport;

/* Open the USB device matching the VID and any PID in the
 * zero-terminated list. Returns NULL if no matching device was found
 * or the OS refused to open it; writes the matched PID into *out_pid
 * (if non-NULL) on success.
 *
 * If preferred_pid != 0, only devices with that exact PID are
 * considered. If preferred_pid == 0 and more than one matching device
 * is present, the call fails after listing the matches on stderr --
 * pass a specific PID to disambiguate. */
struct lbe_transport *lbe_transport_open(uint16_t vid,
                                          const uint16_t *pids,
                                          uint16_t preferred_pid,
                                          uint16_t *out_pid);
void lbe_transport_close(struct lbe_transport *t);

/* Copy the device's USB iSerial string (NUL-terminated) into out. Returns 0
 * if a non-empty serial was available, -1 otherwise. */
int lbe_transport_serial(struct lbe_transport *t, char *out, size_t n);

/* Send a HID Feature report.
 *
 *   report_id  HID Report ID. 0 for devices without a Report ID
 *              (e.g. Mini). For 1420/1421, whose firmware reads the
 *              opcode out of the wValue low byte, pass the opcode
 *              itself.
 *   data       Full 60-byte USB payload as it will appear on the wire.
 *              For Report-ID devices the firmware echo byte lives at
 *              data[0]; for no-ID devices data[0] is the opcode.
 *
 * Internally the Linux backend translates to HIDIOCSFEATURE and the
 * Windows backend to libusb_control_transfer; both produce identical
 * USB traffic so the caller never needs to branch on platform. */
int lbe_transport_feat_set(struct lbe_transport *t, uint8_t report_id,
                            const uint8_t *data);

/* Receive a HID Feature report (LBE_REPORT_SIZE bytes into data).
 * Same report_id semantics as lbe_transport_feat_set. For Report-ID
 * devices data[0] is the ID echo; for no-ID devices data[0] is the
 * firmware's first real byte. */
int lbe_transport_feat_get(struct lbe_transport *t, uint8_t report_id,
                            uint8_t *data);

/* Same as lbe_transport_feat_get but suppresses the stderr message on
 * failure. Use for speculative/drain reads whose STALL is expected. */
int lbe_transport_feat_get_quiet(struct lbe_transport *t, uint8_t report_id,
                                  uint8_t *data);

/* Claim/release the device's HID interface. Required on Windows
 * before any libusb_interrupt_transfer call. No-op on Linux (hidraw
 * already holds the device). Returns 0 on success. */
int lbe_transport_claim(struct lbe_transport *t);
void lbe_transport_release(struct lbe_transport *t);

/* Read one interrupt-IN frame from endpoint `ep`. Returns the number
 * of bytes read (up to len), 0 on timeout, negative on error. */
int lbe_transport_read_input(struct lbe_transport *t, uint8_t ep,
                              uint8_t *buf, size_t len, int timeout_ms);

#endif
