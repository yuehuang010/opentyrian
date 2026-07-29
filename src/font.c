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
/** @file font.c
 * HD sprite-font text drawing: hue/value-shaded glyph blits with optional
 * shadow/glow variants and alignment.
 *
 * Entry points: drawFontHv(), drawFontHvAligned(), drawFontHvShadow(),
 * drawFontHvBlend(), drawFontDark().
 */

#include "font.h"

#include "fonthand.h"
#include "sprite.h"
#include "video.h"

/**
 * \file font.c
 * \brief Text drawing routines.
 */

/**
 * \brief Draws text in a color specified by hue and value and with a drop
 *        shadow.
 * 
 * A '~' in the text is not drawn but instead toggles highlighting which
 * increases \c value by 4.
 * 
 * \li like JE_dString()                    if (black == false && shadowDist == 2 && hue == 15)
 * \li like JE_textShade() with PART_SHADE  if (black == true && shadowDist == 1)
 * \li like JE_outTextAndDarken()           if (black == false && shadowDist == 1)
 * \li like JE_outTextAdjust() with shadow  if (black == false && shadowDist == 2)
 * 
 * @param surface destination surface
 * @param x initial x-position in pixels; which direction(s) the text is drawn
 *        from this position depends on the alignment
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param hue hue component of text color
 * @param value value component of text color
 * @param black if true the shadow is drawn as solid black, if false the shadow
 *        is drawn by darkening the pixels of the destination surface
 * @param shadowDist distance in pixels that the shadow will be drawn away from
 *        the text. (This is added to both the x and y positions, so a value of
 *        1 causes the shadow to be drawn 1 pixel right and 1 pixel lower than
 *        the text.)
 */
void drawFontHvShadow(SDL_Surface *surface, int x, int y, const char *text, Font font, Uint8 hue, Sint8 value, bool black, int shadowDist)
{
	// Classic: two independent passes (opaque glyph overwrites the shadow beneath).
	// Byte-identical when HD is inactive.
	if (!hd_font_active(surface))
	{
		drawFontDark(surface, x + shadowDist, y + shadowDist, text, font, black);
		drawFontHv(surface, x, y, text, font, hue, value);
		return;
	}

	// HD: composite each glyph over its own drop shadow as ONE quad (no bleed).
	const HDFontMode shadow_mode = black ? HD_FONT_MODE_BLACK : HD_FONT_MODE_DARK;
	bool highlight = false;

	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			highlight = !highlight;
			if (highlight)
				value += 4;
			else
				value -= 4;
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit_shadowed(surface, font, sprite_id, x, y, HD_FONT_MODE_HV, hue, value, shadow_mode, shadowDist, shadowDist, false))
				{
					blit_sprite_dark(surface, x + shadowDist, y + shadowDist, font, sprite_id, black);
					blit_sprite_hv(surface, x, y, font, sprite_id, hue, value);
				}

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

void drawFontHvShadowAligned(SDL_Surface *surface, int x, int y, const char *text, Font font, FontAlignment alignment, Uint8 hue, Sint8 value, bool black, int shadowDist)
{
	switch (alignment)
	{
	case ALIGN_LEFT:
		break;
	case ALIGN_CENTER:
		x -= JE_textWidth(text, font) / 2;
		break;
	case ALIGN_RIGHT:
		x -= JE_textWidth(text, font);
		break;
	}

	drawFontHvShadow(surface, x, y, text, font, hue, value, black, shadowDist);
}

/**
 * \brief Draws text in a color specified by hue and value and with a
 *        surrounding shadow.
 * 
 * A '~' in the text is not drawn but instead toggles highlighting which
 * increases \c value by 4.
 * 
 * \li like JE_textShade() with FULL_SHADE  if (black == true && shadowDist == 1)
 * 
 * @param surface destination surface
 * @param x initial x-position in pixels; which direction(s) the text is drawn
 *        from this position depends on the alignment
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param hue hue component of text color
 * @param value value component of text color
 * @param black if true the shadow is drawn as solid black, if false the shadow
 *        is drawn by darkening the pixels of the destination surface
 * @param shadowDist distance in pixels that the shadows will be drawn away
 *        from the text. (This distance is separately added to and subtracted
 *        from the x position and y position, resulting in four shadows -- one
 *        in each cardinal direction.  If this shadow distance is small enough,
 *        this produces a shadow that outlines the text.)
 */
