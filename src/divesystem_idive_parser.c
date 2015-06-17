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

#include <stdlib.h>

#include <libdivecomputer/divesystem_idive.h>

#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_device_isinstance((parser), &divesystem_idive_parser_vtable)

#define SZ_HEADER 0x32
#define SZ_SAMPLE 0x2A

#define NGASMIXES 8

#define EPOCH 1199145600 /* 2008-01-01 00:00:00 */

typedef struct divesystem_idive_parser_t divesystem_idive_parser_t;

struct divesystem_idive_parser_t {
	dc_parser_t base;
	// Cached fields.
	unsigned int cached;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int ngasmixes;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
};

static dc_status_t divesystem_idive_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);
static dc_status_t divesystem_idive_parser_destroy (dc_parser_t *abstract);

static const dc_parser_vtable_t divesystem_idive_parser_vtable = {
	DC_FAMILY_DIVESYSTEM_IDIVE,
	divesystem_idive_parser_set_data, /* set_data */
	divesystem_idive_parser_get_datetime, /* datetime */
	divesystem_idive_parser_get_field, /* fields */
	divesystem_idive_parser_samples_foreach, /* samples_foreach */
	divesystem_idive_parser_destroy /* destroy */
};


dc_status_t
divesystem_idive_parser_create (dc_parser_t **out, dc_context_t *context)
{
	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) malloc (sizeof (divesystem_idive_parser_t));
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Initialize the base class.
	parser_init (&parser->base, context, &divesystem_idive_parser_vtable);

	// Set the default values.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_destroy (dc_parser_t *abstract)
{
	// Free memory.
	free (abstract);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->divetime = 0;
	parser->maxdepth = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->oxygen[i] = 0;
		parser->helium[i] = 0;
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	dc_ticks_t ticks = array_uint32_le(abstract->data + 7) + EPOCH;

	if (!dc_datetime_localtime (datetime, ticks))
        return DC_STATUS_DATAFORMAT;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	if (abstract->size < SZ_HEADER)
		return DC_STATUS_DATAFORMAT;

	if (!parser->cached) {
		dc_status_t rc = divesystem_idive_parser_samples_foreach (abstract, NULL, NULL);
		if (rc != DC_STATUS_SUCCESS)
			return rc;
	}

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			*((unsigned int *) value) = parser->divetime;
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = parser->maxdepth / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->helium = parser->helium[flags] / 100.0;
			gasmix->oxygen = parser->oxygen[flags] / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_le (data + 11) / 1000.0;
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
divesystem_idive_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	divesystem_idive_parser_t *parser = (divesystem_idive_parser_t *) abstract;
	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	unsigned int time = 0;
	unsigned int maxdepth = 0;
	unsigned int ngasmixes = 0;
	unsigned int oxygen[NGASMIXES];
	unsigned int helium[NGASMIXES];
	unsigned int o2_previous = 0xFFFFFFFF;
	unsigned int he_previous = 0xFFFFFFFF;

	unsigned int offset = SZ_HEADER;
	while (offset + SZ_SAMPLE <= size) {
		dc_sample_value_t sample = {0};

		// Time (seconds).
		unsigned int timestamp = array_uint32_le (data + offset + 2);
		if (timestamp <= time) {
			ERROR (abstract->context, "Timestamp moved backwards.");
			return DC_STATUS_DATAFORMAT;
		}
		time = timestamp;
		sample.time = timestamp;
		if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

		// Depth (1/10 m).
		unsigned int depth = array_uint16_le (data + offset + 6);
		if (maxdepth < depth)
			maxdepth = depth;
		sample.depth = depth / 10.0;
		if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

		// Temperature (Celsius).
		signed int temperature = (signed short) array_uint16_le (data + offset + 8);
		sample.temperature = temperature / 10.0;
		if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

		// Gaschange.
		unsigned int o2 = data[offset + 10];
		unsigned int he = data[offset + 11];
		if (o2 != o2_previous || he != he_previous) {
			// Find the gasmix in the list.
			unsigned int i = 0;
			while (i < ngasmixes) {
				if (o2 == oxygen[i] && he == helium[i])
					break;
				i++;
			}

			// Add it to list if not found.
			if (i >= ngasmixes) {
				if (i >= NGASMIXES) {
					ERROR (abstract->context, "Maximum number of gas mixes reached.");
					return DC_STATUS_DATAFORMAT;
				}
				oxygen[i] = o2;
				helium[i] = he;
				ngasmixes = i + 1;
			}

			sample.event.type = SAMPLE_EVENT_GASCHANGE2;
			sample.event.time = 0;
			sample.event.flags = 0;
			sample.event.value = o2 | (he << 16);
			if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			o2_previous = o2;
			he_previous = he;
		}

		// Deco stop / NDL.
		unsigned int deco = array_uint16_le (data + offset + 21);
		unsigned int tts  = array_uint16_le (data + offset + 23);
		if (tts != 0xFFFF) {
			if (deco) {
				sample.deco.type = DC_DECO_DECOSTOP;
				sample.deco.depth = deco / 10.0;
			} else {
				sample.deco.type = DC_DECO_NDL;
				sample.deco.depth = 0.0;
			}
			sample.deco.time = tts;
			if (callback) callback (DC_SAMPLE_DECO, sample, userdata);
		}

		// CNS
		unsigned int cns = array_uint16_le (data + offset + 29);
		sample.cns = cns / 100.0;
		if (callback) callback (DC_SAMPLE_CNS, sample, userdata);

		offset += SZ_SAMPLE;
	}

	// Cache the data for later use.
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->helium[i] = helium[i];
		parser->oxygen[i] = oxygen[i];
	}
	parser->ngasmixes = ngasmixes;
	parser->maxdepth = maxdepth;
	parser->divetime = time;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}
