/*
 *  Phonebook access through D-Bus vCard and call history service
 *
 *  Copyright (C) 2013, 2014 Jolla Ltd.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <glib.h>
#include <dbus/dbus.h>

#include "log.h"
#include "phonebook.h"
#include "obex.h"
#include "manager.h"

#define CALLDATA_SERVICE "com.jolla.ObexCallData"

#define CONTACTS_PATH "/contacts"
#define CONTACTS_INTERFACE "com.jolla.Contacts"
#define CONTACTS_METHOD_FETCH_COUNT "Count"
#define CONTACTS_METHOD_FETCH_ONE "FetchById"
#define CONTACTS_METHOD_FETCH_MANY "Fetch"

#define CALLHIST_PATH "/callhistory"
#define CALLHIST_INTERFACE "com.jolla.CallHistory"
#define CALLHIST_METHOD_FETCH_COUNT "Count"
#define CALLHIST_METHOD_FETCH_ONE "FetchById"
#define CALLHIST_METHOD_FETCH_MANY "Fetch"

#define CHUNK_LENGTH 128

#define VERSION_UNSET 0

#define PB_FORMAT_VCARD21 0
#define PB_FORMAT_VCARD30 1

#define FILTER_BIT_MAX 28

static const char *filter_name[FILTER_BIT_MAX + 1] = {
        "VERSION",
        "FN",
        "N",
        "PHOTO",
        "BDAY",
        "ADR",
        "LABEL",
        "TEL",
        "EMAIL",
        "MAILER",
        "TZ",
        "GEO",
        "TITLE",
        "ROLE",
        "LOGO",
        "AGENT",
        "ORG",
        "NOTE",
        "REV",
        "SOUND",
        "URL",
        "UID",
        "KEY",
        "NICKNAME",
        "CATEGORIES",
        "PROID",
        "CLASS",
        "SORT-STRING",
        "X-IRMC-CALL-DATETIME"
};

static DBusConnection *conn = NULL;

struct phonebook_data
{
	char *name;
	const struct apparam_field *params;
	phonebook_cb cb;
	phonebook_entry_cb entry_cb;
	phonebook_cache_ready_cb ready_cb;
	void *user_data;
	DBusPendingCall *pend;

	void (*process_begin)(struct phonebook_data *data);
	void (*process)(struct phonebook_data *data,
			const char *id,
			const char *name,
			const char *tel,
			const char *vcard);
	void (*process_end)(struct phonebook_data *data, gboolean last);

	guint32 pull_count;
	char *pull_buf;

	guint32 newmissedcalls;

	guint32 chunk_offset;
	guint32 chunk_length;
	guint32 chunk_end;
};

static void append_filter(DBusMessage *msg, uint8_t format, uint64_t filter)
{
	DBusMessageIter iter;
	DBusMessageIter array_iter;

	DBG("Appending filters");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_open_container(&iter,
					DBUS_TYPE_ARRAY,
					"s",
					&array_iter);
	if (filter != 0) {
		uint64_t i;

		filter |= 
			format == PB_FORMAT_VCARD30
			? 0x87 /* VERSION, N, FN, TEL */
			: 0x85 /* VERSION, N, TEL */;
		for (i = 0; i <= FILTER_BIT_MAX; i++) {
			if (filter & (1ULL << i)) {
				const char *f = filter_name[i];
				DBG("Appending filter '%s'", f);
				dbus_message_iter_append_basic(&array_iter,
							DBUS_TYPE_STRING,
							&f);
			}
		}
	}
	dbus_message_iter_close_container(&iter,
					&array_iter);

	DBG("Appending filters done.");
}

static const char *name_to_calltype(const char *name)
{
	return
		(g_strcmp0(name, PB_CALLS_INCOMING) == 0 ||
			g_strcmp0(name, PB_CALLS_INCOMING_FOLDER) == 0)
		? "inbound" :
		(g_strcmp0(name, PB_CALLS_OUTGOING) == 0 ||
			g_strcmp0(name, PB_CALLS_OUTGOING_FOLDER) == 0)
		? "outbound" :
		(g_strcmp0(name, PB_CALLS_MISSED) == 0 ||
			g_strcmp0(name, PB_CALLS_MISSED_FOLDER) == 0)
		? "missed" :
		(g_strcmp0(name, PB_CALLS_COMBINED) == 0 ||
			g_strcmp0(name, PB_CALLS_COMBINED_FOLDER) == 0)
		? "combined" :
		NULL;
}

