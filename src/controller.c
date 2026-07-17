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
/** @file controller.c
 * SDL2 game controller handling: polling, button/axis assignment, hotplug,
 * config (de)serialization, and synthesizing keyboard events from controller
 * input.
 *
 * Entry points: init_controllers(), poll_controllers(),
 * load_controller_assignments(), save_controller_assignments(),
 * detect_controller_assignment(), controller_device_added(),
 * controller_device_removed().
 */

#include "controller.h"

#include "config.h"
#include "config_file.h"
#include "file.h"
#include "keyboard.h"
#include "network.h"
#include "nortsong.h"
#include "opentyr.h"
#include "params.h"
#include "varz.h"
#include "video.h"

#include <assert.h>
#include <ctype.h>
#include <string.h>

typedef enum
{
	FAMILY_XBOX,
	FAMILY_PLAYSTATION,
	FAMILY_NINTENDO,
}
Controller_family;

int controller_axis_threshold(int c, int value);
int check_assigned(SDL_GameController *handle, const Controller_binding assignment[2]);

static void load_gamecontrollerdb(void);
static void add_controller(int device_index);

static Controller_family controller_family(SDL_GameControllerType type);
static const char *button_label(Controller_family family, SDL_GameControllerButton button);
static const char *axis_label(Controller_family family, SDL_GameControllerAxis axis, bool negative_axis);

static const char *assignment_to_code(const Controller_binding *assignment);
static void code_to_assignment(Controller_binding *assignment, const char *buffer);

int controller_repeat_delay = 300; // milliseconds, repeat delay for buttons
bool controllerdown = false;       // any controller buttons down, updated by poll_controllers()
bool ignore_controller = false;

int controllers = 0;
Controller *controller = NULL;

static const int controller_analog_max = 32767;

// eliminates axis movement below the threshold
int controller_axis_threshold(int c, int value)
{
	assert(c < controllers);

	bool negative = value < 0;
	if (negative)
		value = -value;

	if (value <= controller[c].threshold * 1000)
		return 0;

	value -= controller[c].threshold * 1000;

	return negative ? -value : value;
}

// converts controller axis to sane Tyrian-usable value (based on sensitivity)
int controller_axis_reduce(int c, int value)
{
	assert(c < controllers);

	value = controller_axis_threshold(c, value);

	if (value == 0)
		return 0;

	return value / (3000 - 200 * controller[c].sensitivity);
}

// converts analog controller axes to an angle
// returns false if axes are centered (there is no angle)
bool controller_analog_angle(int c, float *angle)
{
	assert(c < controllers);

	float x = controller_axis_threshold(c, controller[c].x), y = controller_axis_threshold(c, controller[c].y);

	if (x != 0)
	{
		*angle += atanf(-y / x);
		*angle += (x < 0) ? -M_PI_2 : M_PI_2;
		return true;
	}
	else if (y != 0)
	{
		*angle += y < 0 ? M_PI : 0;
		return true;
	}

	return false;
}

/* gives back value 0..controller_analog_max indicating that one of the assigned
 * buttons has been pressed or that one of the assigned axes has been moved
 * in the assigned direction
 */
int check_assigned(SDL_GameController *handle, const Controller_binding assignment[2])
{
	int result = 0;

	for (int i = 0; i < 2; i++)
	{
		int temp = 0;

		switch (assignment[i].type)
		{
		case CONTROLLER_BIND_NONE:
			continue;

		case CONTROLLER_BIND_AXIS:
			temp = SDL_GameControllerGetAxis(handle, (SDL_GameControllerAxis)assignment[i].num);

			if (assignment[i].negative_axis)
				temp = -temp;
			break;

		case CONTROLLER_BIND_BUTTON:
			temp = SDL_GameControllerGetButton(handle, (SDL_GameControllerButton)assignment[i].num) ? controller_analog_max : 0;
			break;
		}

		if (temp > result)
			result = temp;
	}

	return result;
}

