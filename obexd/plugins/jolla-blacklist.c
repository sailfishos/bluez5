/*
 *  Blacklist based file access control
 *
 *  Copyright (C) 2015 Jolla Ltd.
 *  Contact: Hannu Mallat <hannu.mallat@jollamobile.com>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <glib.h>

#include "log.h"
#include "access.h"
#include "plugin.h"

#define CONFIG_DIR "/etc/fsstorage.d"
#define CONFIG_SUFFIX ".xml"
#define CONFIG_SUFFIX_LEN 4

/*
   Use GMarkup which is limited but good enough for our purposes of
   parsing XML configuration files.

   XML file should contain a <storage> element and possible
   <blacklist> element(s) within. Anything unexpected or unrecognized,
   including unexpected text, will be treated as an error.

   Blacklist element text points to a file which contains blacklist
   data to be applied under storage path.

   Blacklist data may point to individual files as well as
   directories, meaning for the latter than any file under that path
   is blacklisted.

*/

struct blacklist_data {
	gchar *path;
	GPtrArray *elem;
};

#define PARSER_STACK_MAX 4

enum config_elem {
	CONFIG_ELEM_ROOT,
	CONFIG_ELEM_STORAGE,
	CONFIG_ELEM_BLACKLIST,

	N_CONFIG_ELEM
};

struct blacklist_config {
	gchar *storage_path;
	gchar *blacklist_file;
};

struct parser_state {
	enum config_elem stack[PARSER_STACK_MAX];
	gsize pos;
	gchar *storage_path;
	gchar *blacklist_file;
	GSList *configs;
};

typedef void (*parser_start_fn)(struct parser_state *state,
				const gchar **attribute_names,
				const gchar **attribute_values,
				GError **error);

typedef void (*parser_end_fn)(struct parser_state *state,
				GError **error);

static void start_storage(struct parser_state *state,
				const gchar **attribute_names,
				const gchar **attribute_values,
				GError **error);

static void start_blacklist(struct parser_state *state,
				const gchar **attribute_names,
				const gchar **attribute_values,
				GError **error);

static void end_storage(struct parser_state *state,
				GError **error);

static void end_blacklist(struct parser_state *state,
				GError **error);

static parser_start_fn start_transition[N_CONFIG_ELEM][N_CONFIG_ELEM] = {
	{ NULL, start_storage, NULL },
	{ NULL, NULL, start_blacklist },
	{ NULL, NULL, NULL },
};

static parser_end_fn end_transition[N_CONFIG_ELEM] = {
	NULL, end_storage, end_blacklist
};

static const gchar *config_elem_name[N_CONFIG_ELEM] = {
	"document root", "storage", "blacklist"
};

static GSList *blacklists = NULL;

static gchar *config_dir = NULL;

static gchar *trimmed_string(const gchar *string)
{
	const gchar *s = string, *e = string + strlen(string);

	while (g_ascii_isspace(*s)) s++;

	while (e > s && g_ascii_isspace(*(e - 1))) e--;

	if (s < e)
		return g_strndup(s, e - s);

	return NULL;
}

static gchar *memrchr(gchar *s, int c, int n)
{
	int i;

	for (i = n; i > 0; i--)
		if (s[i - 1] == c)
			return &s[i - 1];
	return NULL;
}

/* Quick and dirty absolute path string fixing, mostly to avoid config
   typos.  Would like to use realpath() but that requires that file
   actually exists at the time of the check. The following is done:
   consecutive slashes are compressed, "./" (or trailing ".") erased,
   "../" (or trailing "..") moves up one level (up to root). */
