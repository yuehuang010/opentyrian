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
/** @file opentyr.c
 * Program entry point: subsystem init and the top-level title/game/destruct loop.
 *
 * Entry points: main() — the program entry point; setupMenu().
 */

#include "opentyr.h"

#include "config.h"
#include "controller.h"
#include "destruct.h"
#include "editship.h"
#include "episodes.h"
#include "file.h"
#include "font.h"
#include "fonthand.h"
#include "helptext.h"
#include "interp.h"
#include "jukebox.h"
#include "keyboard.h"
#include "loudness.h"
#include "lvledit.h"
#include "lvledit_io.h"
#include "mainint.h"
#include "mouse.h"
#include "mtrand.h"
#include "network.h"
#include "nortsong.h"
#include "nortvars.h"
#include "opentyrian_version.h"
#include "palette.h"
#include "params.h"
#include "picload.h"
#include "sprite.h"
#include "tyrian2.h"
#include "varz.h"
#include "vga256d.h"
#include "video.h"
#include "video_scale.h"
#include "xmas.h"

#include "SDL.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *opentyrian_str = "OpenTyrian";
const char *opentyrian_version = OPENTYRIAN_VERSION;

static size_t getDisplayPickerItemsCount(void)
{
	return 1 + (size_t)SDL_GetNumVideoDisplays();
}

static const char *getDisplayPickerItem(size_t i, char *buffer, size_t bufferSize)
{
	if (i == 0)
		return "Window";

	snprintf(buffer, bufferSize, "Display %d", (int)i);
	return buffer;
}

static size_t getScalerPickerItemsCount(void)
{
	return (size_t)scalers_count;
}

static const char *getScalerPickerItem(size_t i, char *buffer, size_t bufferSize)
{
	(void)buffer, (void)bufferSize;

	return scalers[i].name;
}

static size_t getScalingModePickerItemsCount(void)
{
	return (size_t)ScalingMode_MAX;
}

static const char *getScalingModePickerItem(size_t i, char *buffer, size_t bufferSize)
{
	(void)buffer, (void)bufferSize;

	return scaling_mode_names[i];
}

// Generic Off/On picker (index 0 = Off, 1 = On) for boolean toggles.
static size_t getOnOffPickerItemsCount(void)
{
	return 2;
}

static const char *getOnOffPickerItem(size_t i, char *buffer, size_t bufferSize)
{
	(void)buffer, (void)bufferSize;

	return i == 0 ? "Off" : "On";
}

