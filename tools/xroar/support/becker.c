/** \file
 *
 *  \brief Becker port support (lazy-connect variant).
 *
 *  The "becker port" is an IP version of the usually-serial DriveWire protocol.
 *
 *  \copyright Copyright 2012-2025 Ciaran Anscomb
 *
 *  \licenseblock This file is part of XRoar, a Dragon/Tandy CoCo emulator.
 *
 *  XRoar is free software; you can redistribute it and/or modify it under the
 *  terms of the GNU General Public License as published by the Free Software
 *  Foundation, either version 3 of the License, or (at your option) any later
 *  version.
 *
 *  See COPYING.GPL for redistribution conditions.
 *
 *  \endlicenseblock
 *
 *  ---------------------------------------------------------------------------
 *  fujinet-go-coco modification: the upstream becker_open() connects exactly
 *  once and disables the port on failure. Here XRoar shares a process with the
 *  in-process FujiNet runtime (the DriveWire server) that it connects *out* to,
 *  and a CoCo 2 <-> CoCo 3 machine switch tears the link down and back up. So this
 *  variant connects lazily and reconnects automatically: becker_open() always
 *  succeeds (deferred socket), and the read/write/status paths (re)establish the
 *  connection -- throttled -- whenever it is down. Behaviour once connected is
 *  identical to upstream.
 *  ---------------------------------------------------------------------------
 */

#include "top-config.h"

// for addrinfo
#define _POSIX_C_SOURCE 200112L

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef WINDOWS32

#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#else

/* Windows has a habit of making include order important: */
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#endif

#include "xalloc.h"

#include "becker.h"
#include "logging.h"
#include "xroar.h"

#ifdef WANT_BECKER

/* In theory no reponse should be longer than this (though it doesn't actually
 * matter, this only constrains how much is read at a time). */
#define INPUT_BUFFER_SIZE 262
#define OUTPUT_BUFFER_SIZE 16

// How many status/read/write calls to skip between reconnect attempts while the
// port is disconnected. getaddrinfo("127.0.0.1") is cheap, but the CoCo polls
// the status register heavily, so don't hammer it every poll.
#define RECONNECT_THROTTLE 240

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

struct becker {
	int sockfd;
	int reconnect_countdown;
	char input_buf[INPUT_BUFFER_SIZE];
	int input_buf_ptr;
	int input_buf_length;
	char output_buf[OUTPUT_BUFFER_SIZE];
	int output_buf_ptr;
	int output_buf_length;

	// Debugging
	struct log_handle *log_data_in_hex;
	struct log_handle *log_data_out_hex;
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Attempt a single connection to the configured DriveWire/Becker server.
// Returns a connected non-blocking socket, or -1 on any failure.
static int becker_socket_connect(void) {
	struct addrinfo hints, *info = NULL;
	const char *hostname = xroar.cfg.becker.ip ? xroar.cfg.becker.ip : BECKER_IP_DEFAULT;
	const char *portname = xroar.cfg.becker.port ? xroar.cfg.becker.port : BECKER_PORT_DEFAULT;

	int sockfd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	if (getaddrinfo(hostname, portname, &hints, &info) < 0)
		goto failed;
	if (!info)
		goto failed;

	sockfd = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
	if (sockfd < 0)
		goto failed;

	{
		int flag = 1;
		setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (void const *)&flag, sizeof(flag));
	}

	if (connect(sockfd, info->ai_addr, info->ai_addrlen) < 0)
		goto failed;

	freeaddrinfo(info);
	info = NULL;

	// Set the socket to non-blocking
#ifndef WINDOWS32
	{
		int flags = fcntl(sockfd, F_GETFL, 0);
		fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
	}
#else
	{
		u_long iMode = 1;
		if (ioctlsocket(sockfd, FIONBIO, &iMode) != NO_ERROR)
			goto failed;
	}
#endif

	LOG_MOD_DEBUG(2, "becker", "connected to %s:%s\n", hostname, portname);
	return sockfd;

failed:
	if (sockfd != -1)
		close(sockfd);
	if (info)
		freeaddrinfo(info);
	return -1;
}

// If disconnected, throttled attempt to (re)connect.
static void becker_poll_connect(struct becker *b) {
	if (b->sockfd >= 0)
		return;
	if (b->reconnect_countdown > 0) {
		b->reconnect_countdown--;
		return;
	}
	b->reconnect_countdown = RECONNECT_THROTTLE;
	b->sockfd = becker_socket_connect();
	if (b->sockfd >= 0) {
		// Fresh connection: discard any stale buffered data.
		b->input_buf_ptr = b->input_buf_length = 0;
		b->output_buf_ptr = b->output_buf_length = 0;
	}
}

