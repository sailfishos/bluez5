/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <jpeglib.h>
#include <limits.h>
#include <png.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cover-art-image.h"

#define INPUT_LIMIT (1024 * 1024)
#define IMAGE_LIMIT COVER_ART_IMAGE_LIMIT
#define PIXEL_LIMIT (16 * 1024 * 1024)
#define OUTPUT_SIZE 200

struct jpeg_error {
	struct jpeg_error_mgr pub;
	jmp_buf jump;
	int warnings;
};

static void jpeg_failure(j_common_ptr cinfo)
{
	struct jpeg_error *err = (struct jpeg_error *) cinfo->err;

	longjmp(err->jump, 1);
}

static void jpeg_message(j_common_ptr cinfo, int level)
{
	struct jpeg_error *err = (struct jpeg_error *) cinfo->err;

	if (level < 0)
		err->warnings++;
}

static int valid_dimensions(unsigned int width, unsigned int height)
{
	return width && height && width <= IMAGE_LIMIT &&
		height <= IMAGE_LIMIT && (uint64_t) width * height <= PIXEL_LIMIT;
}

static int decode_png(const unsigned char *src, size_t src_size,
			unsigned char **pixels, unsigned int *width,
			unsigned int *height)
{
	static const png_color background = { 255, 255, 255 };
	static const unsigned char iend[] = {
		0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82
	};
	png_image image;
	size_t count;

	if (src_size < 20 || memcmp(src + src_size - sizeof(iend),
				iend, sizeof(iend)))
		return -EINVAL;
	memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, src, src_size)) {
		png_image_free(&image);
		return -EINVAL;
	}
	if (!valid_dimensions(image.width, image.height)) {
		png_image_free(&image);
		return -EFBIG;
	}
	image.format = PNG_FORMAT_RGB;
	count = (size_t) image.width * image.height;
	*pixels = malloc(count * 3);
	if (!*pixels) {
		png_image_free(&image);
		return -ENOMEM;
	}
	if (!png_image_finish_read(&image, &background, *pixels, 0, NULL)) {
		free(*pixels);
		*pixels = NULL;
		png_image_free(&image);
		return -EINVAL;
	}
	*width = image.width;
	*height = image.height;
	png_image_free(&image);
	return 0;
}

static int decode_jpeg(const unsigned char *src, unsigned long src_size,
			unsigned char **pixels, unsigned int *width,
			unsigned int *height)
{
	struct jpeg_decompress_struct cinfo;
	struct jpeg_error err;
	unsigned char * volatile image = NULL;
	volatile int result = -EINVAL;

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = jpeg_failure;
	err.pub.emit_message = jpeg_message;
	err.warnings = 0;
	if (setjmp(err.jump)) {
		free((void *) image);
		jpeg_destroy_decompress(&cinfo);
		return -EINVAL;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, src, src_size);
	if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK)
		goto done;
	if (!valid_dimensions(cinfo.image_width, cinfo.image_height)) {
		result = -EFBIG;
		goto done;
	}
	cinfo.out_color_space = JCS_RGB;
	if (!jpeg_start_decompress(&cinfo))
		goto done;
	if (cinfo.output_components != 3 ||
			(size_t) cinfo.output_width * cinfo.output_height >
			SIZE_MAX / 3)
		goto done;
	image = malloc((size_t) cinfo.output_width * cinfo.output_height * 3);
	if (!image) {
		result = -ENOMEM;
		goto done;
	}
	while (cinfo.output_scanline < cinfo.output_height) {
		JSAMPROW row = image + (size_t) cinfo.output_scanline *
			cinfo.output_width * 3;

		if (jpeg_read_scanlines(&cinfo, &row, 1) != 1)
			goto done;
	}
	if (!jpeg_finish_decompress(&cinfo))
		goto done;
	if (err.warnings)
		goto done;
	*width = cinfo.output_width;
	*height = cinfo.output_height;
	*pixels = (unsigned char *) image;
	image = NULL;
	result = 0;
done:
	free((void *) image);
	jpeg_destroy_decompress(&cinfo);
	return result;
}

static unsigned char *make_canvas(const unsigned char *src,
		unsigned int width, unsigned int height)
{
	unsigned char *canvas;
	unsigned int out_width, out_height, x, y, left, top;

	if (width >= height) {
		out_width = OUTPUT_SIZE;
		out_height = ((uint64_t) height * OUTPUT_SIZE + width / 2) / width;
	} else {
		out_height = OUTPUT_SIZE;
		out_width = ((uint64_t) width * OUTPUT_SIZE + height / 2) / height;
	}
	if (!out_width)
		out_width = 1;
	if (!out_height)
		out_height = 1;
	left = (OUTPUT_SIZE - out_width) / 2;
	top = (OUTPUT_SIZE - out_height) / 2;
	canvas = malloc(OUTPUT_SIZE * OUTPUT_SIZE * 3);
	if (!canvas)
		return NULL;
	memset(canvas, 255, OUTPUT_SIZE * OUTPUT_SIZE * 3);
	for (y = 0; y < out_height; y++) {
		unsigned int sy = (uint64_t) y * height / out_height;

		for (x = 0; x < out_width; x++) {
			unsigned int sx = (uint64_t) x * width / out_width;
			memcpy(canvas + ((size_t) (top + y) * OUTPUT_SIZE + left + x) * 3,
				src + ((size_t) sy * width + sx) * 3, 3);
		}
	}
	return canvas;
}

