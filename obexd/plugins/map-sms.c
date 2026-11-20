// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>

#include "messages.h"
#include "map-sms.h"

#define MSG_START "BEGIN:MSG\r\n"
#define MSG_END "\r\nEND:MSG\r\n"

static char *next_line(char **cursor)
{
	char *line = *cursor;
	char *end = strstr(line, "\r\n");

	if (!end)
		return NULL;

	*end = '\0';
	*cursor = end + 2;

	return line;
}

static gboolean expect_line(char **cursor, const char *expected)
{
	return !g_strcmp0(next_line(cursor), expected);
}

static gboolean sms_number_valid(const char *number)
{
	size_t len;

	if (number[0] == '+')
		number++;

	len = strlen(number);

	return len > 0 && len <= 20 && strspn(number, "0123456789") == len;
}

static gboolean read_vcard(char **cursor, char **number)
{
	char *line;

	if (!expect_line(cursor, "BEGIN:VCARD"))
		return FALSE;

	while ((line = next_line(cursor))) {
		if (!strcmp(line, "END:VCARD"))
			return !number || *number;

		if (number && !g_ascii_strncasecmp(line, "TEL", 3) &&
					(line[3] == ':' || line[3] == ';')) {
			char *value = strchr(line, ':');

			if (*number || !value || !sms_number_valid(++value))
				return FALSE;

			*number = value;
		}
	}

	return FALSE;
}

static char *escape_msg(const char *body, gboolean decode)
{
	GString *result = g_string_new(NULL);
	const char *line = body;

	while (line) {
		const char *end = strstr(line, "\r\n");
		const char *marker = line + strspn(line, "/");

		if (g_str_has_prefix(marker, "END:MSG")) {
			if (!decode)
				g_string_append_c(result, '/');
			else if (marker != line)
				line++;
		}

		if (!end) {
			g_string_append(result, line);
			break;
		}

		g_string_append_len(result, line, end + 2 - line);
		line = end + 2;
	}

	return g_string_free(result, FALSE);
}

int map_sms_decode(char *data, char **number, char **body)
{
	char *line, *recipient = NULL, *end;
	guint64 length;
	size_t framing = sizeof(MSG_START) + sizeof(MSG_END) - 2;

	if (!expect_line(&data, "BEGIN:BMSG"))
		return -EBADR;

	line = next_line(&data);
	if (g_strcmp0(line, "VERSION:1.0") && g_strcmp0(line, "VERSION:1.1"))
		return -EBADR;

	line = next_line(&data);
	if (g_strcmp0(line, "STATUS:READ") && g_strcmp0(line, "STATUS:UNREAD"))
		return -EBADR;

	if (!expect_line(&data, "TYPE:SMS_GSM"))
		return -EMEDIUMTYPE;

	if (g_str_has_prefix(data, "FOLDER:"))
		next_line(&data);

	if (g_str_has_prefix(data, "BEGIN:VCARD\r\n") &&
						!read_vcard(&data, NULL))
		return -EBADR;

	if (!expect_line(&data, "BEGIN:BENV") ||
			!read_vcard(&data, &recipient) ||
			!expect_line(&data, "BEGIN:BBODY") ||
			!expect_line(&data, "CHARSET:UTF-8"))
		return -EBADR;

	if (g_str_has_prefix(data, "LANGUAGE:"))
		next_line(&data);

	line = next_line(&data);
	if (!line || !g_str_has_prefix(line, "LENGTH:") ||
						!g_ascii_isdigit(line[7]))
		return -EBADR;

	length = g_ascii_strtoull(line + 7, &end, 10);
	if (*end || length <= framing || length > strlen(data) ||
			!g_str_has_prefix(data, MSG_START) ||
			strcmp(data + length - sizeof(MSG_END) + 1,
				MSG_END "END:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n"))
		return -EBADR;

	data[length - sizeof(MSG_END) + 1] = '\0';
	data += sizeof(MSG_START) - 1;

	if (!g_utf8_validate(data, -1, NULL))
		return -EBADR;

	*number = recipient;
	*body = escape_msg(data, TRUE);

	return 0;
}

char *map_sms_encode(const char *number, const char *body, gboolean read,
				gboolean incoming, const char *folder)
{
	char *escaped, *vcard, *result;
	size_t length;

	escaped = escape_msg(body, FALSE);
	length = strlen(escaped) + sizeof(MSG_START) + sizeof(MSG_END) - 2;

	/* Alphanumeric SMS senders are valid. Keep vCard fields on one line. */
	vcard = g_strdup_printf("BEGIN:VCARD\r\nVERSION:2.1\r\nN:\r\n"
			"TEL:%.*s\r\nEND:VCARD\r\n",
			(int) strcspn(number, "\r\n"), number);
	result = g_strdup_printf("BEGIN:BMSG\r\nVERSION:1.0\r\nSTATUS:%s\r\n"
				"TYPE:SMS_GSM\r\nFOLDER:%s\r\n%s"
				"BEGIN:BENV\r\n%sBEGIN:BBODY\r\n"
				"CHARSET:UTF-8\r\nLENGTH:%zu\r\n"
				MSG_START "%s" MSG_END
				"END:BBODY\r\nEND:BENV\r\nEND:BMSG\r\n",
				read ? "READ" : "UNREAD", folder,
				incoming ? vcard : "", incoming ? "" : vcard,
				length, escaped);
	g_free(vcard);
	g_free(escaped);

	return result;
}
