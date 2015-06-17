/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Jef Driesen
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

#include <string.h> // memcmp, memcpy
#include <stdlib.h> // malloc, free

#include <libdivecomputer/divesystem_idive.h>

#include "context-private.h"
#include "device-private.h"
#include "serial.h"
#include "checksum.h"
#include "array.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &divesystem_idive_device_vtable)

#define EXITCODE(rc) \
( \
	rc == -1 ? DC_STATUS_IO : DC_STATUS_TIMEOUT \
)

#define MAXRETRIES 9

#define MAXPACKET 0xFF
#define START     0x55
#define ACK       0x06
#define NAK       0x15
#define BUSY      0x60

#define CMD_ID     0x10
#define CMD_RANGE  0x98
#define CMD_HEADER 0xA0
#define CMD_SAMPLE 0xA8

#define SZ_ID     0x0A
#define SZ_RANGE  0x04
#define SZ_HEADER 0x32
#define SZ_SAMPLE 0x2A

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

typedef struct divesystem_idive_device_t {
	dc_device_t base;
	serial_t *port;
	unsigned char fingerprint[4];
} divesystem_idive_device_t;

static dc_status_t divesystem_idive_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t divesystem_idive_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t divesystem_idive_device_close (dc_device_t *abstract);

static const dc_device_vtable_t divesystem_idive_device_vtable = {
	DC_FAMILY_DIVESYSTEM_IDIVE,
	divesystem_idive_device_set_fingerprint, /* set_fingerprint */
	NULL, /* read */
	NULL, /* write */
	NULL, /* dump */
	divesystem_idive_device_foreach, /* foreach */
	divesystem_idive_device_close /* close */
};


