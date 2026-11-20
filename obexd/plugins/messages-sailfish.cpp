// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <glib.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QStringList>

#include <CommHistory/ConversationModel>
#include <CommHistory/DatabaseIO>
#include <CommHistory/Event>
#include <CommHistory/Recipient>
#include <CommHistory/updateslistener.h>

extern "C" {
#include "messages.h"
#include "map-sms.h"
}

#define INBOX "telecom/msg/inbox"
#define OUTBOX "telecom/msg/outbox"
#define SENT "telecom/msg/sent"
#define TYPE "SMS_GSM"
#define MAX_SMS_UPLOAD (16 * 1024)

using CommHistory::Event;
using CommHistory::EventModel;

struct sms_session {
	QByteArray folder;
	QByteArray upload;
	messages_push_cb push_callback = nullptr;
	void *push_data = nullptr;
	CommHistory::UpdatesListener listener;
	guint request = 0;
	QDBusPendingCallWatcher *send = nullptr;
};

static QCoreApplication *qt_app;

void messages_get_capabilities(uint8_t *types, uint32_t *features)
{
	*types = 0x02;
	*features = 0x0000000f;
}

int messages_init(void)
{
	static int argc = 1;
	static char name[] = "obexd";
	static char *argv[] = { name, nullptr };

	qt_app = new QCoreApplication(argc, argv);
	qDBusRegisterMetaType<QList<QVariantMap>>();

	return 0;
}

void messages_exit(void)
{
	delete qt_app;
	qt_app = nullptr;
}

static void defer(sms_session *session, std::function<void()> action)
{
	using Callback = std::function<void()>;
	auto *callback = new Callback([session, action] {
		session->request = 0;
		action();
	});

	session->request = g_idle_add_full(G_PRIORITY_DEFAULT,
		[](gpointer data) {
			(*static_cast<Callback *>(data))();
			return G_SOURCE_REMOVE;
		}, callback, [](gpointer data) {
			delete static_cast<Callback *>(data);
		});
}

void messages_abort(void *data)
{
	auto *session = static_cast<sms_session *>(data);

	if (session->request) {
		g_source_remove(session->request);
		session->request = 0;
	}

	delete session->send;
	session->send = nullptr;
	session->upload.clear();
	session->push_callback = nullptr;
	session->push_data = nullptr;
}

int messages_connect(void **out)
{
	*out = new sms_session;
	return 0;
}

void messages_disconnect(void *data)
{
	messages_abort(data);
	delete static_cast<sms_session *>(data);
}

static QString event_number(const Event &event)
{
	const auto recipients = event.recipients().recipients();

	return recipients.isEmpty() ? QString() : recipients.first().remoteUid();
}

static QByteArray handle_for(const Event &event)
{
	return QByteArray::number(event.id(), 16).rightJustified(16, '0').toUpper();
}

static Event find_sms(const char *handle)
{
	const QByteArray value(handle);
	bool ok;
	Event event;

	/* Some clients omit the leading zeroes from the listed handle. */
	if (value.isEmpty() || value.size() > 16 ||
		std::strspn(value.constData(), "0123456789abcdefABCDEF") !=
							size_t(value.size()))
		return Event();

	const auto id = value.toULongLong(&ok, 16);

	if (!ok || id > INT_MAX ||
				!CommHistory::DatabaseIO::instance()->getEvent(id, event) ||
				event.type() != Event::SMSEvent)
		return Event();

	return event;
}

static const char *folder_for(const Event &event)
{
	if (event.direction() == Event::Inbound)
		return INBOX;

	switch (event.status()) {
	case Event::SendingStatus:
	case Event::TemporarilyFailedStatus:
	case Event::PermanentlyFailedStatus:
		return OUTBOX;
	default:
		return SENT;
	}
}

int messages_set_notification_registration(void *data,
		void (*callback)(void *, const struct messages_event *, void *),
		void *user_data)
{
	auto *session = static_cast<sms_session *>(data);

	QObject::disconnect(&session->listener, nullptr, &session->listener, nullptr);

	if (callback) {
		QObject::connect(&session->listener,
			&CommHistory::UpdatesListener::eventsAdded,
			&session->listener,
			[session, callback, user_data](const QList<Event> &events) {
				for (const Event &event : events) {
					if (event.type() != Event::SMSEvent ||
						event.direction() != Event::Inbound)
						continue;

					QByteArray handle = handle_for(event);
					messages_event report = {
						MET_NEW_MESSAGE, 0, handle.data(),
						const_cast<char *>(INBOX), nullptr,
						const_cast<char *>(TYPE)
					};
					callback(session, &report, user_data);
				}
			});
	}

	return 0;
}