static gchar *normalized_path(const gchar *path)
{
	gchar *result = NULL;
	gchar *p, *r;

	DBG("'%s'", path);

	if (!path || *path != G_DIR_SEPARATOR)
		goto out;

	/* No modification lengthens the result, so we get away with this */
	result = g_strdup(path);
	p = result;
	r = result;

	while (1) {
		gchar *q;

		while (*p == G_DIR_SEPARATOR)
			p++;

		q = strchr(p, G_DIR_SEPARATOR);
		if (!q)
			q = p + strlen(p);

		if (q == p) {
			break;
		} else if (q - p == 1 && p[0] == '.') {
			/* . -> skip */
		} else if (q - p == 2 && p[0] == '.' && p[1] == '.') {
			/* .. -> back up one component if possible */
			char *s = memrchr(result, G_DIR_SEPARATOR, r - result);
			r = s > result ? s : result;
		} else {
			*r++ = G_DIR_SEPARATOR;
			memmove(r, p, q - p);
			r += q - p;
		}

		if (*q == '\0')
			break;
		p = q + 1;
	}

	if (r == result)
		*r++ = G_DIR_SEPARATOR;

	*r = '\0';

out:
	return result;
}

static void blacklist_debug(void)
{
	GSList *b;
	gsize i;

	for (b = blacklists; b; b = b->next) {
		struct blacklist_data *data = b->data;
		DBG("'%s'", data->path);
		for (i = 0; i < data->elem->len; i++) {
			DBG("\t%s", (gchar *)data->elem->pdata[i]);
		}
	}
}

static void blacklist_data_free(struct blacklist_data *data)
{
	g_ptr_array_unref(data->elem);
	g_free(data->path);
	g_free(data);
}

static void blacklist_clear(void)
{
	g_slist_free_full(blacklists, (GDestroyNotify)blacklist_data_free);
	blacklists = NULL;
}

static int blacklist_add(const gchar *storage_path,
			const gchar *blacklist_file)
{
	struct blacklist_data *data = NULL;
	GPtrArray *elem = NULL;
	GIOChannel *chan = NULL;
	gboolean done = FALSE;
	int r;

	if (!storage_path || !blacklist_file) {
		r = -EINVAL;
		goto out;
	}

	DBG("'%s' -> '%s'", storage_path, blacklist_file);

	chan = g_io_channel_new_file(blacklist_file, "r", NULL);
	if (!chan) {
		DBG("Cannot open blacklist file '%s'", blacklist_file);
		r = -EIO;
		goto out;
	}

	data = g_new0(struct blacklist_data, 1);
	data->path = normalized_path(storage_path);

	elem = g_ptr_array_new_with_free_func(g_free);
	while(!done) {
		/* #-lines are comments; rest is content that is trimmed */
		gchar *line = NULL;

		switch (g_io_channel_read_line(chan, &line, NULL, NULL, NULL)) {

		case G_IO_STATUS_AGAIN:
			break;

		case G_IO_STATUS_NORMAL:
			if (!line) {
				DBG("Error reading blacklist data '%s'",
					blacklist_file);
				r = -EIO;
				goto out;
			}

			if (*line != '#') {
				gchar *trim = trimmed_string(line);
				if (trim)
					g_ptr_array_add(elem, trim);
			}

			g_free(line);

			break;

		case G_IO_STATUS_EOF:
			data->elem = elem;
			blacklists = g_slist_append(blacklists, data);
			elem = NULL;
			data = NULL;
			done = TRUE;
			break;

		case G_IO_STATUS_ERROR:
			DBG("Error reading blacklist data '%s'",
				blacklist_file);
			r = -EIO;
			goto out;
			break;

		}
	}

	r = 0;

out:
	if (data)
		blacklist_data_free(data);
	if (chan)
		g_io_channel_unref(chan);
	if (elem)
		g_ptr_array_unref(elem);

	return r;
}

/* O(n) is poor form, but it's expected there's not much data, and
   storing the data in a more complex format has its own cost as
   well. */

static gboolean blacklist_match_under(const gchar *path,
				struct blacklist_data *data)
{
	gsize i;

	DBG("Checking '%s' under '%s'", path, data->path);