static void becker_drop(struct becker *b) {
	if (b->sockfd >= 0)
		close(b->sockfd);
	b->sockfd = -1;
	b->reconnect_countdown = RECONNECT_THROTTLE;
	b->input_buf_ptr = b->input_buf_length = 0;
	b->output_buf_ptr = b->output_buf_length = 0;
}

struct becker *becker_open(void) {
	// Never fail: defer the connection so XRoar tolerates the DriveWire server
	// (the in-process FujiNet runtime) not yet listening, and reconnects after
	// a machine switch.
	struct becker *b = xmalloc(sizeof(*b));
	*b = (struct becker){0};
	b->sockfd = -1;
	b->reconnect_countdown = 0;  // attempt immediately on first use
	becker_reset(b);
	b->sockfd = becker_socket_connect();
	return b;
}

void becker_close(struct becker *b) {
	if (!b) {
		return;
	}
	if (b->sockfd >= 0)
		close(b->sockfd);
	if (b->log_data_in_hex) {
		log_close(&b->log_data_in_hex);
	}
	if (b->log_data_out_hex) {
		log_close(&b->log_data_out_hex);
	}
	free(b);
}

void becker_reset(struct becker *b) {
	if (logging.debug_fdc & LOG_FDC_BECKER) {
		log_open_hexdump(&b->log_data_in_hex, "BECKER IN ");
		log_open_hexdump(&b->log_data_out_hex, "BECKER OUT");
	}
}

static void fetch_input(struct becker *b) {
	if (b->sockfd < 0)
		return;
	if (b->input_buf_ptr == b->input_buf_length) {
		ssize_t new = recv(b->sockfd, b->input_buf, INPUT_BUFFER_SIZE, 0);
		if (new > 0) {
			b->input_buf_length = new;
			if (logging.debug_fdc & LOG_FDC_BECKER) {
				// flush & reopen output hexdump
				log_open_hexdump(&b->log_data_out_hex, "BECKER OUT");
				for (ssize_t i = 0; i < new; ++i) {
					log_hexdump_byte(b->log_data_in_hex, b->input_buf[i]);
				}
			}
		} else if (new == 0) {
			// Peer closed the connection: drop and allow reconnect.
			becker_drop(b);
		}
	}
}

static void write_output(struct becker *b) {
	if (b->sockfd < 0)
		return;
	if (b->output_buf_length > 0) {
		ssize_t sent = send(b->sockfd, b->output_buf + b->output_buf_ptr, b->output_buf_length - b->output_buf_ptr, 0);
		if (sent > 0) {
			if (logging.debug_fdc & LOG_FDC_BECKER) {
				// flush & reopen input hexdump
				log_open_hexdump(&b->log_data_in_hex, "BECKER IN ");
				for (ssize_t i = 0; i < sent; ++i) {
					log_hexdump_byte(b->log_data_out_hex, b->output_buf[b->output_buf_ptr + i]);
				}
			}
			b->output_buf_ptr += sent;
			if (b->output_buf_ptr >= b->output_buf_length) {
				b->output_buf_ptr = b->output_buf_length = 0;
			}
		}
	}
}

uint8_t becker_read_status(struct becker *b) {
	if (logging.debug_fdc & LOG_FDC_BECKER) {
		// flush both hexdump logs
		log_hexdump_line(b->log_data_in_hex);
		log_hexdump_line(b->log_data_out_hex);
	}
	becker_poll_connect(b);
	fetch_input(b);
	if (b->input_buf_length > 0) {
		return 0x02;
	}
	return 0x00;
}

uint8_t becker_read_data(struct becker *b) {
	becker_poll_connect(b);
	fetch_input(b);
	if (b->input_buf_length == 0)
		return 0x00;
	uint8_t r = b->input_buf[b->input_buf_ptr++];
	if (b->input_buf_ptr == b->input_buf_length) {
		b->input_buf_ptr = b->input_buf_length = 0;
	}
	return r;
}

void becker_write_data(struct becker *b, uint8_t D) {
	becker_poll_connect(b);
	if (b->sockfd < 0)
		return;
	if (b->output_buf_length < OUTPUT_BUFFER_SIZE) {
		b->output_buf[b->output_buf_length++] = D;
	}
	write_output(b);
}

#else

extern inline struct becker *becker_open(void);
extern inline void becker_close(struct becker *b);
extern inline void becker_reset(struct becker *b);
extern inline uint8_t becker_read_status(struct becker *b);
extern inline uint8_t becker_read_data(struct becker *b);
extern inline void becker_write_data(struct becker *b, uint8_t D);

#endif
