// SPDX-License-Identifier: GPL-2.0-or-later

int map_sms_decode(char *data, char **number, char **body);
char *map_sms_encode(const char *number, const char *body, gboolean read,
				gboolean incoming, const char *folder);