void setupMenu(void)
{
	typedef enum
	{
		MENU_ITEM_NONE = 0,
		MENU_ITEM_DONE,
		MENU_ITEM_GRAPHICS,
		MENU_ITEM_SOUND,
		MENU_ITEM_JUKEBOX,
		MENU_ITEM_DESTRUCT,
		MENU_ITEM_DISPLAY,
		MENU_ITEM_SCALER,
		MENU_ITEM_SCALING_MODE,
		MENU_ITEM_MUSIC_VOLUME,
		MENU_ITEM_SOUND_VOLUME,
		MENU_ITEM_HD_MUSIC,
		MENU_ITEM_HD_SFX,
		MENU_ITEM_HD_GRAPHICS,
		MENU_ITEM_HIGHFPS,
	} MenuItemId;

	typedef enum
	{
		MENU_NONE = 0,
		MENU_SETUP,
		MENU_GRAPHICS,
		MENU_SOUND,
	} MenuId;

	typedef struct
	{
		MenuItemId id;
		const char *name;
		const char *description;
		size_t (*getPickerItemsCount)(void);
		const char *(*getPickerItem)(size_t i, char *buffer, size_t bufferSize);
	} MenuItem;

	typedef struct
	{
		const char *header;
		const MenuItem items[7];
	} Menu;

	static const Menu menus[] = {
		[MENU_SETUP] = {
			.header = "Setup",
			.items = {
				{ MENU_ITEM_GRAPHICS, "Graphics...", "Change the graphics settings." },
				{ MENU_ITEM_SOUND, "Sound...", "Change the sound settings." },
				{ MENU_ITEM_JUKEBOX, "Jukebox", "Listen to the music of Tyrian." },
				// { MENU_ITEM_DESTRUCT, "Destruct", "Play a bonus mini-game." },
				{ MENU_ITEM_DONE, "Done", "Return to the main menu." },
				{ -1 }
			},
		},
		[MENU_GRAPHICS] = {
			.header = "Graphics",
			.items = {
				{ MENU_ITEM_DISPLAY, "Display:", "Change the display mode.", getDisplayPickerItemsCount, getDisplayPickerItem },
				{ MENU_ITEM_SCALER, "Scaler:", "Change the pixel art scaling algorithm.", getScalerPickerItemsCount, getScalerPickerItem },
				{ MENU_ITEM_SCALING_MODE, "Scaling Mode:", "Change the scaling mode.", getScalingModePickerItemsCount, getScalingModePickerItem },
				{ MENU_ITEM_HD_GRAPHICS, "HD Graphics:", "All HD remaster graphics (Off = classic pixels).", getOnOffPickerItemsCount, getOnOffPickerItem },
				{ MENU_ITEM_HIGHFPS, "Smooth FPS:", "Interpolate extra frames for smoother motion (Off = classic).", getOnOffPickerItemsCount, getOnOffPickerItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
		[MENU_SOUND] = {
			.header = "Sound",
			.items = {
				{ MENU_ITEM_MUSIC_VOLUME, "Music Volume", "Change volume with the left/right arrow keys." },
				{ MENU_ITEM_SOUND_VOLUME, "Sound Volume", "Change volume with the left/right arrow keys." },
				{ MENU_ITEM_HD_MUSIC, "HD Music:", "Stream the remastered music (Off = classic OPL synth).", getOnOffPickerItemsCount, getOnOffPickerItem },
				{ MENU_ITEM_HD_SFX, "HD Sound:", "Use the remastered sound effects (Off = classic).", getOnOffPickerItemsCount, getOnOffPickerItem },
				{ MENU_ITEM_DONE, "Done", "Return to the previous menu." },
				{ -1 }
			},
		},
	};

	char buffer[100];

	if (shopSpriteSheet.data == NULL)
		JE_loadCompShapes(&shopSpriteSheet, '1');  // need mouse pointer sprites

	bool restart = true;

	MenuId menuParents[COUNTOF(menus)] = { MENU_NONE };
	size_t selectedMenuItemIndexes[COUNTOF(menus)] = { 0 };
	MenuId currentMenu = MENU_SETUP;
	MenuItemId currentPicker = MENU_ITEM_NONE;
	size_t pickerSelectedIndex = 0;

	const int xCenter = 320 / 2;
	const int yMenuHeader = 4;
	const int xMenuItem = 45;
	const int xMenuItemName = xMenuItem;
	const int wMenuItemName = 135;
	const int xMenuItemValue = xMenuItemName + wMenuItemName;
	const int wMenuItemValue = 95;
	const int wMenuItem = wMenuItemName + wMenuItemValue;
	const int yMenuItems = 37;
	const int dyMenuItems = 21;
	const int hMenuItem = 13;

	for (; ; )
	{
		setFrameCount(1);

		if (restart)
		{
			JE_loadPic(VGAScreen2, 2, false);

			if (hd_mode && hd_set_backdrop(2))
				JE_clr256(VGAScreen2);

			fill_rectangle_wh(VGAScreen2, 0, 192, 320, 8, 0);

			// HD: fade in the plain backdrop here, before any text is drawn.
			// The post-draw fade below (classic-only) would drain the HD glyph
			// queue and leave the held frame textless.
			if (hd_mode)
			{
				memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);
				fade_palette(colors, 10, 0, 255);
			}
		}

		// Restore background.
		memcpy(VGAScreen->pixels, VGAScreen2->pixels, (size_t)VGAScreen->pitch * VGAScreen->h);

		const Menu *menu = &menus[currentMenu];

		// Draw header.
		drawFontHvShadowAligned(VGAScreen, xCenter, yMenuHeader, menu->header, FONT_LARGE, ALIGN_CENTER, 15, -3, false, 2);

		int yPicker = 0;
		const int dyPickerItem = 15;
		const int dyPickerItemPadding = 2;
		const int hPickerItem = dyPickerItem - dyPickerItemPadding;

		size_t *const selectedMenuItemIndex = &selectedMenuItemIndexes[currentMenu];
		const MenuItem *const menuItems = menu->items;

		// Compute the picker box geometry up front (it depends only on the selected
		// item and its picker length) so both the box-drawing code below and the
		// menu-item loop can consult it. The box anchors at the selected row, clamped
		// so it never runs past the status line at the screen bottom.
		size_t pickerItemsCount = 0;
		int hPicker = 0;
		int pickerBoxTop = 0, pickerBoxBottom = -1;
		yPicker = yMenuItems + dyMenuItems * (int)(*selectedMenuItemIndex);
		if (currentPicker != MENU_ITEM_NONE)
		{
			pickerItemsCount = menuItems[*selectedMenuItemIndex].getPickerItemsCount();
			hPicker = dyPickerItem * (int)pickerItemsCount - dyPickerItemPadding;
			yPicker = MIN(yPicker, 200 - 10 - (hPicker + 5 + 2));
			// Outer extent of the drawn box, including its 5px border rings (see the
			// JE_rectangle calls below at yPicker-5 .. yPicker+hPicker+5). A tall
			// picker gets clamped upward and its top border overlaps the row above,
			// so the suppression band must cover the whole border, not just the fill.
			pickerBoxTop = yPicker - 5;
			pickerBoxBottom = yPicker + hPicker + 5;
		}

		// Draw menu items.

		size_t menuItemsCount = 0;
		for (size_t i = 0; menuItems[i].id != (MenuItemId)-1; ++i)
		{
			menuItemsCount += 1;

			const MenuItem *const menuItem = &menuItems[i];

			const int y = yMenuItems + dyMenuItems * i;

			const bool selected = i == *selectedMenuItemIndex;
			const bool disabled = currentPicker != MENU_ITEM_NONE && !selected;

			// In HD, the opaque picker box lives in the indexed-overlay layer, below
			// the topmost HD-font glyphs, so a value-column glyph the box covers would
			// bleed on top of it. Classic draws the box into the same surface last, so
			// it already occludes these values. Suppress any value row whose glyph span
			// [y, y+hMenuItem] overlaps the box's outer extent so a row the box only
			// partially clips (e.g. "Window" under a tall Scaler picker) is hidden too.
			const bool valueHiddenByPicker = hd_mode && currentPicker != MENU_ITEM_NONE
				&& y + hMenuItem > pickerBoxTop && y < pickerBoxBottom;

			const char *const name = menuItem->name;

			drawFontHvShadow(VGAScreen, xMenuItemName, y, name, FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);

			switch (menuItem->id)
			{
			case MENU_ITEM_DISPLAY:;
				const char *value = "Window";
				if (fullscreen_display >= 0)
				{
					snprintf(buffer, sizeof(buffer), "Display %d", fullscreen_display + 1);
					value = buffer;
				}

				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, value, FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SCALER:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, scalers[scaler].name, FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_SCALING_MODE:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, scaling_mode_names[scaling_mode], FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_MUSIC_VOLUME:
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, music_disabled ? 170 : 174, (tyrMusicVolume + 4) / 8, 2, 10);
				JE_rectangle(VGAScreen, xMenuItemValue - 2, y - 2, xMenuItemValue + 96, y + 11, 242);
				break;

			case MENU_ITEM_SOUND_VOLUME:
				JE_barDrawShadow(VGAScreen, xMenuItemValue, y, 1, samples_disabled ? 170 : 174, (fxVolume + 4) / 8, 2, 10);
				JE_rectangle(VGAScreen, xMenuItemValue - 2, y - 2, xMenuItemValue + 96, y + 11, 242);
				break;

			case MENU_ITEM_HD_MUSIC:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, hd_music ? "On" : "Off", FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_HD_SFX:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, hd_sfx ? "On" : "Off", FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_HD_GRAPHICS:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, hd_mode ? "On" : "Off", FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			case MENU_ITEM_HIGHFPS:
				if (!valueHiddenByPicker)
					drawFontHvShadow(VGAScreen, xMenuItemValue, y, highfps_mode ? "On" : "Off", FONT_NORMAL, 15, -3 + (selected ? 2 : 0) + (disabled ? -4 : 0), false, 2);
				break;

			default:
				break;
			}
		}

		// Draw status text.
		JE_textShade(VGAScreen, xMenuItemName, 190, menuItems[*selectedMenuItemIndex].description, 15, 4, PART_SHADE);

		// Draw picker box and items.

		if (currentPicker != MENU_ITEM_NONE)
		{
			const MenuItem *selectedMenuItem = &menuItems[*selectedMenuItemIndex];

			JE_rectangle(VGAScreen, xMenuItemValue - 5, yPicker- 3, xMenuItemValue + wMenuItemValue + 5 - 1, yPicker + hPicker + 3 - 1, 248);
			JE_rectangle(VGAScreen, xMenuItemValue - 4, yPicker- 4, xMenuItemValue + wMenuItemValue + 4 - 1, yPicker + hPicker + 4 - 1, 250);
			JE_rectangle(VGAScreen, xMenuItemValue - 3, yPicker- 5, xMenuItemValue + wMenuItemValue + 3 - 1, yPicker + hPicker + 5 - 1, 248);
			fill_rectangle_wh(VGAScreen, xMenuItemValue - 2, yPicker - 2, wMenuItemValue + 2 + 2, hPicker + 2 + 2, 224);

			for (size_t i = 0; i < pickerItemsCount; ++i)
			{
				const int y = yPicker + dyPickerItem * (int)i;

				const bool selected = i == pickerSelectedIndex;

				const char *value = selectedMenuItem->getPickerItem(i, buffer, sizeof buffer);

				drawFontHvShadow(VGAScreen, xMenuItemValue, y, value, FONT_NORMAL, 15, -3 + (selected ? 2 : 0), false, 2);
			}
		}

		if (restart)
		{
			mouseCursor = MOUSE_POINTER_NORMAL;

			if (!hd_mode)
				fade_palette(colors, 10, 0, 255);

			restart = false;
		}

		JE_mouseStart();
		JE_showVGA();
		JE_mouseReplace();

		int oldFullscreenDisplay = fullscreen_display;
		while (true)
		{
			waitUntilElapsed();

			// If full-screen is toggled via keyboard shortcut then display
			// setting needs to be updated.
			if (fullscreen_display != oldFullscreenDisplay)
				break;

			if (hasInput(INPUT_ANY))
				break;

			setFrameCount(1);
		}

		if (currentPicker == MENU_ITEM_NONE)
		{
			// Handle menu item interaction.

			bool action = false;

			MouseInput mouseInput;
			KeyboardInput keyboardInput;

			if (mouseGetInput(INPUT_ANY, &mouseInput))
			{
				// Find menu item name or value that was hovered or clicked.
				if (mouseInput.x >= xMenuItem && mouseInput.x < xMenuItem + wMenuItem)
				{
					for (size_t i = 0; i < menuItemsCount; ++i)
					{
						const int yMenuItem = yMenuItems + dyMenuItems * i;
						if (mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
						{
							if (*selectedMenuItemIndex != i)
							{
								JE_playSampleNum(S_CURSOR);

								*selectedMenuItemIndex = i;
							}

							if (mouseInput.button == SDL_BUTTON_LEFT &&
							    mouseInput.y >= yMenuItem && mouseInput.y < yMenuItem + hMenuItem)
							{
								// Act on menu item via name.
								if (mouseInput.x >= xMenuItemName && mouseInput.x < xMenuItemName + wMenuItemName)
								{
									action = true;
								}

								// Act on menu item via value.
								else if (mouseInput.x >= xMenuItemValue && mouseInput.x < xMenuItemValue + wMenuItemValue)
								{
									switch (menuItems[*selectedMenuItemIndex].id)
									{
									case MENU_ITEM_DISPLAY:
									case MENU_ITEM_SCALER:
									case MENU_ITEM_SCALING_MODE:
									{
										action = true;
										break;
									}
									case MENU_ITEM_MUSIC_VOLUME:
									{
										JE_playSampleNum(S_CURSOR);

										int value = (mouseInput.x - xMenuItemValue) * 255 / (wMenuItemValue - 1);
										tyrMusicVolume = MIN(MAX(0, value), 255);

										set_volume(tyrMusicVolume, fxVolume);
										break;
									}
									case MENU_ITEM_SOUND_VOLUME:
									{
										int value = (mouseInput.x - xMenuItemValue) * 255 / (wMenuItemValue - 1);
										fxVolume = MIN(MAX(0, value), 255);

										set_volume(tyrMusicVolume, fxVolume);

										JE_playSampleNum(S_CURSOR);
										break;
									}
									default:
										break;
									}
								}
							}

							break;
						}
					}
				}

				if (mouseInput.button == SDL_BUTTON_RIGHT)
				{
					JE_playSampleNum(S_SPRING);

					currentMenu = menuParents[currentMenu];
				}
			}
			else if (keyboardGetInput(&keyboardInput))
			{
				switch (keyboardInput.scancode)
				{
				case SDL_SCANCODE_UP:
				{
					JE_playSampleNum(S_CURSOR);

					*selectedMenuItemIndex = *selectedMenuItemIndex == 0
						? menuItemsCount - 1
						: *selectedMenuItemIndex - 1;
					break;
				}
				case SDL_SCANCODE_DOWN:
				{
					JE_playSampleNum(S_CURSOR);

					*selectedMenuItemIndex = *selectedMenuItemIndex == menuItemsCount - 1
						? 0
						: *selectedMenuItemIndex + 1;
					break;
				}
				case SDL_SCANCODE_LEFT:
				{
					switch (menuItems[*selectedMenuItemIndex].id)
					{
					case MENU_ITEM_MUSIC_VOLUME:
					{
						JE_playSampleNum(S_CURSOR);

						JE_changeVolume(&tyrMusicVolume, -8, &fxVolume, 0);
						break;
					}
					case MENU_ITEM_SOUND_VOLUME:
					{
						JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, -8);

						JE_playSampleNum(S_CURSOR);
						break;
					}
					default:
						break;
					}
					break;
				}
				case SDL_SCANCODE_RIGHT:
				{
					switch (menuItems[*selectedMenuItemIndex].id)
					{
					case MENU_ITEM_MUSIC_VOLUME:
					{
						JE_playSampleNum(S_CURSOR);

						JE_changeVolume(&tyrMusicVolume, 8, &fxVolume, 0);
						break;
					}
					case MENU_ITEM_SOUND_VOLUME:
					{
						JE_changeVolume(&tyrMusicVolume, 0, &fxVolume, 8);

						JE_playSampleNum(S_CURSOR);
						break;
					}
					default:
						break;
					}
					break;
				}
				case SDL_SCANCODE_SPACE:
				case SDL_SCANCODE_RETURN:
				{
					action = true;
					break;
				}
				case SDL_SCANCODE_ESCAPE:
				{
					JE_playSampleNum(S_SPRING);

					currentMenu = menuParents[currentMenu];
					break;
				}
				default:
					break;
				}
			}

			if (action)
			{
				const MenuItemId selectedMenuItemId = menuItems[*selectedMenuItemIndex].id;

				switch (selectedMenuItemId)
				{
				case MENU_ITEM_DONE:
				{
					JE_playSampleNum(S_SELECT);

					currentMenu = menuParents[currentMenu];
					break;
				}
				case MENU_ITEM_GRAPHICS:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_GRAPHICS] = currentMenu;
					currentMenu = MENU_GRAPHICS;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_SOUND:
				{
					JE_playSampleNum(S_SELECT);

					menuParents[MENU_SOUND] = currentMenu;
					currentMenu = MENU_SOUND;
					selectedMenuItemIndexes[currentMenu] = 0;
					break;
				}
				case MENU_ITEM_JUKEBOX:
				{
					JE_playSampleNum(S_SELECT);

					fade_black(10);
					hd_clear_backdrop();  // don't leak the setup HD backdrop into the jukebox

					jukebox();

					restart = true;
					break;
				}
				case MENU_ITEM_DESTRUCT:
				{
					JE_playSampleNum(S_SELECT);

					fade_black(10);
					hd_clear_backdrop();  // don't leak the setup HD backdrop into the destruct minigame

					JE_destructGame();

					restart = true;
					break;
				}
				case MENU_ITEM_DISPLAY:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = (size_t)(fullscreen_display + 1);
					break;
				}
				case MENU_ITEM_SCALER:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = scaler;
					break;
				}
				case MENU_ITEM_SCALING_MODE:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = scaling_mode;
					break;
				}
				case MENU_ITEM_HD_MUSIC:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = hd_music ? 1 : 0;
					break;
				}
				case MENU_ITEM_HD_SFX:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = hd_sfx ? 1 : 0;
					break;
				}
				case MENU_ITEM_HD_GRAPHICS:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = hd_mode ? 1 : 0;
					break;
				}
				case MENU_ITEM_HIGHFPS:
				{
					JE_playSampleNum(S_CLICK);

					currentPicker = selectedMenuItemId;
					pickerSelectedIndex = highfps_mode ? 1 : 0;
					break;
				}
				case MENU_ITEM_MUSIC_VOLUME:
				{
					JE_playSampleNum(S_CLICK);

					music_disabled = !music_disabled;
					if (!music_disabled)
						restart_song();
					break;
				}
				case MENU_ITEM_SOUND_VOLUME:
				{
					samples_disabled = !samples_disabled;

					JE_playSampleNum(S_CLICK);
					break;
				}
				default:
					break;
				}
			}

			if (currentMenu == MENU_NONE)
			{
				fade_black(10);

				hd_clear_backdrop();
				return;
			}
		}
		else
		{
			const MenuItem *selectedMenuItem = &menuItems[*selectedMenuItemIndex];

			// Handle picker interaction.

			bool action = false;

			MouseInput mouseInput;
			KeyboardInput keyboardInput;

			if (mouseGetInput(INPUT_ANY, &mouseInput))
			{
				const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

				// Find picker item that was hovered or clicked.
				if (mouseInput.x >= xMenuItemValue && mouseInput.x < xMenuItemValue + wMenuItemValue)
				{
					for (size_t i = 0; i < pickerItemsCount; ++i)
					{
						const int yPickerItem = yPicker + dyPickerItem * i;

						if (mouseInput.y >= yPickerItem && mouseInput.y < yPickerItem + hPickerItem)
						{
							if (pickerSelectedIndex != i)
							{
								JE_playSampleNum(S_CURSOR);

								pickerSelectedIndex = i;
							}

							// Act on picker item.
							if (mouseInput.button == SDL_BUTTON_LEFT &&
							    mouseInput.x >= xMenuItemValue && mouseInput.y < xMenuItemValue + wMenuItemName &&
							    mouseInput.y >= yPickerItem && mouseInput.y < yPickerItem + hPickerItem)
							{
								action = true;
							}
						}
					}
				}

				if (mouseInput.button == SDL_BUTTON_RIGHT)
				{
					JE_playSampleNum(S_SPRING);

					currentPicker = MENU_ITEM_NONE;
				}
			}
			else if (keyboardGetInput(&keyboardInput))
			{
				switch (keyboardInput.scancode)
				{
				case SDL_SCANCODE_UP:
				{
					JE_playSampleNum(S_CURSOR);

					const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

					pickerSelectedIndex = pickerSelectedIndex == 0
						? pickerItemsCount - 1
						: pickerSelectedIndex - 1;
					break;
				}
				case SDL_SCANCODE_DOWN:
				{
					JE_playSampleNum(S_CURSOR);

					const size_t pickerItemsCount = selectedMenuItem->getPickerItemsCount();

					pickerSelectedIndex = pickerSelectedIndex == pickerItemsCount - 1
						? 0
						: pickerSelectedIndex + 1;
					break;
				}
				case SDL_SCANCODE_SPACE:
				case SDL_SCANCODE_RETURN:
				{
					action = true;
					break;
				}
				case SDL_SCANCODE_ESCAPE:
				{
					JE_playSampleNum(S_SPRING);

					currentPicker = MENU_ITEM_NONE;
					break;
				}
				default:
					break;
				}
			}

			if (action)
			{
				JE_playSampleNum(S_CLICK);

				switch (selectedMenuItem->id)
				{
				case MENU_ITEM_DISPLAY:
				{
					if ((int)pickerSelectedIndex - 1 != fullscreen_display)
						reinit_fullscreen((int)pickerSelectedIndex - 1);
					break;
				}
				case MENU_ITEM_SCALER:
				{
					if (pickerSelectedIndex != scaler)
					{
						const int oldScaler = scaler;
						if (!init_scaler(pickerSelectedIndex) &&  // try new scaler
							!init_scaler(oldScaler))              // revert on fail
						{
							exit(EXIT_FAILURE);
						}
					}
					break;
				}
				case MENU_ITEM_SCALING_MODE:
				{
					scaling_mode = pickerSelectedIndex;
					break;
				}
				case MENU_ITEM_HD_MUSIC:
				{
					bool value = pickerSelectedIndex == 1;
					if (value != hd_music)
					{
						hd_music = value;
						refresh_current_song();  // swap the live track OGG<->synth now
					}
					break;
				}
				case MENU_ITEM_HD_SFX:
				{
					bool value = pickerSelectedIndex == 1;
					if (value != hd_sfx)
					{
						hd_sfx = value;
						reload_sound_samples(xmas);  // rebuild the sample banks now
					}
					break;
				}
				case MENU_ITEM_HD_GRAPHICS:
				{
					// Applies live: hd_mode is read per present by scale_and_flip
					// (backdrops, sprites, fonts, flight compositor, HUD overlay) and
					// by interp_flight_emit for tiles, so the toggle applies live --
					// no reload is needed. One caveat: a screen's HD *backdrop* is
					// armed on screen entry (hd_set_backdrop), so turning HD on
					// mid-screen shows HD text/sprites immediately but the backdrop
					// may only appear on the next screen change; this matches how
					// the flag already behaved.
					hd_mode = pickerSelectedIndex == 1;
					break;
				}
				case MENU_ITEM_HIGHFPS:
				{
					// Applies live: interp/flight_present reads highfps_mode each present, so
					// no reload is needed -- the next frame starts (or stops) interpolating.
					highfps_mode = pickerSelectedIndex == 1;
					break;
				}
				default:
					break;
				}

				currentPicker = MENU_ITEM_NONE;
			}
		}
	}
}