static DBusMessage *next_chunk_request(struct phonebook_data *data,
					const char *fmt,
					const char *type)
{
	DBusMessage *msg = NULL;
	guint32 off32 = data->chunk_offset;
	guint32 len32 = MIN(data->chunk_end - data->chunk_offset,
			data->chunk_length);

	if (!type) {
		DBG("Fetching %u of %u contacts starting at "
			"position %u, formatting as '%s'",
			len32, data->params->maxlistcount, off32, fmt);
		msg = dbus_message_new_method_call(CALLDATA_SERVICE,
						CONTACTS_PATH,
						CONTACTS_INTERFACE,
						CONTACTS_METHOD_FETCH_MANY);
		dbus_message_append_args(msg,
					DBUS_TYPE_UINT32, &off32,
					DBUS_TYPE_UINT32, &len32,
					DBUS_TYPE_STRING, &fmt,
					DBUS_TYPE_INVALID);
		append_filter(msg,
			data->params->format,
			data->params->filter);
	} else {
		DBG("Fetching %u of %u %s calls starting at "
			"position %u, formatting as '%s'",
			len32, data->params->maxlistcount, type, off32, fmt);
		msg = dbus_message_new_method_call(CALLDATA_SERVICE,
						CALLHIST_PATH,
						CALLHIST_INTERFACE,
						CALLHIST_METHOD_FETCH_MANY);
		dbus_message_append_args(msg,
					DBUS_TYPE_STRING, &type,
					DBUS_TYPE_UINT32, &off32,
					DBUS_TYPE_UINT32, &len32,
					DBUS_TYPE_STRING, &fmt,
					DBUS_TYPE_INVALID);
		append_filter(msg,
			data->params->format,
			data->params->filter);
	}

	return msg;
}

static void count_cb(DBusPendingCall *pend,
		     void *user_data)
{
	struct phonebook_data *data = user_data;
	DBusMessage *reply = NULL;
	DBusMessageIter iter;
	guint32 count = 0, version;
	gboolean contacts_cb = FALSE;
	const gchar *signature = NULL;

	DBG("");

	if (!g_strcmp0(data->name, PB_CONTACTS) ||
		!g_strcmp0(data->name, PB_CONTACTS_FOLDER)) {
		contacts_cb = TRUE;
		signature = "uu";
	} else {
		contacts_cb = FALSE;
		signature = "uuu";
	}

	reply = dbus_pending_call_steal_reply(data->pend);
	if (reply == NULL)
		return;

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		DBG("D-Bus error");
		goto done;
	}

	if (g_strcmp0(dbus_message_get_signature(reply), signature) != 0) {
		DBG("Unexpected D-Bus signature");
		goto done;
	}

	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_get_basic(&iter, &version);
	dbus_message_iter_next(&iter);
	if (!contacts_cb) {
		if (!g_strcmp0(data->name, PB_CALLS_MISSED) ||
			!g_strcmp0(data->name, PB_CALLS_MISSED_FOLDER))
			dbus_message_iter_get_basic(&iter,
						&data->newmissedcalls);
		dbus_message_iter_next(&iter);
	}
	dbus_message_iter_get_basic(&iter, &count);
	DBG("count: %d", count);

done:
	dbus_message_unref(reply);
	dbus_pending_call_unref(data->pend);
	data->pend = NULL;

        data->cb(NULL, 0, count,
		data->newmissedcalls > 0xff ? 0xff : data->newmissedcalls,
		TRUE, data->user_data);
}

static void pull_begin(struct phonebook_data *data)
{
	g_free(data->pull_buf);
	data->pull_buf = NULL;
	data->pull_count = 0;
}

static void pull_process(struct phonebook_data *data,
			const char *id,
			const char *name,
			const char *tel,
			const char *vcard)
{
	data->pull_count++;
	if (data->pull_buf == NULL)
		data->pull_buf = g_strdup(vcard);
	else {
		char *oldbuf = data->pull_buf;
		data->pull_buf = g_strconcat(oldbuf, vcard, NULL);
		g_free(oldbuf);
	}
}