	for (i = 0; i < data->elem->len; i++) {
		gchar *check = data->elem->pdata[i];
		gsize check_len = strlen(check);

		if (g_str_has_prefix(path, check) &&
			(path[check_len] == '\0' ||
				path[check_len] == G_DIR_SEPARATOR)) {
			DBG("'%s' matches '%s'", path, check);
			return TRUE;
		}
	}

	DBG("No match.");
	return FALSE;
}

static gboolean blacklist_match(const gchar *raw_path)
{
	GSList *b;
	gchar *path;
	gboolean r = FALSE;

	DBG("'%s'", raw_path);
	path = normalized_path(raw_path);
	if (!path)
		goto out;

	for (b = blacklists; b; b = b->next) {
		struct blacklist_data *data = b->data;
		gsize path_len = strlen(data->path);

		if (g_str_has_prefix(path, data->path) &&
				path[path_len] == G_DIR_SEPARATOR) {
			const gchar *subpath = &path[path_len + 1];
			if (blacklist_match_under(subpath, data)) {
				r = TRUE;
				goto out;
			}
		}
	}

	DBG("No match.");

out:
	g_free(path);
	return r;
}

static void blacklist_config_free(struct blacklist_config *config)
{
	g_free(config->storage_path);
	g_free(config->blacklist_file);
	g_free(config);
}

static void start_storage(struct parser_state *state,
				const gchar **attr_name,
				const gchar **attr_value,
				GError **error)
{
	int i;

	for (i = 0; attr_name[i]; i++) {
		if (!strcasecmp(attr_name[i], "path")) {
			if (state->storage_path) {
				*error = g_error_new(G_MARKUP_ERROR,
						G_MARKUP_ERROR_INVALID_CONTENT,
						"Duplicate path attribute");
				return;
			}
			if (attr_value[i][0] != G_DIR_SEPARATOR) {
				*error = g_error_new(G_MARKUP_ERROR,
						G_MARKUP_ERROR_INVALID_CONTENT,
						"Relative path");
				return;
			}
			state->storage_path = trimmed_string(attr_value[i]);
		} else if (!strcasecmp(attr_name[i], "name")) {
			/* Ignore */
		} else if (!strcasecmp(attr_name[i], "description")) {
			/* Ignore */
		} else if (!strcasecmp(attr_name[i], "blockdev")) {
			/* Ignore */
		} else if (!strcasecmp(attr_name[i], "removable")) {
			/* Ignore */
		} else {
			*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
					"Unknown attribute '%s'", attr_name[i]);
			return;
		}
	}
}

static void end_storage(struct parser_state *state,
				GError **error)
{
	DBG("storage path: '%s'", state->storage_path);
	g_free(state->storage_path);
	state->storage_path = NULL;
}

static void start_blacklist(struct parser_state *state,
				const gchar **attr_name,
				const gchar **attr_value,
				GError **error)
{
	if (attr_name[0]) {
		*error = g_error_new(G_MARKUP_ERROR,
				G_MARKUP_ERROR_UNKNOWN_ATTRIBUTE,
				"Unknown attribute '%s'", attr_name[0]);
		return;
	}

	if (!state->storage_path) {
		*error = g_error_new(G_MARKUP_ERROR,
				G_MARKUP_ERROR_MISSING_ATTRIBUTE,
				"Missing path but <blacklist> present");
		return;
	}
}

static void end_blacklist(struct parser_state *state,
				GError **error)
{
	struct blacklist_config *config = NULL;

	if (!state->blacklist_file) {
		*error = g_error_new(G_MARKUP_ERROR,
				G_MARKUP_ERROR_INVALID_CONTENT,
				"Missing blacklist file definition");
		return;
	}

	DBG("blacklist file: '%s'", state->blacklist_file);
	config = g_new0(struct blacklist_config, 1);
	config->storage_path = g_strdup(state->storage_path);
	config->blacklist_file = trimmed_string(state->blacklist_file);
	g_free(state->blacklist_file);
	state->blacklist_file = NULL;
	state->configs = g_slist_append(state->configs, config);
}

