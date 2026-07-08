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
#include "pcxload.h"

#include "file.h"
#include "opentyr.h"
#include "palette.h"
#include "video.h"

#include <string.h>

// HD note: an HD backdrop asset for this file already exists
// (tyrian21/hdpcx_tshp2.dat, extracted by tools/hd_extract.py's
// STANDALONE_PCX table) and the engine has the same hd_set_backdrop_asset()
// / hd_clear_backdrop() pattern already wired for every other full-screen
// picture (see e.g. JE_loadPic() call sites via `hd_mode && hd_set_backdrop(N)`
// in src/mainint.c, src/menus.c, src/tyrian2.c). It is deliberately NOT wired
// here: JE_loadPCX()'s only call site is the 'P0' flow-script command inside
// src/tyrian2.c JE_loadMap() (the level/interlude script interpreter), which
// is out of scope for this change -- it's owned by concurrent HD-tileset
// compositor work on the same function. Wiring tshp2 safely also needs an
// enter/exit audit of JE_loadMap's many picture-changing branches ('P', 'U',
// scene transitions, early-exit paths) to make sure hd_clear_backdrop() is
// called on every path that leaves the picture showing -- that survey wasn't
// done here to avoid touching JE_loadMap. Ships classic until a follow-up
// phase revisits JE_loadMap once it's not being edited concurrently.
void JE_loadPCX(const char *file) // this is only meant to load tshp2.pcx
{
	Uint8 *s = VGAScreen->pixels; /* 8-bit specific */
	
	FILE *f = dir_fopen_die(data_dir(), file, "rb");
	
	fseek(f, -769, SEEK_END);

	Uint8 temp;
	fread_u8_die(&temp, 1, f);
	if (temp == 12)
	{
		for (int i = 0; i < 256; i++)
		{
			Uint8 rgb[3];
			fread_u8_die(rgb, 3, f);
			colors[i].r = rgb[0];
			colors[i].g = rgb[1];
			colors[i].b = rgb[2];
		}
	}
	
	fseek(f, 128, SEEK_SET);
	
	for (int i = 0; i < 320 * 200; )
	{
		Uint8 p;
		fread_u8_die(&p, 1, f);
		if ((p & 0xc0) == 0xc0)
		{
			i += (p & 0x3f);
			fread_u8_die(&temp, 1, f);
			memset(s, temp, (p & 0x3f));
			s += (p & 0x3f);
		}
		else
		{
			i++;
			*s = p;
			s++;
		}
		if (i && (i % 320 == 0))
		{
			s += VGAScreen->pitch - 320;
		}
	}
	
	fclose(f);
}