static unsigned char *resize_image(const unsigned char *src,
		unsigned int width, unsigned int height, unsigned int max_edge,
		unsigned int *out_width, unsigned int *out_height)
{
	unsigned char *image;
	unsigned int x, y;

	if (width >= height) {
		*out_width = width > max_edge ? max_edge : width;
		*out_height = (uint64_t) height * *out_width / width;
	} else {
		*out_height = height > max_edge ? max_edge : height;
		*out_width = (uint64_t) width * *out_height / height;
	}
	if (!*out_width)
		*out_width = 1;
	if (!*out_height)
		*out_height = 1;
	image = malloc((size_t) *out_width * *out_height * 3);
	if (!image)
		return NULL;
	for (y = 0; y < *out_height; y++)
		for (x = 0; x < *out_width; x++)
			memcpy(image + ((size_t) y * *out_width + x) * 3,
				src + ((size_t) ((uint64_t) y * height / *out_height) *
				width + (uint64_t) x * width / *out_width) * 3, 3);
	return image;
}

static int encode_jpeg(const unsigned char *pixels, unsigned int width,
		unsigned int height, int quality,
		struct cover_art_image *image)
{
	static const unsigned char exif[] = {
		'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0,
		0, 0, 0, 0, 14, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};
	struct jpeg_compress_struct cinfo;
	struct jpeg_error err;
	unsigned long jpeg_size = 0;
	unsigned char * volatile jpeg_data = NULL;

	memset(&cinfo, 0, sizeof(cinfo));
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = jpeg_failure;
	err.pub.emit_message = jpeg_message;
	err.warnings = 0;
	if (setjmp(err.jump)) {
		free((void *) jpeg_data);
		jpeg_destroy_compress(&cinfo);
		return -EINVAL;
	}
	jpeg_create_compress(&cinfo);
	jpeg_mem_dest(&cinfo, (unsigned char **) &jpeg_data, &jpeg_size);
	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	cinfo.comp_info[0].h_samp_factor = 2;
	cinfo.comp_info[0].v_samp_factor = 1;
	cinfo.comp_info[1].h_samp_factor = 1;
	cinfo.comp_info[1].v_samp_factor = 1;
	cinfo.comp_info[2].h_samp_factor = 1;
	cinfo.comp_info[2].v_samp_factor = 1;
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	jpeg_write_marker(&cinfo, JPEG_APP0 + 1, exif, sizeof(exif));
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = (unsigned char *) pixels +
			(size_t) cinfo.next_scanline * width * 3;

		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	image->data = (char *) jpeg_data;
	image->size = jpeg_size;
	image->width = width;
	image->height = height;
	return 0;
}

int cover_art_image_load(int fd, struct cover_art_image *native,
		struct cover_art_image *thumbnail)
{
	struct stat st;
	unsigned char *input = NULL, *pixels = NULL, *canvas = NULL;
	unsigned char *native_pixels = NULL;
	size_t input_size, offset = 0;
	unsigned int width, height, native_width, native_height;
	int result = -EINVAL;

	if (!native || !thumbnail || native == thumbnail)
		return -EINVAL;
	memset(native, 0, sizeof(*native));
	memset(thumbnail, 0, sizeof(*thumbnail));
	if (fstat(fd, &st) < 0)
		return -errno;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > INPUT_LIMIT)
		return -EFBIG;
	input_size = st.st_size;
	input = malloc(input_size);
	if (!input)
		return -ENOMEM;
	while (offset < input_size) {
		ssize_t n = pread(fd, input + offset, input_size - offset, offset);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			result = n < 0 ? -errno : -EIO;
			goto out;
		}
		offset += n;
	}
	if (input_size >= 8 && !png_sig_cmp(input, 0, 8))
		result = decode_png(input, input_size, &pixels, &width, &height);
	else if (input_size >= 2 && input[0] == 0xff && input[1] == 0xd8)
		result = decode_jpeg(input, input_size, &pixels, &width, &height);
	else
		goto out;
	if (result < 0)
		goto out;
	native_width = width;
	native_height = height;
	if (width > 1024 || height > 1024) {
		native_pixels = resize_image(pixels, width, height, 1024,
				&native_width, &native_height);
		if (!native_pixels) {
			result = -ENOMEM;
			goto out;
		}
	}
	canvas = make_canvas(pixels, width, height);
	if (!canvas) {
		result = -ENOMEM;
		goto out;
	}
	result = encode_jpeg(native_pixels ? native_pixels : pixels,
			native_width, native_height, 85, native);
	if (result < 0)
		goto out;
	result = encode_jpeg(canvas, OUTPUT_SIZE, OUTPUT_SIZE, 85, thumbnail);