static QByteArray child_path(const QByteArray &folder, const char *name)
{
	if (!name || !*name)
		return folder;

	const QByteArray child = QByteArray(name).toLower();

	return folder.isEmpty() ? child : folder + '/' + child;
}

static bool valid_folder(const QByteArray &path)
{
	return path.isEmpty() || path == "telecom" || path == "telecom/msg" ||
		path == INBOX || path == OUTBOX || path == SENT ||
		path == "telecom/msg/deleted" || path == "telecom/msg/draft";
}

int messages_set_folder(void *data, const char *name, gboolean cdup)
{
	auto *session = static_cast<sms_session *>(data);
	QByteArray path = session->folder;

	if (cdup) {
		int slash = path.lastIndexOf('/');
		path = slash < 0 ? QByteArray() : path.left(slash);
	} else if (!name || !*name) {
		path.clear();
	}

	path = child_path(path, name);

	if (!valid_folder(path))
		return -ENOENT;

	session->folder = path;

	return 0;
}

int messages_get_folder_listing(void *data, const char *name, uint16_t max,
		uint16_t offset, messages_folder_listing_cb callback, void *user_data)
{
	auto *session = static_cast<sms_session *>(data);
	const QByteArray path = child_path(session->folder, name);
	QList<QByteArray> children;

	if (!valid_folder(path))
		return -ENOENT;

	if (path.isEmpty())
		children = { "telecom" };
	else if (path == "telecom")
		children = { "msg" };
	else if (path == "telecom/msg")
		children = { "inbox", "outbox", "sent", "deleted", "draft" };

	defer(session, [=] {
		const int count = children.size();

		for (int i = offset; i < count && i - offset < max; i++)
			callback(data, -EAGAIN, count, children[i].constData(),
								user_data);

		callback(data, 0, count, nullptr, user_data);
	});

	return 0;
}

static void list_message(void *data, const Event &event, uint8_t subject_len,
		uint32_t mask, uint16_t count, bool unread,
		messages_get_messages_listing_cb callback, void *user_data)
{
	QByteArray handle = handle_for(event);
	QByteArray number = event_number(event).toUtf8();
	QByteArray timestamp = event.startTime().toLocalTime().toString(
						"yyyyMMdd'T'hhmmss").toLatin1();
	const QString text = event.freeText();
	auto characters = text.toUcs4();
	QByteArray subject = QString::fromUcs4(characters.constData(),
			qMin(characters.size(), subject_len ? subject_len : 256)).toUtf8();
	QByteArray size = QByteArray::number(text.toUtf8().size());
	const bool incoming = event.direction() == Event::Inbound;
	messages_message message = {};

	message.mask = (mask ? mask : UINT32_MAX) &
		(PMASK_SUBJECT | PMASK_DATETIME | PMASK_SENDER_ADDRESSING |
		 PMASK_RECIPIENT_ADDRESSING | PMASK_TYPE | PMASK_SIZE |
		 PMASK_RECEPTION_STATUS | PMASK_ATTACHMENT_SIZE | PMASK_TEXT |
		 PMASK_READ | PMASK_SENT | PMASK_PROTECTED | PMASK_PRIORITY);
	message.handle = handle.data();
	message.subject = subject.data();
	message.datetime = timestamp.data();
	message.sender_addressing = incoming ? number.data() : const_cast<char *>("");
	message.recipient_addressing = incoming ? const_cast<char *>("") : number.data();
	message.type = const_cast<char *>(TYPE);
	message.reception_status = const_cast<char *>("complete");
	message.attachment_size = const_cast<char *>("0");
	message.size = size.data();
	message.text = !text.isEmpty();
	message.read = event.isRead();
	message.sent = !incoming && g_str_equal(folder_for(event), SENT);

	callback(data, -EAGAIN, count, unread, &message, user_data);
}