// updates controller state
void poll_controller(int c)
{
	assert(c < controllers);

	if (controller[c].handle == NULL)
		return;

	SDL_GameControllerUpdate();

	// indicates that a direction/action was pressed since last poll
	controller[c].input_pressed = false;

	// indicates that a direction/action has been held long enough to fake a repeat press
	bool repeat = controller[c].controller_delay < SDL_GetTicks();

	// update direction state
	for (uint d = 0; d < COUNTOF(controller[c].direction); d++)
	{
		bool old = controller[c].direction[d];

		controller[c].analog_direction[d] = check_assigned(controller[c].handle, controller[c].assignment[d]);
		controller[c].direction[d] = controller[c].analog_direction[d] > (controller_analog_max / 2);
		controllerdown |= controller[c].direction[d];

		controller[c].direction_pressed[d] = controller[c].direction[d] && (!old || repeat);
		controller[c].input_pressed |= controller[c].direction_pressed[d];
	}

	controller[c].x = -controller[c].analog_direction[3] + controller[c].analog_direction[1];
	controller[c].y = -controller[c].analog_direction[0] + controller[c].analog_direction[2];

	// update action state
	for (uint d = 0; d < COUNTOF(controller[c].action); d++)
	{
		bool old = controller[c].action[d];

		controller[c].action[d] = check_assigned(controller[c].handle, controller[c].assignment[d + COUNTOF(controller[c].direction)]) > (controller_analog_max / 2);
		controllerdown |= controller[c].action[d];

		controller[c].action_pressed[d] = controller[c].action[d] && (!old || repeat);
		controller[c].input_pressed |= controller[c].action_pressed[d];
	}

	controller[c].confirm = controller[c].action[0] || controller[c].action[4];
	controller[c].cancel = controller[c].action[1] || controller[c].action[5];

	// if new input, reset press-repeat delay
	if (controller[c].input_pressed)
		controller[c].controller_delay = SDL_GetTicks() + controller_repeat_delay;
}

// updates all controller states
void poll_controllers(void)
{
	controllerdown = false;

	for (int c = 0; c < controllers; c++)
		poll_controller(c);
}

// sends SDL KEYDOWN and KEYUP events for a key
void push_key(SDL_Scancode key)
{
	SDL_Event e;

	memset(&e.key.keysym, 0, sizeof(e.key.keysym));

	e.key.keysym.scancode = key;
	e.key.state = SDL_RELEASED;

	e.type = SDL_KEYDOWN;
	SDL_PushEvent(&e);

	e.type = SDL_KEYUP;
	SDL_PushEvent(&e);
}

// helps us be lazy by pretending controllers are a keyboard (useful for menus)
void push_controllers_as_keyboard(void)
{
	const SDL_Scancode confirm = SDL_SCANCODE_RETURN, cancel = SDL_SCANCODE_ESCAPE;
	const SDL_Scancode direction[4] = { SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT };

	poll_controllers();

	for (int c = 0; c < controllers; c++)
	{
		if (!controller[c].input_pressed)
			continue;

		if (controller[c].confirm)
			push_key(confirm);
		if (controller[c].cancel)
			push_key(cancel);

		for (uint d = 0; d < COUNTOF(controller[c].direction_pressed); d++)
		{
			if (controller[c].direction_pressed[d])
				push_key(direction[d]);
		}
	}
}

// loads an optional user-supplied gamecontrollerdb.txt from the data dir, if present
//
// reads the file into memory ourselves rather than handing SDL a FILE* (via
// SDL_RWFromFP): on Windows, SDL may be linked against a different CRT, and a
// FILE* created by our runtime isn't valid to fread/fclose across that boundary.
static void load_gamecontrollerdb(void)
{
	FILE *f = dir_fopen(data_dir(), "gamecontrollerdb.txt", "rb");
	if (f == NULL)
		return; // absent file: silent no-op

	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return;
	}

	long size = ftell(f);
	if (size < 0 || fseek(f, 0, SEEK_SET) != 0)
	{
		fclose(f);
		return;
	}

	char *buffer = malloc((size_t)size);
	if (buffer == NULL)
	{
		fprintf(stderr, "warning: out of memory loading gamecontrollerdb.txt\n");
		fclose(f);
		return;
	}

	size_t read = fread(buffer, 1, (size_t)size, f);
	fclose(f);

	if (read != (size_t)size)
	{
		fprintf(stderr, "warning: failed to read gamecontrollerdb.txt\n");
		free(buffer);
		return;
	}

	SDL_RWops *rw = SDL_RWFromConstMem(buffer, (int)size);
	if (rw == NULL)
	{
		fprintf(stderr, "warning: failed to load gamecontrollerdb.txt: %s\n", SDL_GetError());
		free(buffer);
		return;
	}

	// SDL_GameControllerAddMappingsFromRW(rw, 1) frees the RWops (the 1 means
	// "close it for me"); it does not touch our buffer, so we still free that
	if (SDL_GameControllerAddMappingsFromRW(rw, 1) < 0)
		fprintf(stderr, "warning: failed to load gamecontrollerdb.txt: %s\n", SDL_GetError());

	free(buffer);
}

