/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
/** @file lvledit_png.c
 * See lvledit_png.h.
 *
 * Emits a spec-minimal PNG: signature, IHDR (8-bit truecolor), a single IDAT
 * chunk wrapping a zlib stream built from STORED (uncompressed) deflate
 * blocks, IEND. No zlib/libpng dependency -- everything here (CRC32, Adler32,
 * the stored-block framing) is short enough to hand-roll and verify against
 * the PNG/zlib/deflate specs directly:
 *
 *   - PNG (ISO/IEC 15948): signature, chunk framing (4-byte big-endian
 *     length, 4-byte type, data, 4-byte CRC32 over type+data), IHDR/IDAT/IEND.
 *   - RFC 1950 (zlib): 2-byte header (CMF/FLG), then the deflate stream,
 *     then a 4-byte big-endian Adler-32 of the *uncompressed* data.
 *   - RFC 1951 (deflate): a STORED block is a 1-byte header (bit 0 =
 *     BFINAL, bits 1-2 = BTYPE = 00, rest zero/byte-aligned), then LEN and
 *     NLEN (16-bit little-endian, NLEN = ~LEN), then LEN raw bytes.
 */

#include "lvledit_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------
// CRC32 (PNG chunk checksums) -- standard reflected table, per the PNG
// spec's sample implementation (Appendix D).
// ---------------------------------------------------------------------

static Uint32 crc_table[256];
static bool crc_table_ready = false;

static void make_crc_table(void)
{
	for (Uint32 n = 0; n < 256; ++n)
	{
		Uint32 c = n;
		for (int k = 0; k < 8; ++k)
			c = (c & 1) ? (0xedb88320UL ^ (c >> 1)) : (c >> 1);
		crc_table[n] = c;
	}
	crc_table_ready = true;
}

// Continues a running (not pre/post-inverted) CRC over `len` bytes.
static Uint32 crc_continue(Uint32 crc, const Uint8 *buf, size_t len)
{
	if (!crc_table_ready)
		make_crc_table();

	for (size_t n = 0; n < len; ++n)
		crc = crc_table[(crc ^ buf[n]) & 0xff] ^ (crc >> 8);

	return crc;
}

// ---------------------------------------------------------------------
// Adler32 (zlib stream checksum, over the pre-deflate/uncompressed bytes)
// ---------------------------------------------------------------------

static Uint32 adler32_buf(const Uint8 *data, size_t len)
{
	const Uint32 MOD_ADLER = 65521;
	Uint32 a = 1, b = 0;

	// Accumulate in blocks small enough that a/b can't overflow 32 bits
	// before the next mod (standard trick; 5552 is the largest block for
	// which 255*5552 + 65520 still fits in Uint32).
	size_t i = 0;
	while (i < len)
	{
		size_t block = (len - i < 5552) ? (len - i) : 5552;
		for (size_t j = 0; j < block; ++j)
		{
			a += data[i + j];
			b += a;
		}
		a %= MOD_ADLER;
		b %= MOD_ADLER;
		i += block;
	}

	return (b << 16) | a;
}

// ---------------------------------------------------------------------
// Little helpers
// ---------------------------------------------------------------------

static void put_be32(Uint8 *p, Uint32 v)
{
	p[0] = (Uint8)(v >> 24);
	p[1] = (Uint8)(v >> 16);
	p[2] = (Uint8)(v >> 8);
	p[3] = (Uint8)(v);
}

static void put_le16(Uint8 *p, Uint16 v)
{
	p[0] = (Uint8)(v);
	p[1] = (Uint8)(v >> 8);
}

// Writes `len` bytes to `f`, feeding them into the running chunk CRC.
// Returns false on I/O failure.
static bool stream_out(FILE *f, Uint32 *crc, const Uint8 *buf, size_t len)
{
	if (len > 0 && fwrite(buf, 1, len, f) != len)
		return false;

	*crc = crc_continue(*crc, buf, len);
	return true;
}

// Writes a complete small chunk (type + data known up front, data buffered
// in memory) -- used for IHDR/IEND, which are tiny and fixed-size.
static bool write_small_chunk(FILE *f, const char type[4], const Uint8 *data, Uint32 len)
{
	Uint8 hdr[8];
	put_be32(hdr, len);
	memcpy(hdr + 4, type, 4);

	if (fwrite(hdr, 1, 8, f) != 8)
		return false;

	Uint32 crc = 0xffffffffUL;
	crc = crc_continue(crc, (const Uint8 *)type, 4);

	if (len > 0)
	{
		if (fwrite(data, 1, len, f) != len)
			return false;
		crc = crc_continue(crc, data, len);
	}

	crc ^= 0xffffffffUL;

	Uint8 crc_bytes[4];
	put_be32(crc_bytes, crc);
	return fwrite(crc_bytes, 1, 4, f) == 4;
}