int messages_get_messages_listing(void *data, const char *name, uint16_t max,
		uint16_t offset, uint8_t subject_len, const messages_filter *filter,
		messages_get_messages_listing_cb callback, void *user_data)
{
	auto *session = static_cast<sms_session *>(data);
	const QByteArray path = child_path(session->folder, name);
	const uint32_t mask = filter->parameter_mask;
	QList<Event> matches;
	bool unread = false;

	if (!valid_folder(path))
		return -ENOENT;

	if ((path == INBOX || path == SENT || path == OUTBOX) &&
					!(filter->type & 1) && filter->priority != 1) {
		CommHistory::ConversationModel model;
		model.setTreeMode(false);
		model.setQueryMode(EventModel::SyncQuery);
		model.setResolveContacts(EventModel::DoNotResolve);
		model.setEventCategoryMask(Event::ShortMessagingCategory);

		if (!model.getEvents())
			return -EIO;

		for (int i = 0; i < model.rowCount(); i++) {
			const Event event = model.event(i);

			if (event.type() != Event::SMSEvent || path != folder_for(event))
				continue;

			if ((filter->read_status == 1 && event.isRead()) ||
					(filter->read_status == 2 && !event.isRead()))
				continue;

			const bool incoming = event.direction() == Event::Inbound;
			const QByteArray number = event_number(event).toUtf8();
			const QByteArray time = event.startTime().toLocalTime().toString(
							"yyyyMMdd'T'hhmmss").toLatin1();

			if ((filter->period_begin && time < filter->period_begin) ||
					(filter->period_end && time > filter->period_end))
				continue;

			if ((filter->originator && !strstr(incoming ? number.constData() : "",
								filter->originator)) ||
					(filter->recipient && !strstr(incoming ? "" : number.constData(),
								filter->recipient)))
				continue;

			matches.append(event);
			unread |= !event.isRead();
		}
	}

	defer(session, [=] {
		const int count = qMin(matches.size(), int(UINT16_MAX));

		for (int i = offset; i < count && i - offset < max; i++)
			list_message(data, matches[i], subject_len, mask, count,
							unread, callback, user_data);

		callback(data, 0, count, unread, nullptr, user_data);
	});

	return 0;
}

int messages_get_message(void *data, const char *handle, unsigned long flags,
		messages_get_message_cb callback, void *user_data)
{
	if (!(flags & MESSAGES_UTF8))
		return -ENOSYS;

	Event event = find_sms(handle);

	if (!event.isValid())
		return -ENOENT;

	defer(static_cast<sms_session *>(data), [=] {
		char *encoded = map_sms_encode(event_number(event).toUtf8().constData(),
			event.freeText().toUtf8().constData(), event.isRead(),
			event.direction() == Event::Inbound, folder_for(event));

		callback(data, -EAGAIN, FALSE, encoded, user_data);
		callback(data, 0, FALSE, nullptr, user_data);
		g_free(encoded);
	});

	return 0;
}

int messages_update_inbox(void *data, messages_status_cb callback, void *user_data)
{
	defer(static_cast<sms_session *>(data), [=] { callback(data, 0, user_data); });
	return 0;
}

int messages_set_read(void *, const char *, uint8_t, messages_status_cb, void *)
{
	return -ENOSYS;
}

int messages_set_delete(void *, const char *, uint8_t, messages_status_cb, void *)
{
	return -ENOSYS;
}

static const char tp_account_manager[] = "org.freedesktop.Telepathy.AccountManager";
static const char tp_account_path[] = "/org/freedesktop/Telepathy/AccountManager";
static const char tp_dispatcher[] = "org.freedesktop.Telepathy.ChannelDispatcher";
static const char tp_dispatcher_path[] = "/org/freedesktop/Telepathy/ChannelDispatcher";
static const char tp_ring_prefix[] = "/org/freedesktop/Telepathy/Account/ring/tel/";

static void watch_call(sms_session *session, const QDBusMessage &call,
		std::function<void(QDBusPendingCallWatcher *)> complete)
{
	auto pending = QDBusConnection::sessionBus().asyncCall(call, 20000);
	auto *watcher = new QDBusPendingCallWatcher(pending);
	session->send = watcher;

	QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher,
			[=](QDBusPendingCallWatcher *finished) {
		session->send = nullptr;
		complete(finished);
		finished->deleteLater();
	});
}

