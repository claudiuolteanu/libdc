/*
 * libdivecomputer
 *
 * Copyright (C) 2013 Jef Driesen
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

#include <libdivecomputer/cressi_leonardo.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &cressi_leonardo_parser_vtable)

#define SZ_HEADER 82

typedef struct cressi_leonardo_parser_t cressi_leonardo_parser_t;

struct cressi_leonardo_parser_t {
	dc_parser_t base;
};

static dc_status_t cressi_leonardo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t cressi_leonardo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t cressi_leonardo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t cressi_leonardo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t cressi_leonardo_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t cressi_leonardo_parser_vtable = {
	DC_FAMILY_CRESSI_EDY,
	cressi_leonardo_parser_set_data, /* set_data */
	cressi_leonardo_parser_get_datetime, /* datetime */
	cressi_leonardo_parser_get_field, /* fields */
	cressi_leonardo_parser_samples_foreach, /* samples_foreach */
	cressi_leonardo_parser_destroy /* destroy */
};


dc_status_t
cressi_leonardo_parser_create (dc_parser_t **out, dc_context_t *context)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	cressi_leonardo_parser_t *parser = (cressi_leonardo_parser_t *) malloc (sizeof (cressi_leonardo_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &cressi_leonardo_parser_vtable);

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *p = abstract->data;

	if (datetime) {
		datetime->year = p[8] + 2000;
		datetime->month = p[9];
		datetime->day = p[10];
		datetime->hour = p[11];
		datetime->minute = p[12];
		datetime->second = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *data = abstract->data;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_le (data + 0x06) * 20;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (data + 0x20) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 1;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = 0.0;
			gasmix->oxygen = data[0x19] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = data[0x22];
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
cressi_leonardo_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int interval = 20;

	unsigned int offset = SZ_HEADER;
	while (offset + 2 <= size) {
		dc_sample_value_t sample = {0};

		unsigned int value = array_uint16_le (data + offset);
		unsigned int depth = value & 0x07FF;
		unsigned int ascent = (value & 0xC000) >> 14;

		// Time (seconds).
		time += interval;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m).
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Ascent rate
		if (ascent) {
			sample.event.type = SAMPLE_EVENT_ASCENT;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = ascent;
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
		}

		offset += 2;
	}

	return DC_STATUS_SUCCESS;
}