// opens the controller at the given device index and appends it to controller[],
// unless a controller with the same instance id is already tracked (SDL queues
// CONTROLLERDEVICEADDED even for devices init_controllers() already enumerated),
// or adopts a remembered (disconnected) slot for the same pad, keeping its bindings
static void add_controller(int device_index)
{
	if (!SDL_IsGameController(device_index))
		return;

	SDL_GameController *handle = SDL_GameControllerOpen(device_index);
	if (handle == NULL)
	{
		fprintf(stderr, "warning: failed to open controller %d: %s\n", device_index, SDL_GetError());
		return;
	}

	SDL_JoystickID instance_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(handle));

	for (int c = 0; c < controllers; c++)
	{
		if (controller[c].handle == NULL)
			continue; // disconnected slots all carry instance_id == -1; skip to avoid false matches

		if (controller[c].instance_id == instance_id)
		{
			// already tracked; this is the redundant ADDED event SDL queues at init
			SDL_GameControllerClose(handle);
			return;
		}
	}

	const char *raw_name = SDL_GameControllerName(handle);
	const char *name = raw_name != NULL ? raw_name : "Controller";

	// adopt a remembered (disconnected) slot for the same pad rather than appending a
	// new one, so the options screen keeps showing the same slot's edited bindings
	for (int c = 0; c < controllers; c++)
	{
		if (controller[c].handle != NULL)
			continue;

		if (strncmp(controller[c].name, name, sizeof(controller[c].name) - 1) != 0)
			continue;

		controller[c].handle = handle;
		controller[c].instance_id = instance_id;
		controller[c].type = SDL_GameControllerGetType(handle);

		printf("controller reconnected: %s\n", controller[c].name);
		return;
	}

	Controller *grown = realloc(controller, (controllers + 1) * sizeof(*controller));
	if (grown == NULL)
	{
		fprintf(stderr, "warning: out of memory adding controller\n");
		SDL_GameControllerClose(handle);
		return;
	}
	controller = grown;

	int c = controllers++;
	memset(&controller[c], 0, sizeof(controller[c]));

	controller[c].handle = handle;
	controller[c].instance_id = instance_id;
	controller[c].type = SDL_GameControllerGetType(handle);

	strncpy(controller[c].name, name, sizeof(controller[c].name) - 1);

	printf("controller detected: %s\n", controller[c].name);

	if (!load_controller_assignments(&opentyrian_config, c))
		reset_controller_assignments(c);
}

// finds the tracked controller with this instance id, or NULL if it is gone
static SDL_GameController *handle_for_instance(SDL_JoystickID instance_id)
{
	for (int c = 0; c < controllers; c++)
	{
		if (controller[c].instance_id == instance_id)
			return controller[c].handle;
	}

	return NULL;
}

// handles SDL_CONTROLLERDEVICEADDED; `device_index` (NOT an instance id)
void controller_device_added(int device_index)
{
	if (ignore_controller)
		return;

	add_controller(device_index);
}