void drawFontHvFullShadow(SDL_Surface *surface, int x, int y, const char *text, Font font, Uint8 hue, Sint8 value, bool black, int shadowDist)
{
	// Classic: four independent black passes + the opaque glyph. Byte-identical
	// when HD is inactive.
	if (!hd_font_active(surface))
	{
		drawFontDark(surface, x,              y - shadowDist, text, font, black);
		drawFontDark(surface, x + shadowDist, y,              text, font, black);
		drawFontDark(surface, x,              y + shadowDist, text, font, black);
		drawFontDark(surface, x - shadowDist, y,              text, font, black);

		drawFontHv(surface, x, y, text, font, hue, value);
		return;
	}

	// HD: composite each glyph over its own 4-way outline as ONE quad (no bleed).
	const HDFontMode shadow_mode = black ? HD_FONT_MODE_BLACK : HD_FONT_MODE_DARK;
	bool highlight = false;

	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			highlight = !highlight;
			if (highlight)
				value += 4;
			else
				value -= 4;
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit_shadowed(surface, font, sprite_id, x, y, HD_FONT_MODE_HV, hue, value, shadow_mode, shadowDist, shadowDist, true))
				{
					blit_sprite_dark(surface, x,              y - shadowDist, font, sprite_id, black);
					blit_sprite_dark(surface, x + shadowDist, y,              font, sprite_id, black);
					blit_sprite_dark(surface, x,              y + shadowDist, font, sprite_id, black);
					blit_sprite_dark(surface, x - shadowDist, y,              font, sprite_id, black);
					blit_sprite_hv(surface, x, y, font, sprite_id, hue, value);
				}

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

/**
 * \brief Draws text with a recolored outline at the four diagonal corners.
 *
 * The classic form is four \c drawFontHv() passes at (±dist, ±dist) shaded with
 * \p outlineValue, then the glyph itself -- the title menu's own text style (see
 * titleScreen() in src/tyrian2.c).
 *
 * A '~' in the text is not drawn but instead toggles highlighting which
 * increases \c value by 4.
 *
 * @param surface destination surface
 * @param x initial x-position in pixels
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param hue hue component of both the text and outline color
 * @param value value component of the text color
 * @param outlineValue value component of the outline color
 * @param dist distance in pixels of each outline copy from the text
 */
void drawFontHvOutline(SDL_Surface *surface, int x, int y, const char *text, Font font, Uint8 hue, Sint8 value, Sint8 outlineValue, int dist)
{
	// Classic: four independent recolored passes + the opaque glyph. Byte-identical
	// when HD is inactive.
	if (!hd_font_active(surface))
	{
		drawFontHv(surface, x - dist, y - dist, text, font, hue, outlineValue);
		drawFontHv(surface, x + dist, y + dist, text, font, hue, outlineValue);
		drawFontHv(surface, x + dist, y - dist, text, font, hue, outlineValue);
		drawFontHv(surface, x - dist, y + dist, text, font, hue, outlineValue);
		drawFontHv(surface, x, y, text, font, hue, value);
		return;
	}

	// HD: composite each glyph over its own outline as ONE quad. Drawn as five
	// separate anti-aliased quads instead, the four outline copies accumulate alpha
	// and swallow the 1px background channels the classic passes leave open,
	// stranding a hairline sliver in the middle of the outline.
	bool highlight = false;

	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			highlight = !highlight;
			if (highlight)
				value += 4;
			else
				value -= 4;
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit_outlined(surface, font, sprite_id, x, y, hue, value, outlineValue, dist))
				{
					blit_sprite_hv(surface, x - dist, y - dist, font, sprite_id, hue, outlineValue);
					blit_sprite_hv(surface, x + dist, y + dist, font, sprite_id, hue, outlineValue);
					blit_sprite_hv(surface, x + dist, y - dist, font, sprite_id, hue, outlineValue);
					blit_sprite_hv(surface, x - dist, y + dist, font, sprite_id, hue, outlineValue);
					blit_sprite_hv(surface, x, y, font, sprite_id, hue, value);
				}

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

void drawFontHvFullShadowAligned(SDL_Surface *surface, int x, int y, const char *text, Font font, FontAlignment alignment, Uint8 hue, Sint8 value, bool black, int shadowDist)
{
	switch (alignment)
	{
	case ALIGN_LEFT:
		break;
	case ALIGN_CENTER:
		x -= JE_textWidth(text, font) / 2;
		break;
	case ALIGN_RIGHT:
		x -= JE_textWidth(text, font);
		break;
	}

	drawFontHvFullShadow(surface, x, y, text, font, hue, value, black, shadowDist);
}