static void wait_for_sent_sms(sms_session *session, const QString &token,
				messages_push_cb callback, void *user_data)
{
	using Callback = std::function<gboolean()>;
	const gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
	auto *check = new Callback([=]() -> gboolean {
		Event event;
		const bool found = !token.isEmpty() &&
			CommHistory::DatabaseIO::instance()->getEventByMessageToken(token, event);

		if (found && event.isValid() && event.type() == Event::SMSEvent &&
							event.direction() == Event::Outbound) {
			const QByteArray handle = handle_for(event);

			session->request = 0;
			callback(session, 0, handle.constData(), user_data);
			return G_SOURCE_REMOVE;
		}

		if (!token.isEmpty() && g_get_monotonic_time() < deadline)
			return G_SOURCE_CONTINUE;

		session->request = 0;
		callback(session, -EFAULT, nullptr, user_data);
		return G_SOURCE_REMOVE;
	});

	session->request = g_timeout_add_full(G_PRIORITY_DEFAULT, 100,
		[](gpointer data) { return (*static_cast<Callback *>(data))(); },
		check, [](gpointer data) { delete static_cast<Callback *>(data); });
}

static int messages_push_sms(void *data, const char *recipient, const char *body,
			messages_push_cb callback, void *user_data)
{
	auto *session = static_cast<sms_session *>(data);
	const QString number = QString::fromUtf8(recipient);
	const QString text = QString::fromUtf8(body);
	auto get_accounts = QDBusMessage::createMethodCall(tp_account_manager,
		tp_account_path, "org.freedesktop.DBus.Properties", "Get");
	get_accounts << QString::fromLatin1(tp_account_manager) << QStringLiteral("ValidAccounts");

	watch_call(session, get_accounts, [=](QDBusPendingCallWatcher *finished) {
		const QDBusPendingReply<QDBusVariant> result = *finished;
		QDBusObjectPath account;

		if (!result.isError()) {
			const auto accounts = qdbus_cast<QList<QDBusObjectPath>>(
							result.value().variant());

			for (const auto &candidate : accounts) {
				if (candidate.path().startsWith(QLatin1String(tp_ring_prefix))) {
					account = candidate;
					break;
				}
			}
		}

		if (account.path().isEmpty()) {
			callback(data, -EFAULT, nullptr, user_data);
			return;
		}

		const QList<QVariantMap> parts = {
			{},
			{{"content-type", "text/plain"}, {"content", text}}
		};
		auto send = QDBusMessage::createMethodCall(tp_dispatcher,
			tp_dispatcher_path,
			"org.freedesktop.Telepathy.ChannelDispatcher.Interface.Messages.DRAFT",
			"SendMessage");
		send << QVariant::fromValue(account) << number
			<< QVariant::fromValue(parts) << uint(1);

		watch_call(session, send, [=](QDBusPendingCallWatcher *finished) {
			const QDBusPendingReply<QString> reply = *finished;

			if (reply.isError()) {
				callback(data, -EFAULT, nullptr, user_data);
				return;
			}

			/*
			 * Telepathy may return before history stores MessageSent.
			 * Return the same handle used by listings and retrieval.
			 */
			wait_for_sent_sms(session, reply.value(), callback, user_data);
		});
	});

	return 0;
}

int messages_push_message(void *data, const char *name, unsigned long flags,
		const size_t *size, messages_push_cb callback, void *user_data)
{
	auto *session = static_cast<sms_session *>(data);
	QByteArray path = name ? QByteArray(name).toLower() : QByteArray();

	if (path.startsWith('/'))
		path.remove(0, 1);

	if (path.isEmpty())
		path = session->folder;
	else if (path == "outbox" && session->folder == "telecom/msg")
		path = OUTBOX;

	if (path != OUTBOX)
		return -ENOENT;

	if (!(flags & MESSAGES_UTF8) || (flags & MESSAGES_TRANSPARENT))
		return -ENOSYS;

	if (size && *size > MAX_SMS_UPLOAD)
		return -EFBIG;

	session->upload.clear();
	session->push_callback = callback;
	session->push_data = user_data;
	return 0;
}

ssize_t messages_push_write(void *data, const void *buf, size_t count)
{
	auto *session = static_cast<sms_session *>(data);

	if (count > MAX_SMS_UPLOAD - size_t(session->upload.size()))
		return -EFBIG;

	session->upload.append(static_cast<const char *>(buf), int(count));
	return count;
}

int messages_push_end(void *data)
{
	auto *session = static_cast<sms_session *>(data);
	char *recipient, *body;

	if (session->upload.contains('\0'))
		return -EBADR;

	int err = map_sms_decode(session->upload.data(), &recipient, &body);

	if (err < 0)
		return err;

	err = messages_push_sms(session, recipient, body,
					session->push_callback, session->push_data);
	g_free(body);
	session->upload.clear();
	return err;
}
