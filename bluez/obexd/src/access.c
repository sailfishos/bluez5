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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <glib.h>

#include "log.h"
#include "access.h"

static char *plugin_name = NULL;
static struct access_plugin *plugin_ptr = NULL;
static gpointer plugin_data = NULL;

int access_plugin_register(const char *name,
			struct access_plugin *plugin,
			gpointer user_data)
{
	if (!name || !plugin || !plugin->check || !plugin->check_at)
		return -EINVAL;

	if (plugin_name)
		return -EALREADY;

	plugin_name = g_strdup(name);
	plugin_ptr = plugin;
	plugin_data = user_data;

	return 0;
}

int access_plugin_unregister(const char *name)
{
	if (!name || !plugin_name || strcmp(name, plugin_name))
		return -ENOENT;

	g_free(plugin_name);
	plugin_name = NULL;
	plugin_ptr = NULL;
	plugin_data = NULL;

	return 0;
}

int access_check(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *object)
{
	if (plugin_ptr)
		return (plugin_ptr->check)(target, target_len, op, object,
					plugin_data);
	else
		return 0;
}

int access_check_at(const uint8_t *target,
			gsize target_len,
			enum access_op op,
			const gchar *parent,
			const gchar *object)
{
	if (plugin_ptr)
		return (plugin_ptr->check_at)(target, target_len, op, parent,
					object, plugin_data);
	else
		return 0;
}

