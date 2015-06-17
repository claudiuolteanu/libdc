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
#include <string.h>	// memcmp

#include <libdivecomputer/reefnet_sensusultra.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &reefnet_sensusultra_parser_vtable)

typedef struct reefnet_sensusultra_parser_t reefnet_sensusultra_parser_t;

struct reefnet_sensusultra_parser_t {
	dc_parser_t base;
	// Depth calibration.
	double atmospheric;
	double hydrostatic;
	// Clock synchronization.
	unsigned int devtime;
	dc_ticks_t systime;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
};

static dc_status_t reefnet_sensusultra_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t reefnet_sensusultra_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t reefnet_sensusultra_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t reefnet_sensusultra_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t reefnet_sensusultra_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t reefnet_sensusultra_parser_vtable = {
	DC_FAMILY_REEFNET_SENSUSULTRA,
	reefnet_sensusultra_parser_set_data, /* set_data */
	reefnet_sensusultra_parser_get_datetime, /* datetime */
	reefnet_sensusultra_parser_get_field, /* fields */
	reefnet_sensusultra_parser_samples_foreach, /* samples_foreach */
	reefnet_sensusultra_parser_destroy /* destroy */
};


dc_status_t
reefnet_sensusultra_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int devtime, dc_ticks_t systime)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) malloc (sizeof (reefnet_sensusultra_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &reefnet_sensusultra_parser_vtable);

	// Set the default values.
	parser->atmospheric = ATM;
	parser->hydrostatic = 1025.0 * GRAVITY;
	parser->devtime = devtime;
	parser->systime = systime;
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;

	return DC_STATUS_SUCCESS;
}


dc_status_t
reefnet_sensusultra_parser_set_calibration (dc_parser_t *abstract, double atmospheric, double hydrostatic)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	if (!ISINSTANCE (abstract))
		return DC_STATUS_INVALIDARGS;

	parser->atmospheric = atmospheric;
	parser->hydrostatic = hydrostatic;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	if (abstract->size < 4 + 4)
		return DC_STATUS_DATAFORMAT;

	unsigned int timestamp = array_uint32_le (abstract->data + 4);

	dc_ticks_t ticks = parser->systime - (parser->devtime - timestamp);

	if (!dc_datetime_localtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t *) abstract;

	if (abstract->size < 20)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

		const unsigned char *data = abstract->data;
		unsigned int size = abstract->size;

		unsigned int interval = array_uint16_le (data + 8);
		unsigned int threshold = array_uint16_le (data + 10);

		unsigned int maxdepth = 0;
		unsigned int nsamples = 0;
		unsigned int offset = 16;
		while (offset + sizeof (footer) <= size &&
			memcmp (data + offset, footer, sizeof (footer)) != 0)
		{
			unsigned int depth = array_uint16_le (data + offset + 2);
			if (depth >= threshold) {
				if (depth > maxdepth)
					maxdepth = depth;
				nsamples++;
			}

			offset += 4;
		}

		parser->cached = 1;
		parser->divetime = nsamples * interval;
		parser->maxdepth = maxdepth;
	}

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = (parser->maxdepth * BAR / 1000.0 - parser->atmospheric) / parser->hydrostatic;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = 0;
			break;
		case DC_FIELD_DIVEMODE:
			*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
reefnet_sensusultra_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	reefnet_sensusultra_parser_t *parser = (reefnet_sensusultra_parser_t*) abstract;

	const unsigned char header[4] = {0x00, 0x00, 0x00, 0x00};
	const unsigned char footer[4] = {0xFF, 0xFF, 0xFF, 0xFF};

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int offset = 0;
	while (offset + sizeof (header) <= size) {
		if (memcmp (data + offset, header, sizeof (header)) == 0) {
			if (offset + 16 > size)
				return DC_STATUS_DATAFORMAT;

			unsigned int time = 0;
			unsigned int interval = array_uint16_le (data + offset + 8);

			offset += 16;
			while (offset + sizeof (footer) <= size &&
				memcmp (data + offset, footer, sizeof (footer)) != 0)
			{
				dc_sample_value_t sample = {0};

				// Time (seconds)
				time += interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

				// Temperature (0.01 °K)
				unsigned int temperature = array_uint16_le (data + offset);
				sample.temperature = temperature / 100.0 - 273.15;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

				// Depth (absolute pressure in millibar)
				unsigned int depth = array_uint16_le (data + offset + 2);
				sample.depth = (depth * BAR / 1000.0 - parser->atmospheric) / parser->hydrostatic;
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

				offset += 4;
			}
			break;
		} else {
			offset++;
		}
	}

	return DC_STATUS_SUCCESS;
}
