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
#ifndef BUNDLE_H
#define BUNDLE_H

#include <stdbool.h>
#include <stdio.h>

// Standalone asset bundle (Phase S0 of internal/plan/STANDALONE_PLAN.md).
//
// `tyrian.base` is a single archive built by tools/mkbundle.py. When a loose
// file is not found on disk, `dir_fopen` in file.c falls back to the bundle so
// a checkout with no data dir can still boot. Loose files always win, so
// `--data` and drop-in modding are unaffected.

// True once the bundle has been located and its index parsed. Lazily loaded on
// first lookup; safe to call repeatedly.
bool bundle_available(void);

// Is `name` (matched case-insensitively against the bare filename) in the
// bundle?
bool bundle_has(const char *name);

// Open a bundled file for reading as a FILE*, or NULL if absent. The returned
// stream is read-only and backed by memory; the caller closes it with fclose()
// as usual.
FILE *bundle_fopen(const char *name);

#endif // BUNDLE_H