static void xml_start_element(GMarkupParseContext *context,
				const gchar *element_name,
				const gchar **attr_names,
				const gchar **attr_values,
				gpointer user_data,
				GError **error)
{
	enum config_elem from, to;
	struct parser_state *state = user_data;
	int i;

	DBG("[%d] <%s>", (int)state->pos, element_name);

	if (state->pos == PARSER_STACK_MAX) {
		*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_INVALID_CONTENT,
					"Stack overflow");
		return;
	} else if (state->pos == 0) {
		*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_INVALID_CONTENT,
					"Bad stack");
		return;
	}

	for (i = CONFIG_ELEM_STORAGE; i < N_CONFIG_ELEM; i++) {
		if (!strcasecmp(element_name, config_elem_name[i])) {
			to = i;
			break;
		}
	}
	if (i == N_CONFIG_ELEM) {
		*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_UNKNOWN_ELEMENT,
					"Unknown element '%s'", element_name);
		return;
	}

	from = state->stack[state->pos - 1];

	if (start_transition[from][to] == NULL) {
		*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_INVALID_CONTENT,
					"<%s> inside <%s> not allowed",
					element_name,
					config_elem_name[from]);
		return;
	}

	state->stack[state->pos++] = to;
	(start_transition[from][to])(state, attr_names, attr_values, error);
}

static void xml_end_element(GMarkupParseContext *context,
				const gchar *element_name,
				gpointer user_data,
				GError **error)
{
	struct parser_state *state = user_data;

	DBG("[%d] </%s>", (int)state->pos, element_name);

	if (state->pos == 0) {
		*error = g_error_new(G_MARKUP_ERROR,
					G_MARKUP_ERROR_INVALID_CONTENT,
					"Stack underflow");
		return;
	}

	state->pos--;
	(end_transition[state->stack[state->pos]])(state, error);
}

static void xml_text(GMarkupParseContext *context,
			const gchar *text,
			gsize text_len,
			gpointer user_data,
			GError **error)
{
	struct parser_state *state = user_data;

	if (state->pos > 0 &&
			state->stack[state->pos - 1] == CONFIG_ELEM_BLACKLIST) {
		gsize curr_len = state->blacklist_file
			? strlen(state->blacklist_file)
			: 0;
		state->blacklist_file = g_realloc(state->blacklist_file,
						curr_len + text_len + 1);
		memcpy(&state->blacklist_file[curr_len], text, text_len + 1);
		
	} else {
		/* Whitespace is ignored; anything else triggers an error */
		gsize i;
		for (i = 0; i < text_len; i++) {
			if (!g_ascii_isspace(text[i])) {
				*error = g_error_new(G_MARKUP_ERROR,
						G_MARKUP_ERROR_INVALID_CONTENT,
						"Unexpected text %.*s",
						(int)text_len, text);
				return;
			}
		}
	}
}

static void xml_passthru(GMarkupParseContext *context,
				const gchar *text,
				gsize text_len,
				gpointer user_data,
				GError **error)
{
	struct parser_state *state = user_data;

	/* CDATA inside the document not supported */
	if (state->pos > 0 && state->stack[state->pos - 1] != CONFIG_ELEM_ROOT) {
		*error = g_error_new(G_MARKUP_ERROR,
				G_MARKUP_ERROR_INVALID_CONTENT,
				"Unexpected CDATA %.*s", (int)text_len, text);
		return;
	}
}

