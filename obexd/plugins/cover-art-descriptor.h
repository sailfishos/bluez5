/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef COVER_ART_DESCRIPTOR_H
#define COVER_ART_DESCRIPTOR_H

#include "cover-art-image.h"

struct cover_art_request {
	unsigned int width;
	unsigned int height;
	size_t maxsize;
	enum cover_art_transform transform;
};

int cover_art_descriptor_parse(const void *data, size_t len,
		unsigned int width, unsigned int height,
		struct cover_art_request *request);

#endif
