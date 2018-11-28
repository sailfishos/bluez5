/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2018 Jolla Ltd. All rights reserved.
 *  Contact: Juho Hämäläinen <juho.hamalainen@jolla.com>
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
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

#include "lib/bluetooth.h"
#include "lib/sdp.h"
#include "src/plugin.h"
#include "src/adapter.h"
#include "src/service.h"
#include "src/profile.h"
#include "src/device.h"
#include "src/log.h"

/* if profile with remote uuid "uuid" is available at the same time as
 * service with remote uuid "exclude", the exclude service will be
 * removed. */
struct mutually_exclusive {
	char *uuid;
	char *exclude;
};

struct device_exclude {
	struct btd_device *device;
	struct btd_service *priority;
	struct btd_service *exclude;
};

static guint filter_id;
static unsigned int service_id;
GSList *device_excludes;
GSList *exclusives;

static GKeyFile *load_config(const char *file)
{
	GError *err = NULL;
	GKeyFile *keyfile;

	keyfile = g_key_file_new();
	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		DBG("Parsing %s failed: %s", file, err->message);
		g_error_free(err);
		g_key_file_free(keyfile);
		return NULL;
	}

	return keyfile;
}

static guint parse_exclusives(void)
{
	GKeyFile *config;
	guint count = 0;

	config = load_config(CONFIGDIR "/exclude.conf");
	if (config) {
		struct mutually_exclusive *ex;
		gchar **keys;
		gchar *key;
		gchar *value;
		guint i;

		keys = g_key_file_get_keys(config, "Exclude", NULL, NULL);

		for (i = 0; keys && keys[i]; i++) {
			key = keys[i];
			value = g_key_file_get_string(config, "Exclude", key, NULL);
			DBG("with %s exclude %s", key, value);

			ex = g_new0(struct mutually_exclusive, 1);
			ex->uuid = key;
			ex->exclude = value;
			exclusives = g_slist_append(exclusives, ex);
			count++;
		}

		/* free only array, strings from array are handed to new structs */
		g_free(keys);
		g_key_file_free(config);
	}

	return count;
}

static void exclusive_free(gpointer data)
{
	struct mutually_exclusive *ex = data;
	g_free(ex->uuid);
	g_free(ex->exclude);
	g_free(ex);
}

static struct device_exclude *device_exclude_get(struct btd_device *device,
                                                 struct btd_service *service)
{
	struct btd_profile *profile;
	struct device_exclude *devex = NULL;
	struct mutually_exclusive *ex;
	bool found = false;
	GSList *i;

	g_assert(service);
	profile = btd_service_get_profile(service);

	for (i = device_excludes; i; i = i->next) {
		devex = i->data;
		if (devex->device == device) {
			found = true;
			break;
		}
	}

	if (!found) {
		devex = g_new0(struct device_exclude, 1);
		devex->device = device;
		device_excludes = g_slist_append(device_excludes, devex);
	}

	for (i = exclusives; i; i = i->next) {
		ex = i->data;

		if (!g_strcmp0(ex->uuid, profile->remote_uuid)) {
			devex->priority = service;
			DBG("device %s has priority service with profile %s",
			    device_get_path(devex->device), profile->remote_uuid);
			break;
		} else if (!g_strcmp0(ex->exclude, profile->remote_uuid)) {
			devex->exclude = service;
			DBG("device %s has excludable service with profile %s",
			    device_get_path(devex->device), profile->remote_uuid);
			break;
		}
	}

	return devex;
}

static void device_exclude_remove(struct device_exclude *devex)
{
	if (!devex)
		return;

	device_excludes = g_slist_remove(device_excludes, devex);
	g_free(devex);
}

static void service_cb(struct btd_service *service,
						btd_service_state_t old_state,
						btd_service_state_t new_state,
						void *user_data)
{
	struct btd_device *device;
	struct device_exclude *devex;

	if (old_state == BTD_SERVICE_STATE_UNAVAILABLE && new_state == BTD_SERVICE_STATE_DISCONNECTED) {
		DBG("service %p UNAVAILABLE to DISCONNECTED", service);
		device = btd_service_get_device(service);
		devex = device_exclude_get(device, service);
		if (devex->priority && devex->exclude) {
			struct btd_service *remove = devex->exclude;
			devex->exclude = NULL;
			DBG("device %s with exclude active, removing service for profile %s",
			    device_get_path(device), btd_service_get_profile(remove)->name);
			device_remove_profile(devex->device, btd_service_get_profile(remove));
		}
	}

	if (old_state != BTD_SERVICE_STATE_UNAVAILABLE && new_state == BTD_SERVICE_STATE_UNAVAILABLE) {
		DBG("service %p ANY to UNAVAILABLE", service);
		device = btd_service_get_device(service);
		devex = device_exclude_get(device, service);
		if (devex->priority == service)
			devex->priority = NULL;
		else if (devex->exclude == service)
			devex->exclude = NULL;

		if (!devex->priority && !devex->exclude) {
			DBG("remove tracked device %s", device_get_path(device));
			device_exclude_remove(devex);
		}
	}
}

void filter_cb(struct btd_device *device, GSList **services,
               void *user_data)
{
	struct btd_service *service;
	struct device_exclude *devex;
	GSList *new_list = g_slist_copy(*services);
	GSList *i;

	DBG("");

	for (i = *services; i; i = i->next) {
		service = i->data;
		devex = device_exclude_get(device, service);

		if (devex->priority && devex->exclude) {
			struct btd_service *remove = devex->exclude;
			devex->exclude = NULL;
			DBG("device %s: removing service for profile %s",
			    device_get_path(device), btd_service_get_profile(remove)->name);
			if (btd_service_get_state(remove) != BTD_SERVICE_STATE_UNAVAILABLE)
				device_remove_profile(devex->device, btd_service_get_profile(remove));
			else
				btd_service_unref(remove);
			new_list = g_slist_remove(new_list, remove);
		}
	}

	g_slist_free(*services);
	*services = new_list;
}

static int sailfish_exclude_init(void)
{
	DBG("");

	if (parse_exclusives()) {
		filter_id = device_add_service_probe_filter(filter_cb, NULL);
		service_id = btd_service_add_state_cb(service_cb, NULL);
	}

	return 0;
}

static void sailfish_exclude_exit(void)
{
	if (filter_id) {
		device_remove_service_probe_filter(filter_id);
		filter_id = 0;
	}

	if (service_id) {
		btd_service_remove_state_cb(service_id);
		service_id = 0;
	}

	g_slist_free_full(exclusives, exclusive_free);
	exclusives = NULL;
	g_slist_free_full(device_excludes, g_free);
	device_excludes = NULL;
}

BLUETOOTH_PLUGIN_DEFINE(sailfish_exclude, VERSION,
                        BLUETOOTH_PLUGIN_PRIORITY_DEFAULT,
                        sailfish_exclude_init, sailfish_exclude_exit)