static void pull_end(struct phonebook_data *data, gboolean last)
{
	char *buf = data->pull_buf;
	guint32 count = data->pull_count;
	data->pull_buf = NULL;
	data->pull_count = 0;

	DBG("Forwarding %d bytes, %d items (%d new missed calls).",
		buf ? strlen(buf) : 0, count, data->newmissedcalls);
	data->cb(buf, buf ? strlen(buf) : 0, count,
		data->newmissedcalls > 0xff ? 0xff : data->newmissedcalls,
		last, data->user_data);

	g_free(buf);
}

static void cache_begin(struct phonebook_data *data)
{
}

static void cache_process(struct phonebook_data *data,
			const char *id,
			const char *name,
			const char *tel,
			const char *vcard)
{
	uint32_t handle;

	DBG("Forwarning entry '%s', name '%s', tel '%s'", id, name, tel);
	handle = (strcmp(id, "owner-contact") == 0)
		? 0
		: PHONEBOOK_INVALID_HANDLE;
	(data->entry_cb)(id, handle, name, "", tel, data->user_data);
}

static void cache_end(struct phonebook_data *data, gboolean last)
{
	(data->ready_cb)(data->user_data,
			data->newmissedcalls > 0xff
			? 0xff
			: data->newmissedcalls);
}

static void fetch_one_cb(DBusPendingCall *pend,
			void *user_data)
{
	struct phonebook_data *data = user_data;
	DBusMessage *reply = NULL;
	DBusMessageIter iter, struct_iter;
	char *id;
	char *name;
	char *tel;
	char *vcard;
	guint32 version;
	gboolean contacts_cb = FALSE;
	const gchar *signature = NULL;

	DBG("");

	if (!g_strcmp0(data->name, PB_CONTACTS) ||
		!g_strcmp0(data->name, PB_CONTACTS_FOLDER)) {
		contacts_cb = TRUE;
		signature = "u(ssss)";
	} else {
		contacts_cb = FALSE;
		signature = "uu(ssss)";
	}

	reply = dbus_pending_call_steal_reply(data->pend);
	if (reply == NULL)
		return;

	(data->process_begin)(data);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		DBG("D-Bus error");
		goto done;
	}

	if (g_strcmp0(dbus_message_get_signature(reply), signature) != 0) {
		DBG("Unexpected D-Bus signature");
		goto done;
	}

	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_get_basic(&iter, &version);
	dbus_message_iter_next(&iter);
	if (!contacts_cb) {
		if (!g_strcmp0(data->name, PB_CALLS_MISSED) ||
			!g_strcmp0(data->name, PB_CALLS_MISSED_FOLDER))
			dbus_message_iter_get_basic(&iter,
						&data->newmissedcalls);
		dbus_message_iter_next(&iter);
	}
	dbus_message_iter_recurse(&iter, &struct_iter);
	dbus_message_iter_get_basic(&struct_iter, &id);
	dbus_message_iter_next(&struct_iter);
	dbus_message_iter_get_basic(&struct_iter, &name);
	dbus_message_iter_next(&struct_iter);
	dbus_message_iter_get_basic(&struct_iter, &tel);
	dbus_message_iter_next(&struct_iter);
	dbus_message_iter_get_basic(&struct_iter, &vcard);

	DBG("id: %s, name: %s, tel: %s, vcard: %s", id, name, tel, vcard);
	(data->process)(data, id, name, tel, vcard);

done:
	DBG("Finalizing. ");

	dbus_message_unref(reply);
	dbus_pending_call_unref(data->pend);
	data->pend = NULL;

	(data->process_end)(data, TRUE);
}