static int append_config(const gchar *config_file)
{
	GMarkupParser parser = {
		.start_element = xml_start_element,
		.end_element = xml_end_element,
		.text = xml_text,
		.passthrough = xml_passthru,
		.error = NULL
	};
	struct parser_state state = {
		.pos = 0,
		.storage_path = NULL,
		.blacklist_file = NULL,
		.configs = NULL
	};
	GMarkupParseContext *ctxt = NULL;
	gchar *buf = NULL;
	gsize len = 0;
	int r = -1;
	GSList *l;
	GError *error = NULL;

	if (!g_file_get_contents(config_file, &buf, &len, NULL)) {
		DBG("Cannot read configuration file '%s'", config_file);
		r = -EIO;
		goto out;
	}

	DBG("Opened configuration file '%s'", config_file);

	state.stack[state.pos++] = CONFIG_ELEM_ROOT;

	ctxt = g_markup_parse_context_new(&parser, 0, &state, NULL);
	if (!ctxt) {
		DBG("Cannot create XML parser");
		r = -EINVAL;
		goto out;
	}
	
	if (!g_markup_parse_context_parse(ctxt, buf, len, &error) ||
		!g_markup_parse_context_end_parse(ctxt, &error)) {
		DBG("Cannot parse configuration file '%s': %s",
			config_file, error->message);
		g_error_free(error);
		r = -EINVAL;
		goto out;
	}

	DBG("Parsed XML configuration file '%s'", config_file);

	/* Only add blacklist data after successful XML parsing */
	for (l = state.configs; l; l = l->next) {
		struct blacklist_config *config = l->data;
		r = blacklist_add(config->storage_path, config->blacklist_file);
		if (r < 0)
			goto out;
	}

	r = 0;

out:
	g_free(state.storage_path);
	g_free(state.blacklist_file);
	g_slist_free_full(state.configs, (GDestroyNotify)blacklist_config_free);

	if (ctxt)
		g_markup_parse_context_free(ctxt);

	g_free(buf);

	return r;
}

static int jolla_blacklist_check(const uint8_t *target,
					gsize target_len,
					enum access_op op,
					const gchar *object,
					gpointer user_data)
{
	/* Don't care which target is used; don't care which op is used */
	if (blacklist_match(object))
		return -EPERM;
	return 0;
}

static int jolla_blacklist_check_at(const uint8_t *target,
					gsize target_len,
					enum access_op op,
					const gchar *parent,
					const gchar *object,
					gpointer user_data)
{
	gchar *path = g_build_filename(parent, object, NULL);
	int r = jolla_blacklist_check(target, target_len, op, path, user_data);
	g_free(path);
	return r;
}

struct access_plugin jolla_blacklist_plugin = {
	jolla_blacklist_check,
	jolla_blacklist_check_at
};

static int jolla_blacklist_init(void)
{
	const gchar *name;
	gchar *path = config_dir ? config_dir : CONFIG_DIR;
	GDir *dir = NULL;
	int r;

	if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
		DBG("Cannot open configuration directory '%s'", path);
		r = -EIO;
		goto out;
	}

	dir = g_dir_open(path, 0, NULL);
	if (!dir) {
		DBG("Cannot open configuration directory '%s'", path);
		r = -EIO;
		goto out;
	}

	while ((name = g_dir_read_name(dir)) != NULL) {
		int len = strlen(name);
		if (len > CONFIG_SUFFIX_LEN &&
			!strcasecmp(name + len - CONFIG_SUFFIX_LEN,
					CONFIG_SUFFIX)) {
			gchar *config_file = g_build_filename(path, name, NULL);
			r = append_config(config_file);
			if (r < 0) {
				DBG("Cannot append config '%s'", config_file);
				goto out;
			}
			g_free(config_file);
		}
	}

	r = access_plugin_register("jolla_blacklist",
					&jolla_blacklist_plugin,
					NULL);
	if (r < 0) {
		DBG("Cannot register access plugin");
		goto out;
	}

	DBG("Blacklist configuration done.");
	blacklist_debug();

	r = 0;

out:
	if (dir)
		g_dir_close(dir);

	if (r < 0) {
		blacklist_clear();
	}

	return r;
}

static void jolla_blacklist_exit(void)
{
	access_plugin_unregister("jolla_blacklist");
	blacklist_clear();
}

OBEX_PLUGIN_DEFINE(jolla_blacklist, jolla_blacklist_init, jolla_blacklist_exit)
