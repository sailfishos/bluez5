/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2015 Jolla Ltd. All rights reserved.
 *  Contact: Hannu Mallat <hannu.mallat@jollamobile.com>
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

/* Plugin for reading software version dynamically; used for DI
   profile version ID. Note that DI profile vendor and product IDs are
   always static and assigned in src/main.c:parse_did() outside of
   this plugin */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"
#include "src/plugin.h"
#include "src/log.h"
#include "src/adapter.h"
#include "src/hcid.h"
#include "src/sdpd.h"

static GKeyFile *load_config(const char *file)
{
	GError *err = NULL;
	GKeyFile *keyfile;

	keyfile = g_key_file_new();

	g_key_file_set_list_separator(keyfile, ',');

	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		error("Parsing %s failed: %s", file, err->message);
		g_error_free(err);
		g_key_file_free(keyfile);
		return NULL;
	}

	return keyfile;
}

/* Would be nice if there was a proper BNF for /etc/os-release */

static gsize skip_whitespace(gchar *buf, gsize len, gsize pos)
{
	gsize cur = pos;
	while (cur < len && (buf[cur] == ' ' || buf[cur] == '\t'))
		cur++;
	return cur - pos;
}

static gsize skip_until_eol(gchar *buf, gsize len, gsize pos)
{
	gsize cur = pos;
	while (cur < len && buf[cur] != '\n')
		cur++;
	return cur - pos;
}

static gsize expect_char(gchar *buf, gsize len, gsize pos, gchar val)
{
	if (pos >= len || buf[pos] != val)
		return -1;
	return 1;
}

#define FIRST_CHAR 0x1
#define REST_CHAR 0x2

static gsize read_variable_name(gchar *buf, gsize len, gsize pos)
{
	static uint8_t name_chars[256];
	static gboolean name_chars_inited = FALSE;
	gsize cur = pos;

	if (!name_chars_inited) {
		int i;
		memset(name_chars, 0, sizeof(name_chars));
		name_chars[(int)'_'] = FIRST_CHAR | REST_CHAR;
		for (i = 'A'; i <= 'Z'; i++)
			name_chars[i] = FIRST_CHAR | REST_CHAR;
		for (i = 'a'; i <= 'z'; i++)
			name_chars[i] = FIRST_CHAR | REST_CHAR;
		for (i = '0'; i <= '9'; i++)
			name_chars[i] = REST_CHAR;
		name_chars_inited = TRUE;
	}

	if (cur >= len || !(name_chars[(int)buf[cur]] & FIRST_CHAR))
		return -1;

	cur++;
	while (cur < len && (name_chars[(int)buf[cur]] & REST_CHAR))
		cur++;

	return cur - pos;
}

static gsize read_line(GHashTable *hash, gchar *buf, gsize len, gsize pos)
{
	gsize cur = pos;
	gsize chunk;
	gchar *name = NULL;
	gchar *value = NULL;

	if (buf[cur] == '#') {
		cur += skip_until_eol(buf, len, cur);
		cur++;
		goto out;
	}

	cur += skip_whitespace(buf, len, cur);

	chunk = read_variable_name(buf, len, cur);
	if (chunk < 0) {
		warn("Invalid name at position %u, skipping line", cur);
		cur += skip_until_eol(buf, len, cur);
		cur++;
		goto out;
	}
	name = g_strndup(&buf[cur], chunk);
	DBG("Found variable name '%s'", name);
	cur += chunk;

	chunk = expect_char(buf, len, cur, '=');
	if (chunk < 0) {
		warn("Assignment not found at position %u, skipping line", cur);
		cur += skip_until_eol(buf, len, cur);
		cur++;
		goto out;
	}
	cur += chunk;

	chunk = skip_until_eol(buf, len, cur);
	value = g_strndup(&buf[cur], chunk);
	DBG("Found unprocessed variable value '%s'", value);
	cur += chunk;
	cur++;

	g_hash_table_replace(hash, name, value);
	name = NULL;
	value = NULL;

out:
	g_free(name);
	g_free(value);
	return MIN(len, cur) - pos;
}

static GHashTable *load_os_release(void)
{
	GHashTable *hash = NULL, *result = NULL;
	gchar *buf = NULL;
	gsize len = 0;
	gsize pos = 0;

	if (!g_file_get_contents("/etc/os-release", &buf, &len, NULL)) {
		error("Cannot read /etc/os-release");
		goto cleanup;
	}

	hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	while (pos < len) {
		gsize chunk = read_line(hash, buf, len, pos);
		if (!chunk) {
			error("Error parsing /etc/os-release (offset %u)", pos);
			goto cleanup;
		}
		pos += chunk;
	}

	result = hash;
	hash = NULL;

cleanup:
	if (hash)
		g_hash_table_destroy(hash);

	g_free(buf);

	return result;
}

static int os_version(unsigned int *maj,
			unsigned int *min,
			unsigned int *sub,
			unsigned int *bld)
{
	GHashTable *hash = NULL;
	char *verstr = NULL;
	int r = -1;

	hash = load_os_release();
	if (!hash) {
		error("Cannot read OS version");
		goto out;
	}

	verstr = g_hash_table_lookup(hash, "VERSION_ID");
	if (!verstr) {
		error("No VERSION_ID found");
		goto out;
	}

	DBG("Read version string '%s'", verstr);

	*maj = *min = *sub = *bld = 0;
	if (sscanf(verstr, " %u.%u.%u.%u", maj, min, sub, bld) < 3) {
		error("Cannot fully parse version string '%s'", verstr);
	} else {
		DBG("Version %u.%u.%u.%u", *maj, *min, *sub, *bld);
		r = 0;
	}

out:
	if (hash)
		g_hash_table_destroy(hash);

	return r;
}

static void set_did(struct btd_adapter *adapter, gpointer user_data)
{
	(void) user_data;

	DBG("%p", adapter);
	if (main_opts.did_source)
		btd_adapter_set_did(adapter, main_opts.did_vendor,
					main_opts.did_product,
					main_opts.did_version,
					main_opts.did_source);
}

static int jolla_did_init(void)
{
	GKeyFile *config;
	gboolean dynver = FALSE;

	DBG("");

	config = load_config(CONFIGDIR "/jolla.conf");
	if (config) {
		dynver = g_key_file_get_boolean(config, "General",
						"DeviceIDDynamicVersion", NULL);
		g_key_file_free(config);
	}

	DBG("Dynamic DI version %sconfigured.", dynver ? "" : "not ");

	if (dynver) {
		unsigned int maj, min, sub, bld;
		if (!os_version(&maj, &min, &sub, &bld)) {
			uint16_t bcd = 0;
			if (maj > 99 || min > 9 || sub > 9)
				error("Clamping BCD for OS version %u.%u.%u",
					maj, min, sub);
			bcd |= MIN(maj/10, 9) << 12;
			bcd |= MIN(maj%10, 9) << 8;
			bcd |= MIN(min, 9) << 4;
			bcd |= MIN(sub, 9);
			DBG("Setting version ID to %04x", bcd);
			main_opts.did_version = bcd;

			/* Need to push the version to adapters (for
			   EIR) as well as SDP server */

			adapter_foreach(set_did, NULL);
			update_device_id(main_opts.did_vendor,
						main_opts.did_product,
						main_opts.did_version,
						main_opts.did_source);

		} else {
			error("Cannot get OS version");
		}
	}

	return 0;
}

static void jolla_did_exit(void)
{
}

BLUETOOTH_PLUGIN_DEFINE(jolla_did, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
			jolla_did_init, jolla_did_exit)