out:
	if (result < 0) {
		free(native->data);
		free(thumbnail->data);
		memset(native, 0, sizeof(*native));
		memset(thumbnail, 0, sizeof(*thumbnail));
	}
	free(native_pixels);
	free(canvas);
	free(pixels);
	free(input);
	return result;
}

static unsigned char sample_channel(const unsigned char *pixels,
		unsigned int width, unsigned int height, double x, double y,
		unsigned int channel)
{
	unsigned int x0, y0, x1, y1;
	double fx, fy, a, b;

	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x > width - 1)
		x = width - 1;
	if (y > height - 1)
		y = height - 1;
	x0 = x;
	y0 = y;
	x1 = x0 + 1 < width ? x0 + 1 : x0;
	y1 = y0 + 1 < height ? y0 + 1 : y0;
	fx = x - x0;
	fy = y - y0;
	a = pixels[((size_t) y0 * width + x0) * 3 + channel] * (1 - fx) +
		pixels[((size_t) y0 * width + x1) * 3 + channel] * fx;
	b = pixels[((size_t) y1 * width + x0) * 3 + channel] * (1 - fx) +
		pixels[((size_t) y1 * width + x1) * 3 + channel] * fx;
	return a * (1 - fy) + b * fy + 0.5;
}

int cover_art_image_resize(const void *data, size_t size, unsigned int width,
		unsigned int height, enum cover_art_transform transform,
		size_t maxsize, struct cover_art_image *image)
{
	unsigned char *source = NULL, *pixels = NULL;
	unsigned int source_width, source_height, x, y;
	double crop_x = 0, crop_y = 0, crop_width, crop_height;
	unsigned int left = 0, top = 0, draw_width = width, draw_height = height;
	size_t output_limit = maxsize && maxsize < INPUT_LIMIT ? maxsize :
		INPUT_LIMIT;
	int quality, result;

	if (!image)
		return -EINVAL;
	memset(image, 0, sizeof(*image));
	if (!data || !size || size > ULONG_MAX || !valid_dimensions(width, height) ||
			transform < COVER_ART_FILL || transform > COVER_ART_CROP)
		return -ENOTSUP;
	result = decode_jpeg(data, size, &source, &source_width, &source_height);
	if (result < 0)
		return result;
	if (transform == COVER_ART_FILL) {
		if ((uint64_t) width * source_height <=
				(uint64_t) height * source_width) {
			draw_width = width;
			draw_height = (uint64_t) source_height * width / source_width;
		} else {
			draw_height = height;
			draw_width = (uint64_t) source_width * height / source_height;
		}
		if (!draw_width)
			draw_width = 1;
		if (!draw_height)
			draw_height = 1;
		left = (width - draw_width) / 2;
		top = (height - draw_height) / 2;
	} else if (transform == COVER_ART_CROP) {
		crop_width = source_width;
		crop_height = source_height;
		if ((uint64_t) source_width * height >
				(uint64_t) source_height * width) {
			crop_width = (double) source_height * width / height;
			crop_x = (source_width - crop_width) / 2;
		} else {
			crop_height = (double) source_width * height / width;
			crop_y = (source_height - crop_height) / 2;
		}
	}
	pixels = malloc((size_t) width * height * 3);
	if (!pixels) {
		free(source);
		return -ENOMEM;
	}
	memset(pixels, 255, (size_t) width * height * 3);
	for (y = top; y < top + draw_height; y++) {
		for (x = left; x < left + draw_width; x++) {
			double sx, sy;
			unsigned int c;

			if (transform == COVER_ART_CROP) {
				sx = crop_x + ((x + 0.5) * crop_width / width) - 0.5;
				sy = crop_y + ((y + 0.5) * crop_height / height) - 0.5;
			} else {
				sx = ((x - left + 0.5) * source_width / draw_width) - 0.5;
				sy = ((y - top + 0.5) * source_height / draw_height) - 0.5;
			}
			for (c = 0; c < 3; c++)
				pixels[((size_t) y * width + x) * 3 + c] =
					sample_channel(source, source_width, source_height,
						sx, sy, c);
		}
	}
	free(source);
	for (quality = 85; quality >= 30; quality -= 10) {
		result = encode_jpeg(pixels, width, height, quality, image);
		if (result < 0)
			break;
		if (image->size <= output_limit) {
			free(pixels);
			return 0;
		}
		free(image->data);
		memset(image, 0, sizeof(*image));
	}
	free(pixels);
	if (result >= 0)
		result = -ENOTSUP;
	return result;
}