static void fetch_many_cb(DBusPendingCall *pend,
			void *user_data)
{
	struct phonebook_data *data = user_data;
	DBusMessage *reply = NULL;
	DBusMessageIter iter, array_iter, struct_iter;
	guint32 version;
	gboolean contacts_cb = FALSE;
	const gchar *signature = NULL;
	guint32 results = 0;

	DBG("");

	if (!g_strcmp0(data->name, PB_CONTACTS) ||
		!g_strcmp0(data->name, PB_CONTACTS_FOLDER)) {
		contacts_cb = TRUE;
		signature = "ua(ssss)";
	} else {
		contacts_cb = FALSE;
		signature = "uua(ssss)";
	}

	reply = dbus_pending_call_steal_reply(data->pend);
	if (reply == NULL)
		return;

	/* First chunk of responses? */
	if (data->chunk_offset == data->params->liststartoffset)
		(data->process_begin)(data);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		DBG("D-Bus error");
		goto done;
	}

	if (g_strcmp0(dbus_message_get_signature(reply), signature) != 0) {
		DBG("Unexpected D-Bus signature");
		goto done;
	}

	dbus_message_iter_init(reply, &iter);
	dbus_message_iter_get_basic(&iter, &version);
	dbus_message_iter_next(&iter);
	if (!contacts_cb) {
		if (!g_strcmp0(data->name, PB_CALLS_MISSED) ||
			!g_strcmp0(data->name, PB_CALLS_MISSED_FOLDER))
			dbus_message_iter_get_basic(&iter,
						&data->newmissedcalls);
		dbus_message_iter_next(&iter);
	}
	dbus_message_iter_recurse(&iter, &array_iter);
	while (dbus_message_iter_get_arg_type(&array_iter) !=
		DBUS_TYPE_INVALID) {
		char *id;
		char *name;
		char *tel;
		char *vcard;
		dbus_message_iter_recurse(&array_iter, &struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &id);
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &name);
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &tel);
		dbus_message_iter_next(&struct_iter);
		dbus_message_iter_get_basic(&struct_iter, &vcard);

		DBG("id: %s, name: %s, tel:%s, vcard: %s",
			id, name, tel, vcard);
		(data->process)(data, id, name, tel, vcard);
		results++;

		dbus_message_iter_next(&array_iter);
	}

done:
	DBG("%u results received, now at offset %u (ending at %u)",
		results, data->chunk_offset, data->chunk_end);

	dbus_message_unref(reply);
	dbus_pending_call_unref(data->pend);
	data->pend = NULL;

	data->chunk_offset += results;

	if (results == data->chunk_length &&
			data->chunk_offset < data->chunk_end) {
		/* Full chunk read but not at the end -> read more later */
		(data->process_end)(data, FALSE);

	} else {
		/* Everything read or error */
		(data->process_end)(data, TRUE);

	}
}

int phonebook_init(void)
{
	DBG("");

	conn = manager_dbus_get_connection();
	if (conn == NULL)
		return -1;

	return 0;
}

void phonebook_exit(void)
{
	DBG("");

	dbus_connection_unref(conn);
}

char *phonebook_set_folder(const char *current_folder,
		const char *new_folder, uint8_t flags, int *err)
{
	gboolean root, child;
	char *tmp1, *tmp2, *base, *path = NULL;
	int len, ret = 0;

	DBG("current:'%s', new:'%s', flags:%x",
		current_folder, new_folder, flags);

	root = (g_strcmp0("/", current_folder) == 0);
	child = (new_folder && strlen(new_folder) != 0);

	switch (flags) {
	case 0x02:
		/* Go back to root */
		if (!child) {
			path = g_strdup("/");
			goto done;
		}

		path = g_build_filename(current_folder, new_folder, NULL);
		break;
	case 0x03:
		/* Go up 1 level */
		if (root) {
			/* Already root */
			ret = -EBADR;
			goto done;
		}

		/*
		 * Removing one level of the current folder. Current folder
		 * contains AT LEAST one level since it is not at root folder.
		 * Use glib utility functions to handle invalid chars in the
		 * folder path properly.
		 */
		tmp1 = g_path_get_basename(current_folder);
		tmp2 = g_strrstr(current_folder, tmp1);
		len = tmp2 - (current_folder + 1);

		g_free(tmp1);

		if (len == 0)
			base = g_strdup("/");
		else
			base = g_strndup(current_folder, len);

		/* Return: one level only */
		if (!child) {
			path = base;
			goto done;
		}

		path = g_build_filename(base, new_folder, NULL);
		g_free(base);

		break;
	default:
		ret = -EBADR;
		break;
	}

done:
	if (g_strcmp0(path, "/") &&
		g_strcmp0(path, PB_TELECOM_FOLDER) &&
		g_strcmp0(path, PB_CONTACTS_FOLDER) &&
		g_strcmp0(path, PB_CALLS_COMBINED_FOLDER) &&
		g_strcmp0(path, PB_CALLS_INCOMING_FOLDER) &&
		g_strcmp0(path, PB_CALLS_MISSED_FOLDER) &&
		g_strcmp0(path, PB_CALLS_OUTGOING_FOLDER)) {

		g_free(path);
		path = NULL;
		ret = -EBADR;
	}

	if (!path) {
		if (err)
			*err = ret;

		return NULL;
	}

	if (err)
		*err = ret;

	return path;
}

