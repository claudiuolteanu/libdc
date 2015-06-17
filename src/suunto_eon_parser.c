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

#include <stdlib.h>

#include <libdivecomputer/suunto_eon.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &suunto_eon_parser_vtable)

typedef struct suunto_eon_parser_t suunto_eon_parser_t;

struct suunto_eon_parser_t {
	dc_parser_t base;
	int spyder;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int marker;
	unsigned int nitrox;
};

static dc_status_t suunto_eon_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t suunto_eon_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t suunto_eon_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t suunto_eon_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t suunto_eon_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t suunto_eon_parser_vtable = {
	DC_FAMILY_SUUNTO_EON,
	suunto_eon_parser_set_data, /* set_data */
	suunto_eon_parser_get_datetime, /* datetime */
	suunto_eon_parser_get_field, /* fields */
	suunto_eon_parser_samples_foreach, /* samples_foreach */
	suunto_eon_parser_destroy /* destroy */
};

static dc_status_t
suunto_eon_parser_cache (suunto_eon_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	if (size < 13) {
		return DC_STATUS_DATAFORMAT;
	}

	// The Solution Nitrox/Vario stores nitrox data, not tank pressure.
	unsigned int nitrox = !parser->spyder && (data[4] & 0x80);

	// Parse the samples.
	unsigned int interval = data[3];
	unsigned int nsamples = 0;
	unsigned int depth = 0, maxdepth = 0;
	unsigned int offset = 11;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];
		if (value < 0x7d || value > 0x82) {
			depth += (signed char) value;
			if (depth > maxdepth)
				maxdepth = depth;
			nsamples++;
		}
	}

	// Check the end marker.
	unsigned int marker = offset;
	if (marker + 2 >= size || data[marker] != 0x80) {
		ERROR (abstract->context, "No valid end marker found!");
		return DC_STATUS_DATAFORMAT;
	}

	// Cache the data for later use.
	parser->divetime = nsamples * interval;
	parser->maxdepth = maxdepth;
	parser->marker = marker;
	parser->nitrox = nitrox;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

dc_status_t
suunto_eon_parser_create (dc_parser_t **out, dc_context_t *context, int spyder)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) malloc (sizeof (suunto_eon_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &suunto_eon_parser_vtable);

	// Set the default values.
	parser->spyder = spyder;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->marker = 0;
	parser->nitrox = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->marker = 0;
	parser->nitrox = 0;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	if (abstract->size < 6 + 5)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data + 6;

	if (datetime) {
		if (parser->spyder) {
			datetime->year   = p[0] + (p[0] < 90 ? 2000 : 1900);
			datetime->month  = p[1];
			datetime->day    = p[2];
			datetime->hour   = p[3];
			datetime->minute = p[4];
		} else {
			datetime->year   = bcd2dec (p[0]) + (bcd2dec (p[0]) < 85 ? 2000 : 1900);
			datetime->month  = bcd2dec (p[1]);
			datetime->day    = bcd2dec (p[2]);
			datetime->hour   = bcd2dec (p[3]);
			datetime->minute = bcd2dec (p[4]);
		}
		datetime->second = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;

	const unsigned char *data = abstract->data;

	// Cache the data.
	dc_status_t rc = suunto_eon_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;

	unsigned int oxygen = 21;
	unsigned int beginpressure = 0;
	unsigned int endpressure = 0;
	if (parser->nitrox) {
		oxygen = data[0x05];
	} else {
		beginpressure = data[5] * 2;
		endpressure   = data[parser->marker + 2] * 2;
	}

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth * FEET;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = oxygen / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			if (beginpressure == 0 && endpressure == 0)
				*((unsigned int *) value) = 0;
			else
				*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->gasmix = 0;
			tank->beginpressure = beginpressure;
			tank->endpressure = endpressure;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			if (parser->spyder)
				*((double *) value) = (signed char) data[parser->marker + 1];
			else
				*((double *) value) = data[parser->marker + 1] - 40;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
suunto_eon_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_eon_parser_t *parser = (suunto_eon_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;
	dc_sample_value_t sample = {0};

	// Cache the data.
	dc_status_t rc = suunto_eon_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Time
	sample.time = 0;
	if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

	unsigned int depth = 0;
	unsigned int time = 0;
	unsigned int interval = data[3];
	unsigned int complete = 1;
	unsigned int offset = 11;
	while (offset < size && data[offset] != 0x80) {
		unsigned char value = data[offset++];

		if (complete) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);
			complete = 0;
		}

		if (value < 0x7d || value > 0x82) {
			// Delta depth.
			depth += (signed char) value;

			// Depth (ft).
			sample.depth = depth * FEET;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			complete = 1;
		} else {
			// Event.
			sample.event.type = SAMPLE_EVENT_NONE;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = 0;
			switch (value) {
			case 0x7d: // Surface
				sample.event.type = SAMPLE_EVENT_SURFACE;
				break;
			case 0x7e: // Deco, ASC
				sample.event.type = SAMPLE_EVENT_DECOSTOP;
				break;
			case 0x7f: // Ceiling, ERR
				sample.event.type = SAMPLE_EVENT_CEILING;
				break;
			case 0x81: // Slow
				sample.event.type = SAMPLE_EVENT_ASCENT;
				break;
			default: // Unknown
				WARNING (abstract->context, "Unknown event");
				break;
			}

			if (sample.event.type != SAMPLE_EVENT_NONE) {
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}
		}
	}

	// Time
	if (complete) {
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);
	}

	// Depth (0 ft)
	sample.depth = 0;
	if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

	return DC_STATUS_SUCCESS;
}
