/*
 * libdivecomputer
 *
 * Copyright (C) 2009 Jef Driesen
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

#include <stdlib.h> // malloc
#include <string.h> // memcmp, memcpy
#include <assert.h> // assert

#include "context-private.h"
#include "suunto_common2.h"
#include "ringbuffer.h"
#include "checksum.h"
#include "array.h"

#define MAXRETRIES 2

#define SZ_VERSION    0x04
#define SZ_PACKET     0x78
#define SZ_MINIMUM    8

#define RB_PROFILE_DISTANCE(l,a,b,m)  ringbuffer_distance (a, b, m, l->rb_profile_begin, l->rb_profile_end)

#define VTABLE(abstract)	((suunto_common2_device_vtable_t *) abstract->vtable)

void
suunto_common2_device_init (suunto_common2_device_t *device, dc_context_t *context, const suunto_common2_device_vtable_t *vtable)
{
	assert (device != NULL);

	// Initialize the base class.
	device_init (&device->base, context, &vtable->base);

	// Set the default values.
	device->layout = NULL;
	memset (device->version, 0, sizeof (device->version));
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
}


static dc_status_t
suunto_common2_transfer (dc_device_t *abstract, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize, unsigned int size)
{
	assert (asize >= size + 4);

	if (VTABLE (abstract)->packet == NULL)
		return DC_STATUS_UNSUPPORTED;

	// Occasionally, the dive computer does not respond to a command.
	// In that case we retry the command a number of times before
	// returning an error. Usually the dive computer will respond
	// again during one of the retries.

	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = VTABLE (abstract)->packet (abstract, command, csize, answer, asize, size)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_TIMEOUT && rc != DC_STATUS_PROTOCOL)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;
	}

	return rc;
}


dc_status_t
suunto_common2_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	suunto_common2_device_t *device = (suunto_common2_device_t*) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common2_device_version (dc_device_t *abstract, unsigned char data[], unsigned int size)
{
	if (size < SZ_VERSION) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_INVALIDARGS;
	}

	unsigned char answer[SZ_VERSION + 4] = {0};
	unsigned char command[4] = {0x0F, 0x00, 0x00, 0x0F};
	dc_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, sizeof (answer), 4);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	memcpy (data, answer + 3, SZ_VERSION);

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common2_device_reset_maxdepth (dc_device_t *abstract)
{
	unsigned char answer[4] = {0};
	unsigned char command[4] = {0x20, 0x00, 0x00, 0x20};
	dc_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, sizeof (answer), 0);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common2_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > SZ_PACKET)
			len = SZ_PACKET;

		// Read the package.
		unsigned char answer[SZ_PACKET + 7] = {0};
		unsigned char command[7] = {0x05, 0x00, 0x03,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // CRC
		command[6] = checksum_xor_uint8 (command, 6, 0x00);
		dc_status_t rc = suunto_common2_transfer (abstract, command, sizeof (command), answer, len + 7, len);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		memcpy (data, answer + 6, len);

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common2_device_write (dc_device_t *abstract, unsigned int address, const unsigned char data[], unsigned int size)
{
	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the package size.
		unsigned int len = size - nbytes;
		if (len > SZ_PACKET)
			len = SZ_PACKET;

		// Write the package.
		unsigned char answer[7] = {0};
		unsigned char command[SZ_PACKET + 7] = {0x06, 0x00, len + 3,
				(address >> 8) & 0xFF, // high
				(address     ) & 0xFF, // low
				len, // count
				0};  // data + CRC
		memcpy (command + 6, data, len);
		command[len + 6] = checksum_xor_uint8 (command, len + 6, 0x00);
		dc_status_t rc = suunto_common2_transfer (abstract, command, len + 7, answer, sizeof (answer), 0);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return DC_STATUS_SUCCESS;
}


dc_status_t
suunto_common2_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	suunto_common2_device_t *device = (suunto_common2_device_t *) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);

	// Erase the current contents of the buffer and
	// allocate the required amount of memory.
	if (!dc_buffer_clear (buffer) || !dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), SZ_PACKET);
}


dc_status_t
suunto_common2_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	suunto_common2_device_t *device = (suunto_common2_device_t*) abstract;

	assert (device != NULL);
	assert (device->layout != NULL);

	const suunto_common2_layout_t *layout = device->layout;

	// Error status for delayed errors.
	dc_status_t status = DC_STATUS_SUCCESS;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = layout->rb_profile_end - layout->rb_profile_begin +
		8 + (SZ_MINIMUM > 4 ? SZ_MINIMUM : 4);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read the serial number.
	unsigned char serial[SZ_MINIMUM > 4 ? SZ_MINIMUM : 4] = {0};
	dc_status_t rc = suunto_common2_device_read (abstract, layout->serial, serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory header.");
		return rc;
	}

	// Update and emit a progress event.
	progress.current += sizeof (serial);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->version[0];
	devinfo.firmware = array_uint24_be (device->version + 1);
	devinfo.serial = 0;
	for (unsigned int i = 0; i < 4; ++i) {
		devinfo.serial *= 100;
		devinfo.serial += serial[i];
	}
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	// Read the header bytes.
	unsigned char header[8] = {0};
	rc = suunto_common2_device_read (abstract, 0x0190, header, sizeof (header));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory header.");
		return rc;
	}

	// Obtain the pointers from the header.
	unsigned int last  = array_uint16_le (header + 0);
	unsigned int count = array_uint16_le (header + 2);
	unsigned int end   = array_uint16_le (header + 4);
	unsigned int begin = array_uint16_le (header + 6);
	if (last < layout->rb_profile_begin ||
		last >= layout->rb_profile_end ||
		end < layout->rb_profile_begin ||
		end >= layout->rb_profile_end ||
		begin < layout->rb_profile_begin ||
		begin >= layout->rb_profile_end)
	{
		ERROR (abstract->context, "Invalid ringbuffer pointer detected.");
		return DC_STATUS_DATAFORMAT;
	}

	// Memory buffer to store all the dives.

	unsigned char *data = (unsigned char *) malloc (layout->rb_profile_end - layout->rb_profile_begin + SZ_MINIMUM);
	if (data == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Calculate the total amount of bytes.

	unsigned int remaining = RB_PROFILE_DISTANCE (layout, begin, end, count != 0);

	// Update and emit a progress event.

	progress.maximum -= (layout->rb_profile_end - layout->rb_profile_begin) - remaining;
	progress.current += sizeof (header);
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// To reduce the number of read operations, we always try to read
	// packages with the largest possible size. As a consequence, the
	// last package of a dive can contain data from more than one dive.
	// Therefore, the remaining data of this package (and its size)
	// needs to be preserved for the next dive.

	unsigned int available = 0;

	// The ring buffer is traversed backwards to retrieve the most recent
	// dives first. This allows us to download only the new dives.

	unsigned int current = last;
	unsigned int previous = end;
	unsigned int address = previous;
	unsigned int offset = remaining + SZ_MINIMUM;
	while (remaining) {
		// Calculate the size of the current dive.
		unsigned int size = RB_PROFILE_DISTANCE (layout, current, previous, 1);

		if (size < 4 || size > remaining) {
			ERROR (abstract->context, "Unexpected profile size.");
			free (data);
			return DC_STATUS_DATAFORMAT;
		}

		unsigned int nbytes = available;
		while (nbytes < size) {
			// Handle the ringbuffer wrap point.
			if (address == layout->rb_profile_begin)
				address = layout->rb_profile_end;

			// Calculate the package size. Try with the largest possible
			// size first, and adjust when the end of the ringbuffer or
			// the end of the profile data is reached.
			unsigned int len = SZ_PACKET;
			if (layout->rb_profile_begin + len > address)
				len = address - layout->rb_profile_begin; // End of ringbuffer.
			if (nbytes + len > remaining)
				len = remaining - nbytes; // End of profile.
			/*if (nbytes + len > size)
				len = size - nbytes;*/ // End of dive (for testing only).

			// Move to the begin of the current package.
			offset -= len;
			address -= len;

			// Always read at least the minimum amount of bytes, because
			// reading fewer bytes is unreliable. The memory buffer is
			// large enough to prevent buffer overflows, and the extra
			// bytes are automatically ignored (due to reading backwards).
			unsigned int extra = 0;
			if (len < SZ_MINIMUM)
				extra = SZ_MINIMUM - len;

			// Read the package.
			rc = suunto_common2_device_read (abstract, address - extra, data + offset - extra, len + extra);
			if (rc != DC_STATUS_SUCCESS) {
				ERROR (abstract->context, "Failed to read the memory.");
				free (data);
				return rc;
			}

			// Update and emit a progress event.
			progress.current += len;
			device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

			// Next package.
			nbytes += len;
		}

		// The last package of the current dive contains the previous and
		// next pointers (in a continuous memory area). It can also contain
		// a number of bytes from the next dive.

		remaining -= size;
		available = nbytes - size;

		unsigned char *p = data + offset + available;
		unsigned int prev = array_uint16_le (p + 0);
		unsigned int next = array_uint16_le (p + 2);
		if (prev < layout->rb_profile_begin ||
			prev >= layout->rb_profile_end ||
			next < layout->rb_profile_begin ||
			next >= layout->rb_profile_end)
		{
			ERROR (abstract->context, "Invalid ringbuffer pointer detected.");
			free (data);
			return DC_STATUS_DATAFORMAT;
		}
		if (next != previous && next != current) {
			ERROR (abstract->context, "Profiles are not continuous.");
			free (data);
			return DC_STATUS_DATAFORMAT;
		}

		if (next != current) {
			unsigned int fp_offset = layout->fingerprint + 4;
			if (memcmp (p + fp_offset, device->fingerprint, sizeof (device->fingerprint)) == 0) {
				free (data);
				return DC_STATUS_SUCCESS;
			}

			if (callback && !callback (p + 4, size - 4, p + fp_offset, sizeof (device->fingerprint), userdata)) {
				free (data);
				return DC_STATUS_SUCCESS;
			}
		} else {
			ERROR (abstract->context, "Skipping incomplete dive.");
			status = DC_STATUS_DATAFORMAT;
		}

		// Next dive.
		previous = current;
		current = prev;
	}

	free (data);

	return status;
}
