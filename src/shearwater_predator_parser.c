/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
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
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#define snprintf _snprintf
#endif

#include <libdivecomputer/shearwater_predator.h>
#include <libdivecomputer/shearwater_petrel.h>
#include <libdivecomputer/units.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser)	( \
	dc_parser_isinstance((parser), &shearwater_predator_parser_vtable) || \
	dc_parser_isinstance((parser), &shearwater_petrel_parser_vtable))

#define SZ_BLOCK   0x80
#define SZ_SAMPLE_PREDATOR  0x10
#define SZ_SAMPLE_PETREL    0x20

#define METRIC   0
#define IMPERIAL 1

#define NGASMIXES 10

typedef struct shearwater_predator_parser_t shearwater_predator_parser_t;

struct shearwater_predator_parser_t {
	dc_parser_t base;
	unsigned int petrel;
	// Cached fields.
	unsigned int cached;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int serial;
};

static dc_status_t shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t shearwater_predator_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t shearwater_predator_parser_vtable = {
	DC_FAMILY_SHEARWATER_PREDATOR,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	shearwater_predator_parser_destroy /* destroy */
};

static const dc_parser_vtable_t shearwater_petrel_parser_vtable = {
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	shearwater_predator_parser_destroy /* destroy */
};


dc_status_t
shearwater_common_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial, unsigned int petrel)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) malloc (sizeof (shearwater_predator_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser->petrel = petrel;
	parser->serial = serial;
	if (petrel) {
		parser_init (&parser->base, context, &shearwater_petrel_parser_vtable);
	} else {
		parser_init (&parser->base, context, &shearwater_predator_parser_vtable);
	}

	// Set the default values.
	parser->cached = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, serial, 0);
}


dc_status_t
shearwater_petrel_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, serial, 1);
}


static dc_status_t
shearwater_predator_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2 * SZ_BLOCK)
		return DC_STATUS_DATAFORMAT;

	unsigned int ticks = array_uint32_be (data + 12);

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


#define BUFLEN 16

static dc_status_t
shearwater_predator_parser_cache (shearwater_predator_parser_t *parser)
{
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	// Get the gas mixes.
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES] = {0};
	unsigned int helium[NGASMIXES] = {0};
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		unsigned int o2 = data[20 + i];
		unsigned int he = data[30 + i];
		if (o2 == 0 && he == 0)
			continue;
		oxygen[ngasmixes] = o2;
		helium[ngasmixes] = he;
		ngasmixes++;
	}

	// Cache the data for later use.
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->oxygen[i] = oxygen[i];
		parser->helium[i] = helium[i];
	}
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2 * SZ_BLOCK)
		return DC_STATUS_DATAFORMAT;

	// Get the offset to the footer record.
	unsigned int footer = size - SZ_BLOCK;
	if (parser->petrel || array_uint16_be (data + footer) == 0xFFFD) {
		if (size < 3 * SZ_BLOCK)
			return DC_STATUS_DATAFORMAT;

		footer -= SZ_BLOCK;
	}

	// Get the unit system.
	unsigned int units = data[8];

	// Cache the gas mix data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;
	unsigned int density = 0;
	char buf[BUFLEN];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = array_uint16_be (data + footer + 6) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (units == IMPERIAL)
				*((double *) value) = array_uint16_be (data + footer + 4) * FEET;
			else
				*((double *) value) = array_uint16_be (data + footer + 4);
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_SALINITY:
			density = array_uint16_be (data + 83);
			if (density == 1000)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = density;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_be (data + 47) / 1000.0;
			break;
		case DC_FIELD_STRING:
			switch(flags) {
			case 0: // Battery
				string->desc = "Battery at end";
				snprintf(buf, BUFLEN, "%.1f", data[9] / 10.0);
				break;
			case 1: // Serial
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%08x", parser->serial);
				break;
			case 2: // FW Version
				string->desc = "FW Version";
				snprintf(buf, BUFLEN, "%2x", data[19]);
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
			string->value = strdup(buf);
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	if (size < 2 * SZ_BLOCK)
		return DC_STATUS_DATAFORMAT;

	// Get the offset to the footer record.
	unsigned int footer = size - SZ_BLOCK;
	if (parser->petrel || array_uint16_be (data + footer) == 0xFFFD) {
		if (size < 3 * SZ_BLOCK)
			return DC_STATUS_DATAFORMAT;

		footer -= SZ_BLOCK;
	}

	// Get the sample size.
	unsigned int samplesize = SZ_SAMPLE_PREDATOR;
	if (parser->petrel) {
		samplesize = SZ_SAMPLE_PETREL;
	}

	// Get the unit system.
	unsigned int units = data[8];

	// Previous gas mix.
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int time = 0;
	unsigned int offset = SZ_BLOCK;
	while (offset < footer) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, samplesize, 0x00)) {
			offset += samplesize;
			continue;
		}

		// Time (seconds).
		time += 10;
		sample.time = time;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m or ft).
		unsigned int depth = array_uint16_be (data + offset);
		if (units == IMPERIAL)
			sample.depth = depth * FEET / 10.0;
		else
			sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (°C or °F).
		unsigned int temperature = data[offset + 13];
		if (units == IMPERIAL)
			sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
		else
			sample.temperature = temperature;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// PPO2
		sample.ppo2 = data[offset + 6] / 100.0;
		if (callback) callback (DC_SAMPLE_PPO2, sample, userdata);

		// CNS
		if (parser->petrel) {
			sample.cns = data[offset + 22] / 100.0;
			if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
		}

		// Gaschange.
		unsigned int o2 = data[offset + 7];
		unsigned int he = data[offset + 8];
		if (o2 != o2_previous || he != he_previous) {
			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = o2 | (he << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int decostop = array_uint16_be (data + offset + 2);
		if (decostop) {
			sample.deco.type = DC_DECO_DECOSTOP;
			if (units == IMPERIAL)
				sample.deco.depth = decostop * FEET;
			else
				sample.deco.depth = decostop;
		} else {
			sample.deco.type = DC_DECO_NDL;
			sample.deco.depth = 0.0;
		}
		sample.deco.time = data[offset + 9] * 60;
		if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

		offset += samplesize;
	}

	return DC_STATUS_SUCCESS;
}