int main(int argc, char *argv[])
{
	mt_srand(time(NULL));

	printf("\nWelcome to... >> %s %s <<\n\n", opentyrian_str, opentyrian_version);

	printf("Copyright (C) 2022 The OpenTyrian Development Team\n\n");

	printf("This program comes with ABSOLUTELY NO WARRANTY.\n");
	printf("This is free software, and you are welcome to redistribute it\n");
	printf("under certain conditions.  See the file COPYING for details.\n\n");

	if (SDL_Init(0))
	{
		printf("Failed to initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	loadConfiguration();
	loadSaves();

	xmas = xmas_time();  // arg handler may override

	JE_paramCheck(argc, argv);

	JE_scanForEpisodes();

	if (edit_roundtrip_episode != 0)
	{
		// Hidden level-editor archive-I/O self-test (Phase E0): no video/audio
		// needed, so this runs and exits before any subsystem init below.
		bool ok = lvledit_run_roundtrip_test(edit_roundtrip_episode);
		return ok ? 0 : 1;
	}

	if (edit_addlevel_episode != 0)
	{
		// Hidden level-editor add-level self-test (Phase E4): same rationale
		// as edit_roundtrip_episode above -- no video/audio needed.
		bool ok = lvledit_run_addlevel_test(edit_addlevel_episode);
		return ok ? 0 : 1;
	}

	init_video();
	init_keyboard();
	init_controllers();
	printf("assuming mouse detected\n"); // SDL can't tell us if there isn't one

	if (xmas && (!dir_file_exists(data_dir(), "tyrianc.shp") || !dir_file_exists(data_dir(), "voicesc.snd")))
	{
		xmas = false;

		fprintf(stderr, "warning: Christmas is missing.\n");
	}

	JE_loadPals();
	JE_loadMainShapeTables(xmas ? "tyrianc.shp" : "tyrian.shp");

	if (xmas && !xmas_prompt())
	{
		xmas = false;

		free_main_shape_tables();
		JE_loadMainShapeTables("tyrian.shp");
	}

	/* Default Options */
	youAreCheating = false;
	smoothScroll = true;
	loadDestruct = false;

	if (!audio_disabled)
	{
		printf("initializing SDL audio...\n");

		init_audio();

		load_music();

		loadSndFile(xmas);
	}
	else
	{
		printf("audio disabled\n");
	}

	if (record_demo)
		printf("demo recording enabled (input limited to keyboard)\n");

	JE_loadExtraShapes();  /*Editship*/

	JE_loadHelpText();
	/*debuginfo("Help text complete");*/

	if (isNetworkGame)
	{
#ifdef WITH_NETWORK
		if (network_init())
		{
			network_tyrian_halt(3, false);
		}
#else
		fprintf(stderr, "OpenTyrian was compiled without networking support.");
		JE_tyrianHalt(5);
#endif
	}

#ifdef NDEBUG
	if (!isNetworkGame)
		intro_logos();
#endif

	if (edit_requested)
	{
		// Hidden interactive level editor (Phase E1): boots straight into
		// the editor loop instead of the normal title-screen/game loop.
		// lvledit_run(0) starts at the in-app episode picker, which then
		// leads into the existing per-episode level-select; exits when the
		// user quits the editor.
		lvledit_run(0);
		return 0;
	}

	if (edit_shot_requested)
	{
		// Hidden headless level-editor screenshot dump: renders a fixed set
		// of editor screens for one level and saves them as BMPs, with no
		// interactive loop. Needs video/fonts initialized (done above), same
		// as --edit, so it's hooked in the same place, before titleScreen.
		lvledit_dump_screens(edit_shot_episode, edit_shot_level);
		return 0;
	}

	if (edit_export_requested)
	{
		// Hidden headless full-level PNG map export: same hook point as
		// --edit-shot (needs video/fonts initialized, no interactive loop).
		bool ok = lvledit_export_map_cli(edit_export_episode, edit_export_level);
		return ok ? 0 : 1;
	}

	for (; ; )
	{
		JE_initPlayerData();
		JE_sortHighScores();

		play_demo = false;
		stopped_demo = false;

		gameLoaded = false;
		jumpSection = false;

#ifdef WITH_NETWORK
		if (isNetworkGame)
		{
			networkStartScreen();
		}
		else
#endif
		{
			if (!titleScreen())
			{
				// Player quit from title screen.
				break;
			}
		}

		if (loadDestruct)
		{
			JE_destructGame();

			loadDestruct = false;
		}
		else
		{
			JE_main();

			if (trentWin)
			{
				// Player beat SuperTyrian.
				break;
			}
		}
	}

	JE_tyrianHalt(0);

	return 0;
}
