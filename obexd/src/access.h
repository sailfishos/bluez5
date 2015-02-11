/*
 *  Access control functionality
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

/*
 * Functions for checking access to filesystem objects and and
 * interface for access plugins to provide the implementation for
 * custom access checks.  Intended for cases where file system based
 * access controls are not enough (e.g., when it's not desirable to
 * export all of user's files over OBEX FTP)
 */

enum access_op {
	ACCESS_OP_LIST,
	ACCESS_OP_READ,
	ACCESS_OP_WRITE,
	ACCESS_OP_CREATE,
	ACCESS_OP_DELETE
};

struct access_plugin {
	int (*check)(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *object,
			gpointer user_data);
	int (*check_at)(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *parent,
			const gchar *object,
			gpointer user_data);
};

/*
 * It is possible to register only one plugin at a time; if a plugin
 * is already registered when a new registration is attempted, the
 * attempt will fail.
 *
 * If no plugin is registered, all operations are allowed.
 *
 */
int access_plugin_register(const char *name,
			struct access_plugin *plugin,
			gpointer user_data);

int access_plugin_unregister(const char *name);

int access_check(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *object);

int access_check_at(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *parent,
			const gchar *object);