void *phonebook_pull(const char *name,
		const struct apparam_field *params, phonebook_cb cb,
		void *user_data, int *err)
{
	struct phonebook_data *data;

	DBG("name %s", name);

	data = g_new0(struct phonebook_data, 1);
	data->name = g_strdup(name);
	data->params = params;
	data->user_data = user_data;
	data->cb = cb;

	data->process_begin = pull_begin;
	data->process = pull_process;
	data->process_end = pull_end;

	data->chunk_offset = data->params->liststartoffset;
	data->chunk_length = CHUNK_LENGTH;
	data->chunk_end = data->params->liststartoffset +
				data->params->maxlistcount;

	if (err)
		*err = 0;

	return data;
}

int phonebook_pull_read(void *request)
{
	struct phonebook_data *data = request;
	DBusMessage *msg = NULL;
	void (*cb)(DBusPendingCall *, void *) = NULL;
	const char *fmt = NULL;

	DBG("");

	if (!data)
		return -ENOENT;

	fmt = data->params->format == PB_FORMAT_VCARD30
		? "vcard30"
		: "vcard21";

	if (g_strcmp0(data->name, PB_CONTACTS) == 0) {

		if (data->params->maxlistcount == 0) {
			DBG("Fetching contact count");
			msg = dbus_message_new_method_call
				(CALLDATA_SERVICE,
				CONTACTS_PATH,
				CONTACTS_INTERFACE,
				CONTACTS_METHOD_FETCH_COUNT);
			cb = count_cb;
		} else {
			msg = next_chunk_request(data, fmt, NULL);
			cb = fetch_many_cb;
		}

	} else if (g_strcmp0(data->name, PB_CALLS_INCOMING) == 0 ||
		g_strcmp0(data->name, PB_CALLS_OUTGOING) == 0 ||
		g_strcmp0(data->name, PB_CALLS_MISSED) == 0 ||
		g_strcmp0(data->name, PB_CALLS_COMBINED) == 0) {

		const char *type = name_to_calltype(data->name);

		if (data->params->maxlistcount == 0) {
			DBG("Fetching call count");
			msg = dbus_message_new_method_call
				(CALLDATA_SERVICE,
				CALLHIST_PATH,
				CALLHIST_INTERFACE,
				CALLHIST_METHOD_FETCH_COUNT);
			dbus_message_append_args(msg,
						DBUS_TYPE_STRING, &type,
						DBUS_TYPE_INVALID);
			cb = count_cb;
		} else {
			msg = next_chunk_request(data, fmt, type);
			cb = fetch_many_cb;
		}

	} else {
		return -ENOENT;
	}

	dbus_connection_send_with_reply(conn,
					msg,
					&data->pend,
					DBUS_TIMEOUT_USE_DEFAULT);
	dbus_pending_call_set_notify(data->pend,
				cb,
				data,
				NULL);
	dbus_message_unref(msg);

	return 0;
}

