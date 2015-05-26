/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2015 Jolla Ltd. All rights reserved.
 *  Contact: Slava Monich <slava.monich@jolla.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdint.h>

#include "gdbus/gdbus.h"
#include "src/dbus-common.h"
#include "src/log.h"
#include "src/error.h"
#include "src/plugin.h"

#define LOG_INTERFACE  "org.bluez.DebugLog"
#define LOG_PATH       "/"

static DBusConnection *connection = NULL;

extern struct btd_debug_desc __start___debug[];
extern struct btd_debug_desc __stop___debug[];

static void logcontrol_update(const char* pattern, unsigned int set_flags,
						unsigned int clear_flags)
{
	struct btd_debug_desc *start = __start___debug;
	struct btd_debug_desc *stop = __stop___debug;
	struct btd_debug_desc *desc;

	if (!start || !stop)
		return;

	for (desc = start; desc < stop; desc++) {
		if (desc->file && g_pattern_match_simple(pattern, desc->file)) {
			desc->flags |= set_flags;
			desc->flags &= ~clear_flags;
		}
	}
}

static DBusMessage *logcontrol_dbusmsg(DBusConnection *conn, DBusMessage *msg,
			unsigned int set_flags, unsigned int clear_flags)
{
	const char *pattern;

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
							DBUS_TYPE_INVALID)) {
		logcontrol_update(pattern, set_flags, clear_flags);
		return g_dbus_create_reply(msg, DBUS_TYPE_INVALID);
	} else {
		return btd_error_invalid_args(msg);
	}
}

static DBusMessage *logcontrol_enable(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return logcontrol_dbusmsg(conn, msg, BTD_DEBUG_FLAG_PRINT, 0);
}

static DBusMessage *logcontrol_disable(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	return logcontrol_dbusmsg(conn, msg, 0, BTD_DEBUG_FLAG_PRINT);
}

static gint logcontrol_list_compare(gconstpointer a, gconstpointer b)
{
	return strcmp(a, b);
}

static void logcontrol_list_append(gpointer name, gpointer iter)
{
	dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &name);
}

static void logcontrol_list_store(GHashTable *hash, const char *name)
{
	if (name && !g_hash_table_contains(hash, (gpointer)name))
		g_hash_table_insert(hash, (gpointer)name, (gpointer)name);
}

static DBusMessage *logcontrol_list(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	DBusMessage *reply = dbus_message_new_method_return(msg);

	if (reply) {
		struct btd_debug_desc *start = __start___debug;
		struct btd_debug_desc *stop = __stop___debug;
		DBusMessageIter iter, array;

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_TYPE_STRING_AS_STRING, &array);

		if (start && stop) {
			struct btd_debug_desc *desc;
			GList *files;
			GHashTable *hash = g_hash_table_new_full(g_str_hash,
						g_str_equal, NULL, NULL);

			for (desc = start; desc < stop; desc++)
				logcontrol_list_store(hash, desc->file);

			files = g_list_sort(g_hash_table_get_keys(hash),
						logcontrol_list_compare);
			g_list_foreach(files, logcontrol_list_append, &array);
			g_list_free(files);
			g_hash_table_destroy(hash);
		}

		dbus_message_iter_close_container(&iter, &array);
	}

	return reply;
}

static const GDBusMethodTable methods[] = {
	{ GDBUS_METHOD("Enable", GDBUS_ARGS({ "pattern", "s" }), NULL,
							logcontrol_enable) },
	{ GDBUS_METHOD("Disable", GDBUS_ARGS({ "pattern", "s" }), NULL,
							logcontrol_disable) },
	{ GDBUS_METHOD("List", NULL, GDBUS_ARGS({ "names", "as" }),
							logcontrol_list) },
	{ },
};

static int logcontrol_init(void)
{
	DBG("");

	connection = btd_get_dbus_connection();
	if (!connection)
		return -1;

	dbus_connection_ref(connection);

	if (!g_dbus_register_interface(connection, LOG_PATH, LOG_INTERFACE,
					methods, NULL, NULL, NULL, NULL)) {
		error("logcontrol: failed to register " LOG_INTERFACE);
		dbus_connection_unref(connection);
		return -1;
	}

	return 0;
}

static void logcontrol_exit(void)
{
	DBG("");

	if (connection) {
		g_dbus_unregister_interface(connection, LOG_PATH,
								LOG_INTERFACE);
		dbus_connection_unref(connection);
		connection = NULL;
	}
}

BLUETOOTH_PLUGIN_DEFINE(jolla_logcontrol, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
			logcontrol_init, logcontrol_exit)
