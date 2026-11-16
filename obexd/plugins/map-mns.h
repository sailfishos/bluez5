// SPDX-License-Identifier: GPL-2.0-or-later

struct map_mns;
struct messages_event;

struct map_mns *map_mns_new(const char *source, const char *destination);
void map_mns_free(struct map_mns *mns);
void map_mns_send(struct map_mns *mns, const struct messages_event *event);
