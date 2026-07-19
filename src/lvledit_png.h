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
/** @file lvledit_png.h
 * Minimal self-contained 24-bit RGB PNG writer, no library dependencies
 * beyond SDL2 (already a build requirement). Used by the level editor's map
 * export (lvledit.c) to write full-level overview images -- deliberately
 * uncompressed (STORED deflate blocks) since this is a one-shot export tool,
 * not a hot path; file size is not a concern.
 */
#ifndef LVLEDIT_PNG_H
#define LVLEDIT_PNG_H

#include "opentyr.h"

#include <stdbool.h>

// Writes a truecolor (8-bit-per-channel RGB, no alpha) PNG to `path`.
// `rgb` is row-major, top row first, 3 bytes/pixel, no row padding
// (w*h*3 bytes total). Returns false on any I/O or size failure.
bool lvledit_png_write(const char *path, const Uint8 *rgb, int w, int h);

#endif /* LVLEDIT_PNG_H */
