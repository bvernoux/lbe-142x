/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2024-2026 Benjamin Vernoux
 */
#ifndef LBE_SERIAL_H
#define LBE_SERIAL_H

#include <stddef.h>

/* Thin cross-platform wrapper around a CDC / tty serial port. Used to
 * read NMEA from the LBE-1421/1423/1425 COM port. */
struct lbe_serial;

/* Open the port at `path`. On Windows expects "COM12" (without the
 * \\.\ prefix); on Linux expects "/dev/ttyACM0". Returns NULL and prints
 * a diagnostic on failure. */
struct lbe_serial *lbe_serial_open(const char *path);
void lbe_serial_close(struct lbe_serial *s);

/* Read one CR/LF-terminated line into `buf` (terminated NUL; no CR/LF).
 * Waits up to `timeout_ms` for a complete line. Returns line length on
 * success, 0 on timeout, -1 on error. */
int lbe_serial_readline(struct lbe_serial *s, char *buf, size_t n,
                        int timeout_ms);

/* Try common ports and return the first one that looks like an NMEA
 * stream (`$G` seen within 1 s). Writes the path into `out` on success.
 * Returns 0 on success, -1 if no NMEA port was found. */
int lbe_serial_find_nmea(char *out, size_t n);

/* Read the current state of the DCD (RLSD) modem status line. The
 * LBE-1421/1423/1425 drives this from its u-blox 1PPS output. Returns 1 if
 * asserted, 0 if deasserted, -1 on error. */
int lbe_serial_get_dcd(struct lbe_serial *s);

#endif