/**
 * \brief Draws text in a color specified by hue and value.
 * 
 * A '~' in the text is not drawn but instead toggles highlighting which
 * increases \c value by 4.
 * 
 * \li like JE_outText() with (brightness >= 0)
 * \li like JE_outTextAdjust() without shadow
 * 
 * @param surface destination surface
 * @param x initial x-position in pixels; which direction(s) the text is drawn
 *        from this position depends on the alignment
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param hue hue component of text color
 * @param value value component of text color
 */
void drawFontHv(SDL_Surface *surface, int x, int y, const char *text, Font font, Uint8 hue, Sint8 value)
{
	bool highlight = false;

	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			highlight = !highlight;
			if (highlight)
				value += 4;
			else
				value -= 4;
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit(surface, font, sprite_id, x, y, HD_FONT_MODE_HV, hue, value))
					blit_sprite_hv(surface, x, y, font, sprite_id, hue, value);

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

void drawFontHvAligned(SDL_Surface *surface, int x, int y, const char *text, Font font, FontAlignment alignment, Uint8 hue, Sint8 value)
{
	switch (alignment)
	{
	case ALIGN_LEFT:
		break;
	case ALIGN_CENTER:
		x -= JE_textWidth(text, font) / 2;
		break;
	case ALIGN_RIGHT:
		x -= JE_textWidth(text, font);
		break;
	}

	drawFontHv(surface, x, y, text, font, hue, value);
}

/**
 * \brief Draws blended text in a color specified by hue and value.
 * 
 * Corresponds to blit_sprite_hv_blend()
 * 
 * \li like JE_outTextModify()
 * 
 * @param surface destination surface
 * @param x initial x-position in pixels; which direction(s) the text is drawn
 *        from this position depends on the alignment
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param hue hue component of text color
 * @param value value component of text color
 */
void drawFontHvBlend(SDL_Surface *surface, int x, int y, const char *text, Font font, Uint8 hue, Sint8 value)
{
	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit(surface, font, sprite_id, x, y, HD_FONT_MODE_HV_BLEND, hue, value))
					blit_sprite_hv_blend(surface, x, y, font, sprite_id, hue, value);

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

void drawFontHvBlendAligned(SDL_Surface *surface, int x, int y, const char *text, Font font, FontAlignment alignment, Uint8 hue, Sint8 value)
{
	switch (alignment)
	{
	case ALIGN_LEFT:
		break;
	case ALIGN_CENTER:
		x -= JE_textWidth(text, font) / 2;
		break;
	case ALIGN_RIGHT:
		x -= JE_textWidth(text, font);
		break;
	}

	drawFontHvBlend(surface, x, y, text, font, hue, value);
}

/**
 * \brief Draws darkened text.
 * 
 * Corresponds to blit_sprite_dark()
 * 
 * \li like JE_outText() with (brightness < 0)  if (black == true)
 * 
 * @param surface destination surface
 * @param x initial x-position in pixels; which direction(s) the text is drawn
 *        from this position depends on the alignment
 * @param y initial upper y-position in pixels
 * @param text text to be drawn
 * @param font style/size of text
 * @param black if true text is drawn as solid black, if false text is drawn by
 *        darkening the pixels of the destination surface
 */
void drawFontDark(SDL_Surface *surface, int x, int y, const char *text, Font font, bool black)
{
	for (; *text != '\0'; ++text)
	{
		int sprite_id = fontMap[(unsigned char)*text];

		switch (*text)
		{
		case ' ':
			x += 6;
			break;

		case '~':
			break;

		default:
			if (sprite_id != -1 && sprite_exists(font, sprite_id))
			{
				if (!hd_font_emit(surface, font, sprite_id, x, y, black ? HD_FONT_MODE_BLACK : HD_FONT_MODE_DARK, 0, 0))
					blit_sprite_dark(surface, x, y, font, sprite_id, black);

				x += sprite(font, sprite_id)->width + 1;
			}
			break;
		}
	}
}

void drawFontDarkAligned(SDL_Surface *surface, int x, int y, const char *text, Font font, FontAlignment alignment, bool black)
{
	switch (alignment)
	{
	case ALIGN_LEFT:
		break;
	case ALIGN_CENTER:
		x -= JE_textWidth(text, font) / 2;
		break;
	case ALIGN_RIGHT:
		x -= JE_textWidth(text, font);
		break;
	}

	drawFontDark(surface, x, y, text, font, black);
}
