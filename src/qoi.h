/*

QOI decoder -- Quite OK Image format
Vendored for OpenTyrian's Phase S5 bundle compression (STANDALONE_PLAN.md).

-----------------------------------------------------------------------------
Original QOI implementation (encoder + decoder):

Copyright (c) 2021, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT

    https://qoiformat.org
    https://github.com/phoboslab/qoi

This file is a trimmed, DECODE-ONLY derivative of the reference qoi.h,
carrying forward its license below. The encoder half of the reference
implementation is intentionally not vendored here; OpenTyrian's bundle
packer (tools/mkbundle.py) re-implements a from-spec QOI encoder in
Python so the C engine only ever needs to decode.

-----------------------------------------------------------------------------
MIT License

Copyright (c) 2021 Dominic Szablewski

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.

-----------------------------------------------------------------------------
Format summary (see https://qoiformat.org/qoi-specification.pdf for the
full spec this decoder implements):

A QOI stream (as produced by tools/mkbundle.py for OpenTyrian's HDPX
payloads) is RGBA8888 pixels, one scanline after another, top to bottom,
left to right, encoded as a stream of tagged chunks:

    QOI_OP_INDEX (2 bits: 00)  -- reuse one of 64 previously seen pixels
    QOI_OP_DIFF  (2 bits: 01)  -- small per-channel delta from previous pixel
    QOI_OP_LUMA  (2 bits: 10)  -- luma-biased delta from previous pixel
    QOI_OP_RUN   (2 bits: 11)  -- repeat the previous pixel 1..62 times
    QOI_OP_RGB   (8 bits: 0xFE) -- full RGB, alpha unchanged
    QOI_OP_RGBA  (8 bits: 0xFF) -- full RGBA

This header only implements decoding: qoi_decode() below turns a QOI byte
stream back into raw RGBA8888 pixels, byte-for-byte identical to the
original input the encoder was given (QOI is lossless).
*/
#ifndef QOI_H
#define QOI_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
	uint8_t r, g, b, a;
} qoi_rgba_t;

#define QOI_OP_INDEX 0x00 /* 00xxxxxx */
#define QOI_OP_DIFF  0x40 /* 01xxxxxx */
#define QOI_OP_LUMA  0x80 /* 10xxxxxx */
#define QOI_OP_RUN   0xc0 /* 11xxxxxx */
#define QOI_OP_RGB   0xfe /* 11111110 */
#define QOI_OP_RGBA  0xff /* 11111111 */

#define QOI_MASK_2   0xc0 /* 11000000 */

#define QOI_COLOR_HASH(C) \
	((uint32_t)(C).r * 3 + (uint32_t)(C).g * 5 + (uint32_t)(C).b * 7 + (uint32_t)(C).a * 11)

/*
 * Decode a QOI byte stream (raw chunk data -- no external container header;
 * OpenTyrian stores width/height/channels separately in the HDPX header) into
 * freshly malloc'd RGBA8888 pixels.
 *
 * `data`/`size` is the QOI chunk stream. `width`/`height` are the known
 * (already-parsed-elsewhere) image dimensions. Returns a malloc'd buffer of
 * width*height*4 bytes (caller frees), or NULL on malformed input / OOM.
 */
static inline uint8_t *qoi_decode(const uint8_t *data, size_t size, uint32_t width, uint32_t height)
{
	if (data == NULL || width == 0 || height == 0)
		return NULL;

	size_t px_count = (size_t)width * (size_t)height;
	if (px_count == 0 || px_count > (SIZE_MAX / 4))
		return NULL;

	size_t pixels_bytes = px_count * 4;
	uint8_t *pixels = malloc(pixels_bytes);
	if (pixels == NULL)
		return NULL;

	qoi_rgba_t index[64];
	memset(index, 0, sizeof(index));

	qoi_rgba_t px;
	px.r = 0; px.g = 0; px.b = 0; px.a = 255;

	size_t px_pos = 0;
	size_t chunks_pos = 0;
	int run = 0;

	for (px_pos = 0; px_pos < pixels_bytes; px_pos += 4)
	{
		if (run > 0)
		{
			run--;
		}
		else if (chunks_pos < size)
		{
			uint8_t b1 = data[chunks_pos++];

			if (b1 == QOI_OP_RGB)
			{
				if (chunks_pos + 3 > size)
					goto malformed;
				px.r = data[chunks_pos++];
				px.g = data[chunks_pos++];
				px.b = data[chunks_pos++];
			}
			else if (b1 == QOI_OP_RGBA)
			{
				if (chunks_pos + 4 > size)
					goto malformed;
				px.r = data[chunks_pos++];
				px.g = data[chunks_pos++];
				px.b = data[chunks_pos++];
				px.a = data[chunks_pos++];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_INDEX)
			{
				px = index[b1 & 0x3f];
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_DIFF)
			{
				px.r = (uint8_t)(px.r + (((b1 >> 4) & 0x03) - 2));
				px.g = (uint8_t)(px.g + (((b1 >> 2) & 0x03) - 2));
				px.b = (uint8_t)(px.b + ((b1 & 0x03) - 2));
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_LUMA)
			{
				if (chunks_pos + 1 > size)
					goto malformed;
				uint8_t b2 = data[chunks_pos++];
				int vg = (b1 & 0x3f) - 32;
				px.r = (uint8_t)(px.r + (vg - 8 + ((b2 >> 4) & 0x0f)));
				px.g = (uint8_t)(px.g + vg);
				px.b = (uint8_t)(px.b + (vg - 8 + (b2 & 0x0f)));
			}
			else if ((b1 & QOI_MASK_2) == QOI_OP_RUN)
			{
				run = (b1 & 0x3f);
			}

			index[QOI_COLOR_HASH(px) % 64] = px;
		}

		pixels[px_pos + 0] = px.r;
		pixels[px_pos + 1] = px.g;
		pixels[px_pos + 2] = px.b;
		pixels[px_pos + 3] = px.a;
	}

	return pixels;

malformed:
	free(pixels);
	return NULL;
}

#endif // QOI_H