// handles SDL_CONTROLLERDEVICEREMOVED; `instance_id` (NOT a device index)
//
// the slot is kept (not compacted out of controller[]) so the options screen can
// keep showing its mapping after a disconnect; only the handle is dropped and the
// live input state is zeroed so a pad yanked mid-press doesn't leave input latched
void controller_device_removed(SDL_JoystickID instance_id)
{
	if (ignore_controller)
		return;

	for (int c = 0; c < controllers; c++)
	{
		if (controller[c].instance_id != instance_id)
			continue;

		save_controller_assignments(&opentyrian_config, c);

		if (controller[c].handle != NULL)
			SDL_GameControllerClose(controller[c].handle);

		controller[c].handle = NULL;
		controller[c].instance_id = -1;

		// zero live input state; name/type/assignment/analog/sensitivity/threshold survive
		memset(controller[c].direction, 0, sizeof(controller[c].direction));
		memset(controller[c].direction_pressed, 0, sizeof(controller[c].direction_pressed));
		memset(controller[c].action, 0, sizeof(controller[c].action));
		memset(controller[c].action_pressed, 0, sizeof(controller[c].action_pressed));
		memset(controller[c].analog_direction, 0, sizeof(controller[c].analog_direction));
		controller[c].x = 0;
		controller[c].y = 0;
		controller[c].confirm = false;
		controller[c].cancel = false;
		controller[c].input_pressed = false;

		break;
	}
}

// initializes SDL game controller system and loads assignments for controllers found
void init_controllers(void)
{
	if (ignore_controller)
		return;

	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER))
	{
		fprintf(stderr, "warning: failed to initialize controller system: %s\n", SDL_GetError());
		ignore_controller = true;
		return;
	}

	// we poll state ourselves; don't also consume it from the event queue, but do
	// keep hotplug notifications flowing (handleSdlEvents() dispatches those)
	SDL_GameControllerEventState(SDL_IGNORE);
	SDL_EventState(SDL_CONTROLLERDEVICEADDED, SDL_ENABLE);
	SDL_EventState(SDL_CONTROLLERDEVICEREMOVED, SDL_ENABLE);

	load_gamecontrollerdb();

	controllers = 0;
	controller = NULL;

	const int joystick_count = SDL_NumJoysticks();
	for (int i = 0; i < joystick_count; i++)
		add_controller(i);

	if (controllers == 0)
		printf("no controllers detected\n");
}

