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
#ifndef HD_HUD_H
#define HD_HUD_H

#include "SDL.h"

/**
 * HD in-flight HUD overlay (REMASTER_HUD.md phase H2.6). Draws the sidebar
 * (VGA x in [264,320), y in [0,200)) and bottom bar (VGA y in [184,200),
 * x in [0,264)) strips from a once-per-level-palette hq4x upscale of a clean
 * PIC #3 panel (the original game art, crisp instead of blurry), plus
 * vectorized shield/armor/power bars, weapon-power dots, and sidekick ammo
 * gauges on top, at output resolution.
 *
 * Called from the hd_mode && hd_flight_active branch of scale_and_flip()
 * (src/video.c), after the flight-sprite clip rect is released and before
 * draw_hd_font_queue() (so the labels queued here render this same present).
 * `dst_rect` is the same output rect the flight-sprite queue scales against
 * (src/video.c's classic_scale_base() output).
 *
 * Gated internally to 1P only; 2P falls through, leaving the classic 8-bit
 * panel (already present in the base image) showing through. Elements not yet
 * vectorized (sidekick icons, rear-config buttons, the message-bar interior)
 * stay as punch-outs: the procedural fills skip them so the classic pixels show.
 */
void hd_hud_draw(SDL_Renderer *renderer, const SDL_Rect *dst_rect);

/**
 * Message-bar text shadow hooks (REMASTER_HUD.md phase H3).
 *
 * The bottom message window is the last classic in-flight text. Rather than
 * intercept the draws, hd_hud keeps a small "shadow" of the message glyphs
 * populated by these tiny hooks at the classic draw sites; hd_hud_draw() then
 * re-emits them as crisp HD glyphs over the baked window art every present
 * (the message-bar punch-out is dropped, so the baked art shows underneath).
 *
 * ALL of these are no-ops unless the HD HUD is the live in-flight producer
 * (hd_mode && hd_flight_active, targeting VGAScreenSeg): the classic draws
 * underneath are never altered, and non-flight text screens that also use
 * JE_outCharGlow stay fully classic. Safe to call unconditionally from the
 * classic paths.
 */

// From JE_drawTextWindow: capture the whole static message string laid out
// exactly as JE_outText(screen, x, y, s, hue, value) would (space + '~'
// brightness handling included), replacing any prior shadow.
void hd_hud_msg_text(int x, int y, const char *s, Uint8 hue, Sint8 value);

// From JE_outCharGlow, before its per-char animation loop: start a fresh
// (empty) glow capture so stale glyphs from an earlier message can't linger.
void hd_hud_msg_glow_begin(void);

// From JE_outCharGlow's inner blit_sprite_hv: capture/refresh one glyph at
// char index `slot` with its live animated glow `value` (and `hue` = bank).
void hd_hud_msg_glow_char(int slot, int x, int y, unsigned int sprite_id, Uint8 hue, Sint8 value);

// From the message-erase sites (JE_drawTextWindow's erase blit and the
// textErase countdown erase in tyrian2.c): drop the shadow so the baked
// empty-window art shows with no stale glyphs.
void hd_hud_msg_clear(void);

#endif /* HD_HUD_H */
