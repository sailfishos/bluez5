/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014 Jolla Ltd. All rights reserved.
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

/* Plugin for checking D-Bus method access for restricted methods.
   Caller must belong to the privileged group for restricted method
   calls to succeed. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <grp.h>

#include "gdbus/gdbus.h"

#include "src/plugin.h"
#include "src/log.h"
#include "src/dbus-common.h"

struct access_check {
	char *busname;
	DBusConnection *connection;
	gboolean pending_unanswered;
	GDBusPendingReply pending;
};

static void jolla_dbus_access_check(DBusConnection *connection,
					DBusMessage *message,
					const char *action,
					gboolean interaction,
					GDBusPendingReply pending);

static const GDBusSecurityTable security[] = {
	{ BLUEZ_PRIVILEGED_ACCESS, "org.bluez.privileged", 0,
	  jolla_dbus_access_check },
	{ }
};

static GHashTable *gid_hash = NULL;
static GSList *authorized_gids = NULL;
static GHashTable *watch_hash = NULL;

static void access_check_free(void *pointer)
{
	struct access_check *check = pointer;
	DBG("free %p", check);
	if (check->pending_unanswered) {
		DBG("rejecting access for pending %u", check->pending);
		g_dbus_pending_error(check->connection, check->pending,
					DBUS_ERROR_AUTH_FAILED, NULL);
	}
	g_free(check->busname);
	dbus_connection_unref(check->connection);
	g_free(check);
}

static gboolean gid_is_authorized(gid_t gid)
{
	if (g_slist_find(authorized_gids, GINT_TO_POINTER(gid))) {
		DBG("gid %d allowed.", gid);
		return TRUE;
	}

	DBG("gid %d denied.", gid);
	return FALSE;
}

static void busname_exit_callback(DBusConnection *conn, void *user_data)
{
	char *busname = user_data;
	DBG("D-Bus name '%s' gone.", busname);
	g_hash_table_remove(gid_hash, busname);
	g_hash_table_remove(watch_hash, busname);
}

static void pid_query_result(DBusPendingCall *pend,
				void *user_data)
{
	char path[32];
	struct access_check *check = user_data;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	dbus_uint32_t pid;
	struct stat st;
	guint name_watch;

	DBG("query for busname %s", check->busname);

	reply = dbus_pending_call_steal_reply(pend);
	if (!reply)
		goto done;

	if (!gid_hash)
		goto done;

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR)
		goto done;

	if (g_strcmp0(dbus_message_get_signature(reply), "u"))
		goto done;

	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_get_basic(&iter, &pid);

	snprintf(path, sizeof(path), "/proc/%u", pid);
	if (stat(path, &st) < 0)
		goto done;

	DBG("query done, pid %d has gid %d", pid, st.st_gid);

	name_watch = g_dbus_add_disconnect_watch(check->connection, 
						check->busname,
						busname_exit_callback,
						g_strdup(check->busname),
						g_free);

	g_hash_table_replace(watch_hash, g_strdup(check->busname),
				GUINT_TO_POINTER(name_watch));
	g_hash_table_replace(gid_hash, g_strdup(check->busname),
				GINT_TO_POINTER(st.st_gid));

	if (gid_is_authorized(st.st_gid)) {
		DBG("allowing access for pid %d, pending %u",
			pid, check->pending);
		g_dbus_pending_success(check->connection, check->pending);
		check->pending_unanswered = FALSE;
	}

done:
	if (reply)
		dbus_message_unref(reply);
	dbus_pending_call_unref(pend);
}

static void jolla_dbus_access_check(DBusConnection *connection,
					DBusMessage *message,
					const char *action,
					gboolean interaction,
					GDBusPendingReply pending)
{
	struct access_check *check = NULL;
	DBusMessage *query = NULL;
	DBusPendingCall *pend = NULL;
	const char *busname = dbus_message_get_sender(message);
	gpointer p;

	if (!busname)
		goto fail;

	if (!authorized_gids) {
		DBG("No authorization configuration, allowing busname '%s'",
			busname);
		g_dbus_pending_success(connection, pending);
		return;
	}

	if (g_hash_table_lookup_extended(gid_hash, busname, NULL, &p)) {
		gid_t gid = GPOINTER_TO_INT(p);
		DBG("known busname '%s' has gid %d", busname, gid);
		if (gid_is_authorized(gid)) {
			DBG("allowing access for known busname '%s', "
				"pending %u", busname, pending);
			g_dbus_pending_success(connection, pending);
			return;
		}

		goto fail;
	}

	query = dbus_message_new_method_call("org.freedesktop.DBus",
						"/",
						"org.freedesktop.DBus",
						"GetConnectionUnixProcessID");
	if (!query)
		goto fail;

	if (!dbus_message_append_args(query, DBUS_TYPE_STRING, &busname,
					DBUS_TYPE_INVALID))
		goto fail;

	if (!dbus_connection_send_with_reply(connection, query,	&pend, -1))
		goto fail;

	check = g_new0(struct access_check, 1);
	check->busname = g_strdup(busname);
	check->connection = dbus_connection_ref(connection);
	check->pending = pending;
	check->pending_unanswered = TRUE;

	if (!dbus_pending_call_set_notify(pend, pid_query_result,
						check, access_check_free))
		goto fail;

	DBG("pid query sent for pending %u", pending);

	return;

fail:
	if (check)
		access_check_free(check);

	if (pend) {
		dbus_pending_call_cancel(pend);
		dbus_pending_call_unref(pend);
	}

	if (query)
		dbus_message_unref(query);

	DBG("rejecting access for pending %u", pending);
	g_dbus_pending_error(connection, pending, DBUS_ERROR_AUTH_FAILED, NULL);
}

static void unregister_watch(gpointer value)
{
	DBusConnection *connection = btd_get_dbus_connection();
	guint name_watch = GPOINTER_TO_UINT(value);
	g_dbus_remove_watch(connection, name_watch);
}

static void jolla_dbus_access_exit(void)
{
	g_dbus_unregister_security(security);
	g_hash_table_unref(watch_hash);
	watch_hash = NULL;
	g_hash_table_unref(gid_hash);
	gid_hash = NULL;
	g_slist_free(authorized_gids);
	authorized_gids = NULL;
}

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

static int jolla_dbus_access_init(void)
{
	GKeyFile *config;

	config = load_config(CONFIGDIR "/jolla.conf");
	if (config) {
		int i;
		char **groups = g_key_file_get_string_list(config,
							"Security",
							"DBusAuthorizedGroups",
							NULL, NULL);

		for (i = 0; groups && groups[i]; i++) {
			struct group *group = getgrnam(groups[i]);
			if (group)
				authorized_gids = g_slist_prepend(authorized_gids,
						GINT_TO_POINTER(group->gr_gid));
		}

		if (groups)
			g_strfreev(groups);
		g_key_file_free(config);
	}

	if (!authorized_gids)
		info("No valid configuration for D-Bus authorized groups, "
			"allowing all");

	watch_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
						unregister_watch);
	gid_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	g_dbus_register_security(security);
	return 0;

	jolla_dbus_access_exit();
	return -1;
}

BLUETOOTH_PLUGIN_DEFINE(jolla_dbus_access, VERSION,
			BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
			jolla_dbus_access_init, jolla_dbus_access_exit)