// deinitializes SDL game controller system and saves controller assignments
void deinit_controllers(void)
{
	if (ignore_controller)
		return;

	for (int c = 0; c < controllers; c++)
	{
		save_controller_assignments(&opentyrian_config, c);

		if (controller[c].handle != NULL)
			SDL_GameControllerClose(controller[c].handle);
	}

	free(controller);
	controller = NULL;
	controllers = 0;

	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

// true if the slot has a live handle (i.e. the pad is currently plugged in)
bool controller_is_connected(int c)
{
	assert(c < controllers);

	return controller[c].handle != NULL;
}

/* a remembered (disconnected) slot has no handle to query -- assume the control
 * exists so Reset still restores a usable map for when the pad comes back */
static bool controller_has_button(int c, SDL_GameControllerButton button)
{
	if (controller[c].handle == NULL)
		return true;

	return SDL_GameControllerHasButton(controller[c].handle, button);
}

static bool controller_has_axis(int c, SDL_GameControllerAxis axis)
{
	if (controller[c].handle == NULL)
		return true;

	return SDL_GameControllerHasAxis(controller[c].handle, axis);
}

// sets assignment[a][slot] to a button binding, but only if the pad has that button
static void set_button(int c, uint a, uint slot, SDL_GameControllerButton button)
{
	if (!controller_has_button(c, button))
		return;

	controller[c].assignment[a][slot] = (Controller_binding){ CONTROLLER_BIND_BUTTON, button, false };
}

// sets assignment[a][slot] to an axis binding, but only if the pad has that axis
static void set_axis(int c, uint a, uint slot, SDL_GameControllerAxis axis, bool negative)
{
	if (!controller_has_axis(c, axis))
		return;

	controller[c].assignment[a][slot] = (Controller_binding){ CONTROLLER_BIND_AXIS, axis, negative };
}

void reset_controller_assignments(int c)
{
	assert(c < controllers);

	for (uint a = 0; a < COUNTOF(controller[c].assignment); a++)
		for (uint i = 0; i < COUNTOF(controller[c].assignment[a]); i++)
			controller[c].assignment[a][i].type = CONTROLLER_BIND_NONE;

	// directions: left stick primary, D-pad secondary
	set_axis(c, 0, 0, SDL_CONTROLLER_AXIS_LEFTY, true);                    // up
	set_button(c, 0, 1, SDL_CONTROLLER_BUTTON_DPAD_UP);

	set_axis(c, 1, 0, SDL_CONTROLLER_AXIS_LEFTX, false);                   // right
	set_button(c, 1, 1, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);

	set_axis(c, 2, 0, SDL_CONTROLLER_AXIS_LEFTY, false);                   // down
	set_button(c, 2, 1, SDL_CONTROLLER_BUTTON_DPAD_DOWN);

	set_axis(c, 3, 0, SDL_CONTROLLER_AXIS_LEFTX, true);                    // left
	set_button(c, 3, 1, SDL_CONTROLLER_BUTTON_DPAD_LEFT);

	// actions
	set_button(c, 4, 0, SDL_CONTROLLER_BUTTON_A);                          // fire
	set_axis(c, 4, 1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, false);

	set_button(c, 5, 0, SDL_CONTROLLER_BUTTON_B);                          // change fire
	set_button(c, 5, 1, SDL_CONTROLLER_BUTTON_Y);

	set_button(c, 6, 0, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);               // left sidekick
	set_axis(c, 6, 1, SDL_CONTROLLER_AXIS_TRIGGERLEFT, false);

	set_button(c, 7, 0, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);              // right sidekick
	set_button(c, 7, 1, SDL_CONTROLLER_BUTTON_X);

	set_button(c, 8, 0, SDL_CONTROLLER_BUTTON_START);                      // menu

	set_button(c, 9, 0, SDL_CONTROLLER_BUTTON_BACK);                       // pause

	controller[c].analog = true;
	controller[c].sensitivity = 5;
	controller[c].threshold = 5;
}

static const char* const assignment_names[] =
{
	"up",
	"right",
	"down",
	"left",
	"fire",
	"change fire",
	"left sidekick",
	"right sidekick",
	"menu",
	"pause",
};

bool load_controller_assignments(Config *config, int c)
{
	ConfigSection *section = config_find_section(config, "controller", controller[c].name);
	if (section == NULL)
		return false;

	if (!config_get_bool_option(section, "analog", &controller[c].analog))
		controller[c].analog = true;

	controller[c].sensitivity = config_get_or_set_int_option(section, "sensitivity", 5);

	controller[c].threshold = config_get_or_set_int_option(section, "threshold", 5);

	for (size_t a = 0; a < COUNTOF(assignment_names); ++a)
	{
		for (unsigned int i = 0; i < COUNTOF(controller[c].assignment[a]); ++i)
			controller[c].assignment[a][i].type = CONTROLLER_BIND_NONE;

		ConfigOption *option = config_get_option(section, assignment_names[a]);
		if (option == NULL)
			continue;

		foreach_option_i_value(i, value, option)
		{
			if (i >= COUNTOF(controller[c].assignment[a]))
				break;

			code_to_assignment(&controller[c].assignment[a][i], value);
		}
	}

	return true;
}

bool save_controller_assignments(Config *config, int c)
{
	ConfigSection *section = config_find_or_add_section(config, "controller", controller[c].name);
	if (section == NULL)
		exit(EXIT_FAILURE);  // out of memory

	config_set_bool_option(section, "analog", controller[c].analog, NO_YES);

	config_set_int_option(section, "sensitivity", controller[c].sensitivity);

	config_set_int_option(section, "threshold", controller[c].threshold);

	for (size_t a = 0; a < COUNTOF(assignment_names); ++a)
	{
		ConfigOption *option = config_set_option(section, assignment_names[a], NULL);
		if (option == NULL)
			exit(EXIT_FAILURE);  // out of memory

		option = config_set_value(option, NULL);
		if (option == NULL)
			exit(EXIT_FAILURE);  // out of memory

		for (size_t i = 0; i < COUNTOF(controller[c].assignment[a]); ++i)
		{
			if (controller[c].assignment[a][i].type == CONTROLLER_BIND_NONE)
				continue;

			option = config_add_value(option, assignment_to_code(&controller[c].assignment[a][i]));
			if (option == NULL)
				exit(EXIT_FAILURE);  // out of memory
		}
	}

	return true;
}

// classifies a controller type into a display label family
static Controller_family controller_family(SDL_GameControllerType type)
{
	switch (type)
	{
	case SDL_CONTROLLER_TYPE_PS3:
	case SDL_CONTROLLER_TYPE_PS4:
	case SDL_CONTROLLER_TYPE_PS5:
		return FAMILY_PLAYSTATION;

	case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
		return FAMILY_NINTENDO;

	default:
		return FAMILY_XBOX;
	}
}

/* short (6 or fewer characters) display label for a button, matching what's
 * printed on the pad. note SDL reports button *positions*, so on a Nintendo
 * pad SDL_CONTROLLER_BUTTON_A (bottom face button) is labeled "B" -- Nintendo
 * swaps A/B and X/Y relative to Xbox.
 */
static const char *button_label(Controller_family family, SDL_GameControllerButton button)
{
	switch (family)
	{
	case FAMILY_PLAYSTATION:
		switch (button)
		{
		case SDL_CONTROLLER_BUTTON_A:             return "CROSS";
		case SDL_CONTROLLER_BUTTON_B:              return "CIRCLE";
		case SDL_CONTROLLER_BUTTON_X:              return "SQUARE";
		case SDL_CONTROLLER_BUTTON_Y:              return "TRIANG";
		case SDL_CONTROLLER_BUTTON_BACK:           return "SHARE";
		case SDL_CONTROLLER_BUTTON_GUIDE:          return "PS";
		case SDL_CONTROLLER_BUTTON_START:          return "OPTION";
		case SDL_CONTROLLER_BUTTON_LEFTSTICK:      return "L3";
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return "R3";
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:   return "L1";
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:  return "R1";
		case SDL_CONTROLLER_BUTTON_DPAD_UP:        return "D-UP";
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:      return "D-DN";
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:      return "D-LT";
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:     return "D-RT";
		case SDL_CONTROLLER_BUTTON_MISC1:          return "MIC";
		case SDL_CONTROLLER_BUTTON_PADDLE1:        return "PDL1";
		case SDL_CONTROLLER_BUTTON_PADDLE2:        return "PDL2";
		case SDL_CONTROLLER_BUTTON_PADDLE3:        return "PDL3";
		case SDL_CONTROLLER_BUTTON_PADDLE4:        return "PDL4";
		case SDL_CONTROLLER_BUTTON_TOUCHPAD:       return "PAD";
		default:                                   return "?";
		}

	case FAMILY_NINTENDO:
		switch (button)
		{
		case SDL_CONTROLLER_BUTTON_A:              return "B";
		case SDL_CONTROLLER_BUTTON_B:              return "A";
		case SDL_CONTROLLER_BUTTON_X:              return "Y";
		case SDL_CONTROLLER_BUTTON_Y:              return "X";
		case SDL_CONTROLLER_BUTTON_BACK:           return "MINUS";
		case SDL_CONTROLLER_BUTTON_GUIDE:          return "HOME";
		case SDL_CONTROLLER_BUTTON_START:          return "PLUS";
		case SDL_CONTROLLER_BUTTON_LEFTSTICK:      return "LS";
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return "RS";
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:   return "L";
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:  return "R";
		case SDL_CONTROLLER_BUTTON_DPAD_UP:        return "D-UP";
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:      return "D-DN";
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:      return "D-LT";
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:     return "D-RT";
		case SDL_CONTROLLER_BUTTON_MISC1:          return "CAPTR";
		case SDL_CONTROLLER_BUTTON_PADDLE1:        return "PDL1";
		case SDL_CONTROLLER_BUTTON_PADDLE2:        return "PDL2";
		case SDL_CONTROLLER_BUTTON_PADDLE3:        return "PDL3";
		case SDL_CONTROLLER_BUTTON_PADDLE4:        return "PDL4";
		case SDL_CONTROLLER_BUTTON_TOUCHPAD:       return "PAD";
		default:                                   return "?";
		}

	case FAMILY_XBOX:
	default:
		switch (button)
		{
		case SDL_CONTROLLER_BUTTON_A:              return "A";
		case SDL_CONTROLLER_BUTTON_B:              return "B";
		case SDL_CONTROLLER_BUTTON_X:              return "X";
		case SDL_CONTROLLER_BUTTON_Y:              return "Y";
		case SDL_CONTROLLER_BUTTON_BACK:           return "BACK";
		case SDL_CONTROLLER_BUTTON_GUIDE:          return "GUIDE";
		case SDL_CONTROLLER_BUTTON_START:          return "START";
		case SDL_CONTROLLER_BUTTON_LEFTSTICK:      return "LS";
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK:     return "RS";
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:   return "LB";
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:  return "RB";
		case SDL_CONTROLLER_BUTTON_DPAD_UP:        return "D-UP";
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN:      return "D-DN";
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT:      return "D-LT";
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:     return "D-RT";
		case SDL_CONTROLLER_BUTTON_MISC1:          return "SHARE";
		case SDL_CONTROLLER_BUTTON_PADDLE1:        return "PDL1";
		case SDL_CONTROLLER_BUTTON_PADDLE2:        return "PDL2";
		case SDL_CONTROLLER_BUTTON_PADDLE3:        return "PDL3";
		case SDL_CONTROLLER_BUTTON_PADDLE4:        return "PDL4";
		case SDL_CONTROLLER_BUTTON_TOUCHPAD:       return "PAD";
		default:                                   return "?";
		}
	}
}

// short (6 or fewer characters) display label for a signed axis direction
static const char *axis_label(Controller_family family, SDL_GameControllerAxis axis, bool negative_axis)
{
	switch (axis)
	{
	case SDL_CONTROLLER_AXIS_LEFTX:  return negative_axis ? "LS-LT" : "LS-RT";
	case SDL_CONTROLLER_AXIS_LEFTY:  return negative_axis ? "LS-UP" : "LS-DN";
	case SDL_CONTROLLER_AXIS_RIGHTX: return negative_axis ? "RS-LT" : "RS-RT";
	case SDL_CONTROLLER_AXIS_RIGHTY: return negative_axis ? "RS-UP" : "RS-DN";

	case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
		switch (family)
		{
		case FAMILY_PLAYSTATION: return "L2";
		case FAMILY_NINTENDO:    return "ZL";
		case FAMILY_XBOX:
		default:                 return "LT";
		}

	case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
		switch (family)
		{
		case FAMILY_PLAYSTATION: return "R2";
		case FAMILY_NINTENDO:    return "ZR";
		case FAMILY_XBOX:
		default:                 return "RT";
		}

	default:
		return "?";
	}
}

// short on-screen label for a single binding, per the pad's own button/stick vocabulary
const char *controller_binding_label(const Controller *c, const Controller_binding *assignment)
{
	Controller_family family = controller_family(c->type);

	switch (assignment->type)
	{
	case CONTROLLER_BIND_BUTTON:
		return button_label(family, (SDL_GameControllerButton)assignment->num);

	case CONTROLLER_BIND_AXIS:
		return axis_label(family, (SDL_GameControllerAxis)assignment->num, assignment->negative_axis);

	case CONTROLLER_BIND_NONE:
	default:
		return "";
	}
}

// fills buffer with comma separated list of assigned controller functions (display labels)
void controller_assignments_to_string(char *buffer, size_t buffer_len, const Controller *c, const Controller_binding *assignments)
{
	if (buffer_len == 0)
		return;

	buffer[0] = '\0';

	bool comma = false;
	for (uint i = 0; i < 2; ++i)
	{
		if (assignments[i].type == CONTROLLER_BIND_NONE)
			continue;

		int len = snprintf(buffer, buffer_len, "%s%s",
		                   comma ? ", " : "",
		                   controller_binding_label(c, &assignments[i]));

		/* snprintf returns the length it WOULD have written; clamp so a truncated
		 * label can't advance past the end or underflow buffer_len */
		if (len < 0 || (size_t)len >= buffer_len)
			return;

		buffer += len;
		buffer_len -= len;

		comma = true;
	}
}

// encodes a binding using SDL's own string vocabulary, so config values round-trip
// exactly (e.g. "a", "dpup", "-lefty", "+righttrigger")
static const char *assignment_to_code(const Controller_binding *assignment)
{
	static char code[32];

	switch (assignment->type)
	{
	case CONTROLLER_BIND_NONE:
		code[0] = '\0';
		break;

	case CONTROLLER_BIND_BUTTON:
		snprintf(code, sizeof(code), "%s",
		         SDL_GameControllerGetStringForButton((SDL_GameControllerButton)assignment->num));
		break;

	case CONTROLLER_BIND_AXIS:
		snprintf(code, sizeof(code), "%c%s",
		         assignment->negative_axis ? '-' : '+',
		         SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)assignment->num));
		break;
	}

	return code;
}