void *phonebook_get_entry(const char *folder, const char *id,
				const struct apparam_field *params,
				phonebook_cb cb, void *user_data, int *err)
{
	struct phonebook_data *data;
	DBusMessage *msg;
	const char *fmt = NULL;

	DBG("folder:%s, id:%s", folder, id);

	if (g_strcmp0(folder, PB_CONTACTS_FOLDER) &&
		g_strcmp0(folder, PB_CALLS_INCOMING_FOLDER) &&
		g_strcmp0(folder, PB_CALLS_OUTGOING_FOLDER) &&
		g_strcmp0(folder, PB_CALLS_MISSED_FOLDER) &&
		g_strcmp0(folder, PB_CALLS_COMBINED_FOLDER)) {
		if (err)
			*err = -ENOENT;
		return NULL;
	}

	data = g_new0(struct phonebook_data, 1);
	data->name = g_strdup(folder);
	data->params = params;
	data->user_data = user_data;
	data->cb = cb;

	data->process_begin = pull_begin;
	data->process = pull_process;
	data->process_end = pull_end;

	fmt = data->params->format == PB_FORMAT_VCARD30
		? "vcard30"
		: "vcard21";

	if (g_strcmp0(folder, PB_CONTACTS_FOLDER) == 0) {
		DBG("Fetching contact entry");
		msg = dbus_message_new_method_call
			(CALLDATA_SERVICE,
				CONTACTS_PATH,
				CONTACTS_INTERFACE,
				CONTACTS_METHOD_FETCH_ONE);
		dbus_message_append_args(msg,
					DBUS_TYPE_STRING, &id,
					DBUS_TYPE_STRING, &fmt,
					DBUS_TYPE_INVALID);
		append_filter(msg, data->params->format, data->params->filter);
	} else {
		const char *type = name_to_calltype(folder);
		DBG("Fetching call history entry");
		msg = dbus_message_new_method_call
			(CALLDATA_SERVICE,
				CALLHIST_PATH,
				CALLHIST_INTERFACE,
				CALLHIST_METHOD_FETCH_ONE);
		dbus_message_append_args(msg,
					DBUS_TYPE_STRING, &type,
					DBUS_TYPE_STRING, &id,
					DBUS_TYPE_STRING, &fmt,
					DBUS_TYPE_INVALID);
		append_filter(msg, data->params->format, data->params->filter);
	}

	dbus_connection_send_with_reply(conn,
					msg,
					&data->pend,
					DBUS_TIMEOUT_USE_DEFAULT);
	dbus_pending_call_set_notify(data->pend,
				fetch_one_cb, 
				data,
				NULL);
	dbus_message_unref(msg);

	if (err)
		*err = 0;

	return data;
}

void *phonebook_create_cache(const char *name,
			phonebook_entry_cb entry_cb,
			phonebook_cache_ready_cb ready_cb, void *user_data,
			int *err)
{
	static const struct apparam_field dummy_cache_params = {
		.liststartoffset = 0,
		.maxlistcount = ~0,
		.format = PB_FORMAT_VCARD21,
		.filter = 0
	};
	struct phonebook_data *data;
	DBusMessage *msg;
	const char *fmt = "vcard21";

	DBG("name %s", name);

	if (g_strcmp0(name, PB_CONTACTS_FOLDER) &&
		g_strcmp0(name, PB_CALLS_INCOMING_FOLDER) &&
		g_strcmp0(name, PB_CALLS_OUTGOING_FOLDER) &&
		g_strcmp0(name, PB_CALLS_MISSED_FOLDER) &&
		g_strcmp0(name, PB_CALLS_COMBINED_FOLDER)) {
		if (err)
			*err = -ENOENT;
		return NULL;
	}

	data = g_new0(struct phonebook_data, 1);
	data->name = g_strdup(name);
	data->user_data = user_data;
	data->entry_cb = entry_cb;
	data->ready_cb = ready_cb;

	data->process_begin = cache_begin;
	data->process = cache_process;
	data->process_end = cache_end;

	data->params = &dummy_cache_params;

	data->chunk_offset = 0;
	data->chunk_length = ~0;
	data->chunk_end = ~0;

	if (g_strcmp0(name, PB_CONTACTS_FOLDER) == 0) {
		DBG("Caching contacts");
		msg = next_chunk_request(data, fmt, NULL);

	} else {
		const char *type = name_to_calltype(name);
		DBG("Caching call history");
		msg = next_chunk_request(data, fmt, type);
	}

	dbus_connection_send_with_reply(conn,
					msg,
					&data->pend,
					DBUS_TIMEOUT_USE_DEFAULT);
	dbus_pending_call_set_notify(data->pend,
				fetch_many_cb,
				data,
				NULL);
	dbus_message_unref(msg);

	if (err)
		*err = 0;

	return data;
}

void phonebook_req_finalize(void *request)
{
	struct phonebook_data *data = request;

	DBG("");

	if (!data)
		return;

	if (data->pend != NULL) {
		dbus_pending_call_cancel(data->pend);
		dbus_pending_call_unref(data->pend);
	}

	g_free(data->pull_buf);
	g_free(data->name);
	g_free(data);
}
