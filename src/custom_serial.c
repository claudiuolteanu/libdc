/*
 * libdivecomputer
 *
 * Copyright (C) 2008 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <libdivecomputer/custom_serial.h>
#include <serial.h>

#include "context-private.h"

const dc_serial_operations_t native_serial_ops = {
	.open = serial_open,
	.close = serial_close,
	.read = serial_read,
	.write = serial_write,
	.flush = serial_flush,
	.get_received = serial_get_received,
	.get_transmitted = serial_get_transmitted
};


void
dc_serial_init (dc_serial_t *serial, void *data, const dc_serial_operations_t *ops)
{
	memset (serial, 0, sizeof (*serial));
	serial->data = data;
	serial->ops = ops;
}


dc_status_t
dc_serial_native_open(dc_serial_t **out, dc_context_t *context, const char *devname)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	dc_serial_t *serial = (dc_serial_t *) malloc (sizeof (dc_serial_t));

	if (serial == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	serial_t *port;

	// Open the serial device.
	int rc = serial_open (&port, context, devname);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (serial);
		return DC_STATUS_IO;
	}

	// Initialize data and function pointers
	dc_serial_init(serial, port, &native_serial_ops);

	// Set the type of the device
	serial->type = DC_TRANSPORT_SERIAL;

	*out = serial;

	return DC_STATUS_SUCCESS;
}
