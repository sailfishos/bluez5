/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef COVER_ART_IMAGE_H
#define COVER_ART_IMAGE_H

#include <stddef.h>

struct cover_art_image {
	char *data;
	size_t size;
	unsigned int width;
	unsigned int height;
};

#define COVER_ART_IMAGE_LIMIT 4096

enum cover_art_transform {
	COVER_ART_FILL,
	COVER_ART_STRETCH,
	COVER_ART_CROP
};

int cover_art_image_load(int fd, struct cover_art_image *native,
		struct cover_art_image *thumbnail);
int cover_art_image_resize(const void *data, size_t size, unsigned int width,
		unsigned int height, enum cover_art_transform transform,
		size_t maxsize, struct cover_art_image *image);

#endif