// ---------------------------------------------------------------------
// PNG writer
// ---------------------------------------------------------------------

bool lvledit_png_write(const char *path, const Uint8 *rgb, int w, int h)
{
	if (path == NULL || rgb == NULL || w <= 0 || h <= 0)
		return false;

	const size_t row_bytes = (size_t)w * 3;         // raw RGB bytes per row
	const size_t filt_row_bytes = row_bytes + 1;     // + 1 filter-type byte
	const size_t data_len = filt_row_bytes * (size_t)h;

	// Filtered (but not yet compressed) scanline stream: one 0x00 "None"
	// filter byte followed by the row's raw RGB bytes, per row.
	Uint8 *filtered = malloc(data_len);
	if (filtered == NULL)
		return false;

	for (int y = 0; y < h; ++y)
	{
		Uint8 *drow = filtered + (size_t)y * filt_row_bytes;
		drow[0] = 0;  // filter type 0 = None
		memcpy(drow + 1, rgb + (size_t)y * row_bytes, row_bytes);
	}

	const Uint32 adler = adler32_buf(filtered, data_len);

	// Deflate STORED framing: ceil(data_len / 65535) blocks, each with a
	// 1-byte header + 2-byte LEN + 2-byte NLEN in front of its slice.
	const size_t max_block = 65535;
	const size_t num_blocks = (data_len == 0) ? 1 : (data_len + max_block - 1) / max_block;
	const size_t idat_len = 2 /* zlib header */
	                       + num_blocks * 5
	                       + data_len
	                       + 4 /* adler32 */;

	FILE *f = fopen(path, "wb");
	if (f == NULL)
	{
		free(filtered);
		return false;
	}

	bool ok = true;

	// PNG signature.
	static const Uint8 signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
	ok = ok && fwrite(signature, 1, 8, f) == 8;

	// IHDR: width, height, bit depth 8, color type 2 (truecolor), the three
	// always-zero method bytes (compression/filter/interlace).
	Uint8 ihdr[13];
	put_be32(ihdr + 0, (Uint32)w);
	put_be32(ihdr + 4, (Uint32)h);
	ihdr[8]  = 8;  // bit depth
	ihdr[9]  = 2;  // color type: truecolor
	ihdr[10] = 0;  // compression method
	ihdr[11] = 0;  // filter method
	ihdr[12] = 0;  // interlace method
	ok = ok && write_small_chunk(f, "IHDR", ihdr, sizeof(ihdr));

	// IDAT: streamed so we never need a second ~data_len-sized buffer for
	// the compressed form (STORED blocks are the same size as their input
	// plus a few bytes of header, so we just interleave the framing with
	// the filtered data slices as we go).
	if (ok)
	{
		Uint8 len_hdr[8];
		put_be32(len_hdr, (Uint32)idat_len);
		memcpy(len_hdr + 4, "IDAT", 4);
		ok = fwrite(len_hdr, 1, 8, f) == 8;

		Uint32 crc = 0xffffffffUL;
		crc = crc_continue(crc, (const Uint8 *)"IDAT", 4);

		// zlib header: CMF=0x78 (deflate, 32K window), FLG=0x01 (FLEVEL=0
		// "fastest", no preset dictionary; 0x7801 is a multiple of 31 as
		// RFC 1950 requires for the FCHECK bits).
		Uint8 zlib_hdr[2] = { 0x78, 0x01 };
		ok = ok && stream_out(f, &crc, zlib_hdr, sizeof(zlib_hdr));

		size_t off = 0;
		for (size_t b = 0; ok && b < num_blocks; ++b)
		{
			size_t block_len = data_len - off;
			if (block_len > max_block)
				block_len = max_block;

			bool bfinal = (b + 1 == num_blocks);
			Uint8 block_hdr[5];
			block_hdr[0] = bfinal ? 1 : 0;  // BFINAL bit, BTYPE=00 (stored)
			put_le16(block_hdr + 1, (Uint16)block_len);
			put_le16(block_hdr + 3, (Uint16)(~(Uint16)block_len));

			ok = ok && stream_out(f, &crc, block_hdr, sizeof(block_hdr));
			ok = ok && stream_out(f, &crc, filtered + off, block_len);

			off += block_len;
		}

		Uint8 adler_bytes[4];
		put_be32(adler_bytes, adler);
		ok = ok && stream_out(f, &crc, adler_bytes, sizeof(adler_bytes));

		if (ok)
		{
			crc ^= 0xffffffffUL;
			Uint8 crc_bytes[4];
			put_be32(crc_bytes, crc);
			ok = fwrite(crc_bytes, 1, 4, f) == 4;
		}
	}

	// IEND: empty data chunk.
	ok = ok && write_small_chunk(f, "IEND", NULL, 0);

	free(filtered);

	if (fclose(f) != 0)
		ok = false;

	if (!ok)
		remove(path);

	return ok;
}
