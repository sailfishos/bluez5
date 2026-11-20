// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdint.h>
#include <string.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/uuid.h"
#include "gobex/gobex.h"
#include "obexd/client/transport.h"
#include "obexd/src/map_ap.h"
#include "obexd/src/log.h"
#include "messages.h"
#include "map-mns.h"

static const uint8_t mns_target[] = {
	0xbb, 0x58, 0x2b, 0x41, 0x42, 0x0c, 0x11, 0xdb,
	0xb0, 0xde, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
};

struct event_report {
	uint8_t instance;
	char *xml;
};

static void report_free(struct event_report *report)
{
	g_free(report->xml);
	g_free(report);
}

struct map_mns {
	struct obc_transport *transport;
	char *source;
	char *destination;
	GQueue events;
	guint transport_id;
	guint connect_timeout;
	GObex *obex;
	guint request;
	guint transfer;
	size_t offset;
	gboolean connected;
};

static void send_next(struct map_mns *mns);

static void mns_close(struct map_mns *mns)
{
	if (mns->connect_timeout) {
		g_source_remove(mns->connect_timeout);
		mns->connect_timeout = 0;
	}

	if (mns->transfer)
		g_obex_cancel_transfer(mns->transfer, NULL, NULL);

	if (mns->request)
		g_obex_cancel_req(mns->obex, mns->request, TRUE);

	g_clear_pointer(&mns->obex, g_obex_unref);

	if (mns->transport_id)
		mns->transport->disconnect(mns->transport_id);

	mns->transport_id = 0;
	mns->request = 0;
	mns->transfer = 0;
	mns->connected = FALSE;
	while (!g_queue_is_empty(&mns->events))
		report_free(g_queue_pop_head(&mns->events));
}

static void mns_failed(struct map_mns *mns)
{
	error("MAP notification connection closed");
	mns_close(mns);
}

static gboolean connect_expired(gpointer data)
{
	struct map_mns *mns = data;

	mns->connect_timeout = 0;
	mns_failed(mns);

	return G_SOURCE_REMOVE;
}

static void report_complete(GObex *obex, GError *err, gpointer data)
{
	struct map_mns *mns = data;

	mns->transfer = 0;
	report_free(g_queue_pop_head(&mns->events));

	if (err)
		mns_failed(mns);
	else
		send_next(mns);
}

static gssize report_read(void *buf, gsize len, gpointer data)
{
	struct map_mns *mns = data;
	struct event_report *report = g_queue_peek_head(&mns->events);
	const char *xml = report->xml;

	len = MIN(len, strlen(xml + mns->offset));
	memcpy(buf, xml + mns->offset, len);
	mns->offset += len;

	return len;
}

static void send_next(struct map_mns *mns)
{
	static const char type[] = "x-bt/MAP-event-report";
	uint8_t params[] = { MAP_AP_MASINSTANCEID, 1, 0 };
	struct event_report *report;

	if (!mns->connected || mns->transfer || g_queue_is_empty(&mns->events))
		return;

	report = g_queue_peek_head(&mns->events);
	params[2] = report->instance;
	mns->offset = 0;
	mns->transfer = g_obex_put_req(mns->obex, report_read, report_complete,
				mns, NULL,
				G_OBEX_HDR_TYPE, type, sizeof(type),
				G_OBEX_HDR_APPARAM, params, sizeof(params),
				G_OBEX_HDR_INVALID);

	if (!mns->transfer)
		mns_failed(mns);
}

static void mns_connected(GObex *obex, GError *err, GObexPacket *rsp,
							gpointer data)
{
	struct map_mns *mns = data;

	mns->request = 0;

	if (err || g_obex_packet_get_operation(rsp, NULL) !=
							G_OBEX_RSP_SUCCESS) {
		mns_failed(mns);
		return;
	}

	g_source_remove(mns->connect_timeout);
	mns->connect_timeout = 0;
	mns->connected = TRUE;
	send_next(mns);
}

static void mns_disconnected(GObex *obex, GError *err, gpointer data)
{
	mns_failed(data);
}

