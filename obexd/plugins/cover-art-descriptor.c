/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <errno.h>
#include <glib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../client/bip-common.h"
#include "cover-art-descriptor.h"

#define DESCRIPTOR_LIMIT 4096

struct descriptor_state {
	struct cover_art_request *request;
	unsigned int images;
	unsigned int roots;
	unsigned int depth;
	int result;
};

static int parse_size(const char *value, size_t *size)
{
	uint64_t n = 0;
	const unsigned char *p = (const unsigned char *) value;

	if (!*p)
		return -EBADR;
	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return -EBADR;
		if (n > (UINT64_MAX - (*p - '0')) / 10)
			return -EBADR;
		n = n * 10 + (*p - '0');
	}
	if (n > SIZE_MAX)
		return -EBADR;
	*size = n;
	return 0;
}

static const char *attribute(const gchar **names, const gchar **values,
		const char *name)
{
	unsigned int i;

	for (i = 0; names[i]; i++)
		if (!strcmp(names[i], name))
			return values[i];
	return NULL;
}

static gboolean allowed_attribute(const char *name, const char * const *allowed)
{
	unsigned int i;

	for (i = 0; allowed[i]; i++)
		if (!strcmp(name, allowed[i]))
			return TRUE;
	return FALSE;
}

static gboolean unique_attributes(const gchar **names)
{
	unsigned int i, j;

	for (i = 0; names[i]; i++)
		for (j = 0; j < i; j++)
			if (!strcmp(names[i], names[j]))
				return FALSE;
	return TRUE;
}

static void start_element(GMarkupParseContext *context, const gchar *name,
		const gchar **names, const gchar **values, gpointer user_data,
		GError **error)
{
	static const char * const root_attrs[] = { "version", NULL };
	static const char * const image_attrs[] = {
		"encoding", "pixel", "size", "maxsize", "transformation", NULL
	};
	struct descriptor_state *state = user_data;
	const char *value;
	unsigned int i, lower[2], upper[2], width, height;
	gboolean fixed_ratio;
	struct cover_art_request *request = state->request;
	size_t n;
	int unsupported = 0;

	state->depth++;
	if (state->depth == 1) {
		if (state->roots++ || strcmp(name, "image-descriptor") ||
				!unique_attributes(names))
			goto malformed;
		for (i = 0; names[i]; i++)
			if (!allowed_attribute(names[i], root_attrs))
				goto malformed;
		value = attribute(names, values, "version");
		if (!value)
			goto malformed;
		if (strcmp(value, "1.0")) {
			state->result = -ENOTSUP;
			return;
		}
		return;
	}
	if (state->depth != 2 || strcmp(name, "image") || state->images++)
		goto malformed;
	if (!unique_attributes(names))
		goto malformed;
	for (i = 0; names[i]; i++)
		if (!allowed_attribute(names[i], image_attrs))
			goto malformed;
	value = attribute(names, values, "encoding");
	if (!value)
		goto malformed;
	if (*value && strcmp(value, "JPEG"))
		unsupported = 1;
	value = attribute(names, values, "pixel");
	if (!value)
		goto malformed;
	if (*value) {
		if (!parse_pixel_range(value, lower, upper, &fixed_ratio))
			goto malformed;
		width = MIN(upper[0], COVER_ART_IMAGE_LIMIT);
		height = MIN(upper[1], COVER_ART_IMAGE_LIMIT);
		if (fixed_ratio && upper[0] && upper[1]) {
			width = MIN(width, (uint64_t) height * upper[0] /
							upper[1]);
			width = MIN(width, MAX(request->width, lower[0]));
			height = (uint64_t) width * upper[1] / upper[0];
		} else {
			width = MIN(width, MAX(request->width, lower[0]));
			height = MIN(height, MAX(request->height, lower[1]));
		}
		if (!width || !height || width < lower[0] || height < lower[1])
			unsupported = 1;
		request->width = width;
		request->height = height;
	}
	/* Size describes uploads; GetImage uses maxsize instead. */
	value = attribute(names, values, "size");
	if (value && parse_size(value, &n) < 0)
		goto malformed;
	value = attribute(names, values, "maxsize");
	if (value) {
		if (parse_size(value, &n) < 0)
			goto malformed;
		if (!n)
			unsupported = 1;
		request->maxsize = n;
	}
	value = attribute(names, values, "transformation");
	if (value && *value) {
		if (!strcmp(value, "stretch"))
			request->transform = COVER_ART_STRETCH;
		else if (!strcmp(value, "crop"))
			request->transform = COVER_ART_CROP;
		else if (strcmp(value, "fill"))
			unsupported = 1;
	}
	if (unsupported)
		state->result = -ENOTSUP;
	return;

malformed:
	state->result = -EBADR;
	if (error && !*error)
		g_set_error_literal(error, G_MARKUP_ERROR, G_MARKUP_ERROR_INVALID_CONTENT,
				"invalid image descriptor");
	(void) context;
}

static void end_element(GMarkupParseContext *context, const gchar *name,
		gpointer user_data, GError **error)
{
	struct descriptor_state *state = user_data;

	(void) context;
	(void) name;
	(void) error;
	if (state->depth)
		state->depth--;
}

static void text_data(GMarkupParseContext *context, const gchar *text,
		size_t len, gpointer user_data, GError **error)
{
	struct descriptor_state *state = user_data;
	size_t i;

	(void) context;
	for (i = 0; i < len; i++)
		if (!g_ascii_isspace(text[i])) {
			state->result = -EBADR;
			if (error && !*error)
				g_set_error_literal(error, G_MARKUP_ERROR,
					G_MARKUP_ERROR_INVALID_CONTENT,
					"unexpected descriptor text");
			return;
		}
}

int cover_art_descriptor_parse(const void *data, size_t len,
		unsigned int width, unsigned int height,
		struct cover_art_request *request)
{
	static const GMarkupParser parser = {
		start_element, end_element, text_data, NULL, NULL
	};
	struct descriptor_state state = { .request = request };
	GMarkupParseContext *context;
	GError *error = NULL;
	gboolean parsed;

	memset(request, 0, sizeof(*request));
	request->width = width;
	request->height = height;
	request->transform = COVER_ART_FILL;
	if (!len)
		return 0;
	if (!data || len > DESCRIPTOR_LIMIT || memchr(data, '\0', len))
		return -EBADR;
	context = g_markup_parse_context_new(&parser, G_MARKUP_TREAT_CDATA_AS_TEXT,
			&state, NULL);
	parsed = g_markup_parse_context_parse(context, data, len, &error);
	if (parsed)
		parsed = g_markup_parse_context_end_parse(context, &error);
	g_markup_parse_context_free(context);
	if (!parsed || error || state.roots != 1 ||
			!state.images || state.depth)
		state.result = -EBADR;
	g_clear_error(&error);
	if (state.result)
		return state.result;
	return state.images == 1 ? 0 : -EBADR;
}