// reverse of assignment_to_code()
static void code_to_assignment(Controller_binding *assignment, const char *buffer)
{
	memset(assignment, 0, sizeof(*assignment)); // CONTROLLER_BIND_NONE

	while (isspace((unsigned char)*buffer))
		++buffer;

	bool negative = false;
	if (*buffer == '+' || *buffer == '-')
	{
		negative = (*buffer == '-');
		++buffer;
	}

	char name[32];
	size_t len = 0;
	while (buffer[len] != '\0' && !isspace((unsigned char)buffer[len]) && len < sizeof(name) - 1)
		++len;
	memcpy(name, buffer, len);
	name[len] = '\0';

	if (len == 0)
		return;

	SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(name);
	if (axis != SDL_CONTROLLER_AXIS_INVALID)
	{
		assignment->type = CONTROLLER_BIND_AXIS;
		assignment->num = axis;
		assignment->negative_axis = negative;
		return;
	}

	SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(name);
	if (button != SDL_CONTROLLER_BUTTON_INVALID)
	{
		assignment->type = CONTROLLER_BIND_BUTTON;
		assignment->num = button;
	}
}

// captures controller input for configuring assignments
// returns false if non-controller input was detected
// TODO: input from a controller other than the one being configured probably should not be ignored
bool detect_controller_assignment(int c, Controller_binding *assignment)
{
	assert(c < controllers);

	if (controller[c].handle == NULL)
		return false; // can't press a button on a pad that isn't there

	/* the pad can vanish inside the capture loop below: handleSdlEvents() dispatches
	 * CONTROLLERDEVICEREMOVED, which closes the handle. re-resolve by instance id
	 * every pass instead of trusting a cached pointer.
	 */
	const SDL_JoystickID instance_id = controller[c].instance_id;

	SDL_GameController *handle = controller[c].handle;

	// get initial controller state to compare against to see if anything was pressed

	Sint16 axis_initial[SDL_CONTROLLER_AXIS_MAX];
	for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
		axis_initial[i] = SDL_GameControllerGetAxis(handle, (SDL_GameControllerAxis)i);

	Uint8 button_initial[SDL_CONTROLLER_BUTTON_MAX];
	for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
		button_initial[i] = SDL_GameControllerGetButton(handle, (SDL_GameControllerButton)i);

	bool detected = false;

	while (true)
	{
		setFrameCount(1);

		NETWORK_KEEP_ALIVE();

		delayUntilElapsed();

		handleSdlEvents();

		handle = handle_for_instance(instance_id);
		if (handle == NULL)
			return false; // unplugged mid-capture

		for (int i = 0; i < SDL_CONTROLLER_AXIS_MAX && !detected; ++i)
		{
			Sint16 temp = SDL_GameControllerGetAxis(handle, (SDL_GameControllerAxis)i);

			if (abs(temp - axis_initial[i]) > controller_analog_max * 2 / 3)
			{
				assignment->type = CONTROLLER_BIND_AXIS;
				assignment->num = i;
				assignment->negative_axis = temp < axis_initial[i];
				detected = true;
			}
		}

		for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX && !detected; ++i)
		{
			Uint8 new_button = SDL_GameControllerGetButton(handle, (SDL_GameControllerButton)i);

			if (new_button && !button_initial[i])
			{
				assignment->type = CONTROLLER_BIND_BUTTON;
				assignment->num = i;
				detected = true;
			}
			else
			{
				button_initial[i] = new_button;
			}
		}

		if (detected || hasInput(INPUT_NO_MOTION))
			break;
	}

	return detected;
}

// compares relevant parts of controller bindings for equality
bool controller_assignment_cmp(const Controller_binding *a, const Controller_binding *b)
{
	if (a->type == b->type)
	{
		switch (a->type)
		{
		case CONTROLLER_BIND_NONE:
			return true;
		case CONTROLLER_BIND_AXIS:
			return (a->num == b->num) &&
			       (a->negative_axis == b->negative_axis);
		case CONTROLLER_BIND_BUTTON:
			return (a->num == b->num);
		}
	}

	return false;
}