static void transport_connected(GIOChannel *io, GError *err, gpointer data)
{
	struct map_mns *mns = data;
	int rx_mtu = -1, tx_mtu = -1;
	GObexTransportType type = G_OBEX_TRANSPORT_STREAM;

	if (err || !io) {
		/* The transport cleans itself up when the connection fails. */
		mns->transport_id = 0;
		mns_failed(mns);
		return;
	}

	if (!mns->transport->getpacketopt(io, &tx_mtu, &rx_mtu))
		type = G_OBEX_TRANSPORT_PACKET;

	mns->obex = g_obex_new(io, type, rx_mtu, tx_mtu);
	if (!mns->obex) {
		mns_failed(mns);
		return;
	}

	g_obex_set_disconnect_function(mns->obex, mns_disconnected, mns);
	mns->request = g_obex_connect(mns->obex, mns_connected, mns, NULL,
					G_OBEX_HDR_TARGET, mns_target,
					sizeof(mns_target), G_OBEX_HDR_INVALID);

	if (!mns->request)
		mns_failed(mns);
}

static void mns_connect(struct map_mns *mns)
{
	if (mns->transport_id)
		return;

	mns->transport_id = mns->transport->connect(mns->source,
				mns->destination, OBEX_MNS_UUID, 0,
				transport_connected, mns);
	if (mns->transport_id)
		mns->connect_timeout = g_timeout_add_seconds(20,
							connect_expired, mns);
	else
		mns_failed(mns);
}

struct map_mns *map_mns_new(const char *source, const char *destination)
{
	struct obc_transport *transport = obc_transport_find("Bluetooth");
	struct map_mns *mns;

	if (!transport || !source || !destination)
		return NULL;

	mns = g_new0(struct map_mns, 1);
	mns->transport = transport;
	mns->source = g_strdup(source);
	mns->destination = g_strdup(destination);
	mns_connect(mns);

	return mns;
}

void map_mns_free(struct map_mns *mns)
{
	if (!mns)
		return;

	mns_close(mns);
	g_free(mns->source);
	g_free(mns->destination);
	g_free(mns);
}

static void append_attribute(GString *xml, const char *name, const char *value)
{
	char *attribute = g_markup_printf_escaped(" %s=\"%s\"", name, value);

	g_string_append(xml, attribute);
	g_free(attribute);
}

void map_mns_send(struct map_mns *mns, const struct messages_event *event)
{
	static const char * const types[] = {
		[MET_NEW_MESSAGE] = "NewMessage",
		[MET_DELIVERY_SUCCESS] = "DeliverySuccess",
		[MET_SENDING_SUCCESS] = "SendingSuccess",
		[MET_DELIVERY_FAILURE] = "DeliveryFailure",
		[MET_SENDING_FAILURE] = "SendingFailure",
		[MET_MEMORY_FULL] = "MemoryFull",
		[MET_MEMORY_AVAILABLE] = "MemoryAvailable",
		[MET_MESSAGE_DELETED] = "MessageDeleted",
		[MET_MESSAGE_SHIFT] = "MessageShift",
	};
	struct event_report *report;
	GString *xml;
	gboolean memory;

	if (!mns || !event || event->type < 0 ||
			event->type >= G_N_ELEMENTS(types) ||
			g_queue_get_length(&mns->events) >= 64)
		return;

	memory = event->type == MET_MEMORY_FULL ||
					event->type == MET_MEMORY_AVAILABLE;
	if (!memory && (!event->handle || !event->msg_type))
		return;

	xml = g_string_new("<?xml version=\"1.0\"?>\r\n"
				"<MAP-event-report version=\"1.0\">");
	g_string_append_printf(xml, "<event type=\"%s\"", types[event->type]);

	if (!memory) {
		append_attribute(xml, "handle", event->handle);
		append_attribute(xml, "msg_type", event->msg_type);
		if (event->folder)
			append_attribute(xml, "folder", event->folder);
		if (event->type == MET_MESSAGE_SHIFT && event->old_folder)
			append_attribute(xml, "old_folder", event->old_folder);
	}

	g_string_append(xml, "/></MAP-event-report>");
	report = g_new(struct event_report, 1);
	report->instance = event->instance_id;
	report->xml = g_string_free(xml, FALSE);
	g_queue_push_tail(&mns->events, report);
	mns_connect(mns);
	send_next(mns);
}