dc_status_t
divesystem_idive_device_open (dc_device_t **out, dc_context_t *context, const char *name)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) malloc (sizeof (divesystem_idive_device_t));
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	device_init (&device->base, context, &divesystem_idive_device_vtable);

	// Set the default values.
	device->port = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	// Open the device.
	int rc = serial_open (&device->port, context, name);
	if (rc == -1) {
		ERROR (context, "Failed to open the serial port.");
		free (device);
		return DC_STATUS_IO;
	}

	// Set the serial communication protocol (115200 8N1).
	rc = serial_configure (device->port, 115200, 8, SERIAL_PARITY_NONE, 1, SERIAL_FLOWCONTROL_NONE);
	if (rc == -1) {
		ERROR (context, "Failed to set the terminal attributes.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Set the timeout for receiving data (1000ms).
	if (serial_set_timeout (device->port, 1000) == -1) {
		ERROR (context, "Failed to set the timeout.");
		serial_close (device->port);
		free (device);
		return DC_STATUS_IO;
	}

	// Make sure everything is in a sane state.
	serial_sleep (device->port, 300);
	serial_flush (device->port, SERIAL_QUEUE_BOTH);

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_device_close (dc_device_t *abstract)
{
	divesystem_idive_device_t *device = (divesystem_idive_device_t*) abstract;

	// Close the device.
	if (serial_close (device->port) == -1) {
		free (device);
		return DC_STATUS_IO;
	}

	// Free memory.
	free (device);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_send (divesystem_idive_device_t *device, const unsigned char command[], unsigned int csize)
{
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET + 4];
	unsigned short crc = 0;

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	if (csize < 1 || csize > MAXPACKET)
		return DC_STATUS_INVALIDARGS;

	// Setup the data packet
	packet[0] = START;
	packet[1] = csize;
	memcpy(packet + 2, command, csize);
	crc = checksum_crc_ccitt_uint16 (packet, csize + 2);
	packet[csize + 2] = (crc >> 8) & 0xFF;
	packet[csize + 3] = (crc     ) & 0xFF;

	// Send the data packet.
	int n = serial_write (device->port, packet, csize + 4);
	if (n != csize + 4) {
		ERROR (abstract->context, "Failed to send the command.");
		return EXITCODE (n);
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_receive (divesystem_idive_device_t *device, unsigned char answer[], unsigned int *asize)
{
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET + 4];
	int n = 0;

	if (asize == NULL || *asize < MAXPACKET) {
		ERROR (abstract->context, "Invalid arguments.");
		return DC_STATUS_INVALIDARGS;
	}

	// Read the packet start byte.
	while (1) {
		n = serial_read (device->port, packet + 0, 1);
		if (n != 1) {
			ERROR (abstract->context, "Failed to receive the packet start byte.");
			return EXITCODE (n);
		}

		if (packet[0] == START)
			break;
	}

	// Read the packet length.
	n = serial_read (device->port, packet + 1, 1);
	if (n != 1) {
		ERROR (abstract->context, "Failed to receive the packet length.");
		return EXITCODE (n);
	}

	unsigned int len = packet[1];
	if (len < 2 || len > MAXPACKET) {
		ERROR (abstract->context, "Invalid packet length.");
		return DC_STATUS_PROTOCOL;
	}

	// Read the packet payload and checksum.
	n = serial_read (device->port, packet + 2, len + 2);
	if (n != len + 2) {
		ERROR (abstract->context, "Failed to receive the packet payload and checksum.");
		return EXITCODE (n);
	}

	// Verify the checksum.
	unsigned short crc = array_uint16_be (packet + len + 2);
	unsigned short ccrc = checksum_crc_ccitt_uint16 (packet, len + 2);
	if (crc != ccrc) {
		ERROR (abstract->context, "Unexpected packet checksum.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy(answer, packet + 2, len);
	*asize = len;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_transfer (divesystem_idive_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned char packet[MAXPACKET] = {0};
	unsigned int length = 0;
	unsigned int nretries = 0;

	while (1) {
		// Send the command.
		rc = divesystem_idive_send (device, command, csize);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Receive the answer.
		length = sizeof(packet);
		rc = divesystem_idive_receive (device, packet, &length);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		// Verify the command byte.
		if (packet[0] != command[0]) {
			ERROR (abstract->context, "Unexpected packet header.");
			return DC_STATUS_PROTOCOL;
		}

		// Check the ACK byte.
		if (packet[length - 1] == ACK)
			break;

		// Verify the NAK byte.
		if (packet[length - 1] != NAK) {
			ERROR (abstract->context, "Unexpected ACK/NAK byte.");
			return DC_STATUS_PROTOCOL;
		}

		// Verify the length of the packet.
		if (length != 3) {
			ERROR (abstract->context, "Unexpected packet length.");
			return DC_STATUS_PROTOCOL;
		}

		// Verify the error code.
		unsigned int errcode = packet[1];
		if (errcode != BUSY) {
			ERROR (abstract->context, "Received NAK packet with error code %02x.", errcode);
			return DC_STATUS_PROTOCOL;
		}

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return DC_STATUS_PROTOCOL;

		// Delay the next attempt.
		serial_sleep(device->port, 100);
	}

	// Verify the length of the packet.
	if (asize != length - 2) {
		ERROR (abstract->context, "Unexpected packet length.");
		return DC_STATUS_PROTOCOL;
	}

	memcpy(answer, packet + 1, length - 2);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
divesystem_idive_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	divesystem_idive_device_t *device = (divesystem_idive_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	unsigned char cmd_id[] = {CMD_ID, 0xED};
	unsigned char id[SZ_ID];
	rc = divesystem_idive_transfer (device, cmd_id, sizeof(cmd_id), id, sizeof(id));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = array_uint16_le (id);
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (id + 6);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = id;
	vendor.size = sizeof (id);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	unsigned char cmd_range[] = {CMD_RANGE, 0x8D};
	unsigned char range[4];
	rc = divesystem_idive_transfer (device, cmd_range, sizeof(cmd_range), range, sizeof(range));
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Get the range of the available dive numbers.
	unsigned int first = array_uint16_le (range + 0);
	unsigned int last  = array_uint16_le (range + 2);
	if (first > last) {
		ERROR(abstract->context, "Invalid dive numbers.");
		return DC_STATUS_DATAFORMAT;
	}

	// Calculate the number of dives.
	unsigned int ndives = last - first + 1;

	// Update and emit a progress event.
	progress.maximum = ndives * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	dc_buffer_t *buffer = dc_buffer_new(0);
	if (buffer == NULL) {
		return DC_STATUS_NOMEMORY;
	}

	for (unsigned int i = 0; i < ndives; ++i) {
		unsigned int number = last - i;
		unsigned char cmd_header[] = {CMD_HEADER,
			(number     ) & 0xFF,
			(number >> 8) & 0xFF};
		unsigned char header[SZ_HEADER];
		rc = divesystem_idive_transfer (device, cmd_header, sizeof(cmd_header), header, sizeof(header));
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		if (memcmp(header + 7, device->fingerprint, sizeof(device->fingerprint)) == 0)
			break;

		unsigned int nsamples = array_uint16_le (header + 1);

		// Update and emit a progress event.
		progress.current = i * NSTEPS + STEP(1, nsamples + 1);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		dc_buffer_clear(buffer);
		dc_buffer_reserve(buffer, SZ_HEADER + SZ_SAMPLE * nsamples);
		dc_buffer_append(buffer, header, sizeof(header));

		for (unsigned int j = 0; j < nsamples; ++j) {
			unsigned int idx = j + 1;
			unsigned char cmd_sample[] = {CMD_SAMPLE,
				(idx     ) & 0xFF,
				(idx >> 8) & 0xFF};
			unsigned char sample[SZ_SAMPLE];
			rc = divesystem_idive_transfer (device, cmd_sample, sizeof(cmd_sample), sample, sizeof(sample));
			if (rc != DC_STATUS_SUCCESS)
				return rc;

			// Update and emit a progress event.
			progress.current = i * NSTEPS + STEP(j + 2, nsamples + 1);
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			dc_buffer_append(buffer, sample, sizeof(sample));
		}

		unsigned char *data = dc_buffer_get_data(buffer);
		unsigned int   size = dc_buffer_get_size(buffer);
		if (callback && !callback (data, size, data + 7, sizeof(device->fingerprint), userdata)) {
			dc_buffer_free (buffer);
			return DC_STATUS_SUCCESS;
		}
	}

	dc_buffer_free(buffer);

	return DC_STATUS_SUCCESS;
}
