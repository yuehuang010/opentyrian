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
#ifndef CONTROLLER_H
#define CONTROLLER_H

#include "opentyr.h"
#include "config_file.h"

#include "SDL.h"

typedef enum
{
	CONTROLLER_BIND_NONE,
	CONTROLLER_BIND_BUTTON,
	CONTROLLER_BIND_AXIS
}
Controller_bind_type;

typedef struct
{
	Controller_bind_type type;
	int num; // SDL_GameControllerButton or SDL_GameControllerAxis

	// if axis
	bool negative_axis; // else positive
}
Controller_binding;

typedef struct
{
	SDL_GameController *handle;
	SDL_JoystickID instance_id;  // hotplug identity -- NOT the device index
	SDL_GameControllerType type; // drives Xbox vs PlayStation vs Nintendo labels
	char name[64];                // config section key

	Controller_binding assignment[11][2]; // 0-3: directions, 4-10: actions

	bool analog;
	int sensitivity, threshold;

	signed int x, y;
	int analog_direction[4];
	bool direction[4], direction_pressed[4]; // up, right, down, left  (_pressed, for emulating key presses)

	bool confirm, cancel;
	bool action[7], action_pressed[7]; // fire, change fire, left sidekick, right sidekick, menu, pause, ship morph

	Uint32 controller_delay;
	bool input_pressed;
}
Controller;

extern int controller_repeat_delay;
extern bool controllerdown;
extern bool ignore_controller;
extern int controllers;
extern Controller *controller;

int controller_axis_reduce(int c, int value);
bool controller_analog_angle(int c, float *angle);

void poll_controller(int c);
void poll_controllers(void);

void push_key(SDL_Scancode key);
void push_controllers_as_keyboard(void);

void init_controllers(void);
void deinit_controllers(void);

void controller_device_added(int device_index);
void controller_device_removed(SDL_JoystickID instance_id);

bool controller_is_connected(int c);

void reset_controller_assignments(int c);
bool load_controller_assignments(Config* config, int c);
bool save_controller_assignments(Config* config, int c);

void controller_assignments_to_string(char *buffer, size_t buffer_len, const Controller *c, const Controller_binding *assignments);
const char *controller_binding_label(const Controller *c, const Controller_binding *assignment);

bool detect_controller_assignment(int c, Controller_binding *assignment);
bool controller_assignment_cmp(const Controller_binding *, const Controller_binding *);

#endif /* CONTROLLER_H */
