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

#include <stdlib.h>

#include <libdivecomputer/oceanic_vtpro.h>
#include <libdivecomputer/units.h>

#include "oceanic_common.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &oceanic_vtpro_parser_vtable)

typedef struct oceanic_vtpro_parser_t oceanic_vtpro_parser_t;

struct oceanic_vtpro_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	double maxdepth;
};

static dc_status_t oceanic_vtpro_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t oceanic_vtpro_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t oceanic_vtpro_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t oceanic_vtpro_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t oceanic_vtpro_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t oceanic_vtpro_parser_vtable = {
	DC_FAMILY_OCEANIC_VTPRO,
	oceanic_vtpro_parser_set_data, /* set_data */
	oceanic_vtpro_parser_get_datetime, /* datetime */
	oceanic_vtpro_parser_get_field, /* fields */
	oceanic_vtpro_parser_samples_foreach, /* samples_foreach */
	oceanic_vtpro_parser_destroy /* destroy */
};


dc_status_t
oceanic_vtpro_parser_create (dc_parser_t **out, dc_context_t *context)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	oceanic_vtpro_parser_t *parser = (oceanic_vtpro_parser_t *) malloc (sizeof (oceanic_vtpro_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &oceanic_vtpro_parser_vtable);

	// Set the default values.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	oceanic_vtpro_parser_t *parser = (oceanic_vtpro_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0.0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < 8)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		// The logbook entry can only store the last digit of the year field,
		// but the full year is also available in the dive header.
		if (abstract->size < 40)
			datetime->year = bcd2dec (p[4] & 0x0F) + 2000;
		else
			datetime->year = bcd2dec (((p[32 + 3] & 0xC0) >> 2) + ((p[32 + 2] & 0xF0) >> 4)) + 2000;
		datetime->month  = (p[4] & 0xF0) >> 4;
		datetime->day    = bcd2dec (p[3]);
		datetime->hour   = bcd2dec (p[1] & 0x7F);
		datetime->minute = bcd2dec (p[0]);
		datetime->second = 0;

		// Convert to a 24-hour clock.
		datetime->hour %= 12;
		if (p[1] & 0x80)
			datetime->hour += 12;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	oceanic_vtpro_parser_t *parser = (oceanic_vtpro_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		sample_statistics_t statistics = SAMPLE_STATISTICS_INITIALIZER;
		dc_status_t rc = oceanic_vtpro_parser_samples_foreach (
			abstract, sample_statistics_cb, &statistics);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		parser->cached = 1;
		parser->divetime = statistics.divetime;
		parser->maxdepth = statistics.maxdepth;
	}

	unsigned int footer = size - PAGESIZE;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (data[footer + 0] + ((data[footer + 1] & 0x0F) << 8)) * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			if (data[footer + 3])
				gasmix->oxygen = data[footer + 3] / 100.0;
			else
				gasmix->oxygen = 0.21;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
oceanic_vtpro_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 7 * PAGESIZE / 2)
		return DC_STATUS_DATAFORMAT;

	unsigned int time = 0;
	unsigned int interval = 0;
	switch ((data[0x27] >> 4) & 0x07) {
	case 0:
		interval = 2;
		break;
	case 1:
		interval = 15;
		break;
	case 2:
		interval = 30;
		break;
	case 3:
		interval = 60;
		break;
	default:
		interval = 0;
		break;
	}

	// Initialize the state for the timestamp processing.
	unsigned int timestamp = 0, count = 0, i = 0;

	unsigned int offset = 5 * PAGESIZE / 2;
	while (offset + PAGESIZE / 2 <= size - PAGESIZE) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, PAGESIZE / 2, 0x00)) {
			offset += PAGESIZE / 2;
			continue;
		}

		// Get the current timestamp.
		unsigned int current = bcd2dec (data[offset + 1] & 0x0F) * 60 + bcd2dec (data[offset + 0]);
		if (current < timestamp) {
			ERROR (abstract->context, "Timestamp moved backwards.");
			return DC_STATUS_DATAFORMAT;
		}

		if (current != timestamp || count == 0) {
			// A sample with a new timestamp.
			i = 0;
			if (interval) {
				// With a time based sample interval, the maximum number
				// of samples for a single timestamp is always fixed.
				count = 60 / interval;
			} else {
				// With a depth based sample interval, the exact number
				// of samples for a single timestamp needs to be counted.
				count = 1;
				unsigned int idx = offset + PAGESIZE / 2 ;
				while (idx + PAGESIZE / 2 <= size - PAGESIZE) {
					// Ignore empty samples.
					if (array_isequal (data + idx, PAGESIZE / 2, 0x00)) {
						idx += PAGESIZE / 2;
						continue;
					}

					unsigned int next = bcd2dec (data[idx + 1] & 0x0F) * 60 + bcd2dec (data[idx + 0]);
					if (next != current)
						break;

					idx += PAGESIZE / 2;
					count++;
				}
			}
		} else {
			// A sample with the same timestamp.
			i++;
		}

		if (interval) {
			if (current > timestamp + 1) {
				ERROR (abstract->context, "Unexpected timestamp jump.");
				return DC_STATUS_DATAFORMAT;
			}
			if (i >= count) {
				WARNING (abstract->context, "Unexpected sample with the same timestamp ignored.");
				offset += PAGESIZE / 2;
				continue;
			}
		}

		// Store the current timestamp.
		timestamp = current;

		// Time.
		if (interval)
			time += interval;
		else
			time = timestamp * 60 + (i + 1) * 60.0 / count + 0.5;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Vendor specific data
		sample.vendor.type = SAMPLE_VENDOR_OCEANIC_VTPRO;
		sample.vendor.size = PAGESIZE / 2;
		sample.vendor.data = data + offset;
		if (callback) callback (DC_SAMPLE_VENDOR, sample, userdata);

		// Depth (ft)
		unsigned int depth = data[offset + 3];
		sample.depth = depth * FEET;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (°F)
		unsigned int temperature = data[offset + 6];
		sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		offset += PAGESIZE / 2;
	}

	return DC_STATUS_SUCCESS;
}
