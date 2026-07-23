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
/** @file params.c
 * Command-line argument parsing, dispatching flags to the relevant subsystems.
 *
 * Entry points: JE_paramCheck().
 */

#include "params.h"

#include "arg_parse.h"
#include "controller.h"
#include "file.h"
#include "loudness.h"
#include "network.h"
#include "opentyr.h"
#include "varz.h"
#include "xmas.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

JE_boolean richMode = false, constantPlay = false, constantDie = false;

int edit_roundtrip_episode = 0;
bool edit_requested = false;

bool edit_shot_requested = false;
int edit_shot_episode = 0;
int edit_shot_level = 0;

bool edit_export_requested = false;
int edit_export_episode = 0;
int edit_export_level = 0;

/* YKS: Note: LOOT cheat had non letters removed. */
const char pars[][9] = {
	"LOOT", "RECORD", "NOJOY", "CONSTANT", "DEATH", "NOSOUND", "NOXMAS", "YESXMAS"
};

void JE_paramCheck(int argc, char *argv[])
{
	const Options options[] =
	{
		{ 'h', 'h', "help",              false },
		
		{ 's', 's', "no-sound",          false },
		{ 'j', 'j', "no-joystick",       false },
		{ 'x', 'x', "no-xmas",           false },
		
		{ 't', 't', "data",              true },
		
		{ 'n', 'n', "net",               true },
		{ 256, 0,   "net-player-name",   true }, // TODO: no short codes because there should
		{ 257, 0,   "net-player-number", true }, //       be a menu for entering these in the future
		{ 'p', 'p', "net-port",          true },
		{ 'd', 'd', "net-delay",         true },

		// Hidden: interactive level editor (Phase E1), not shown in --help.
		// Listed before edit-roundtrip so an exact "--edit" match short-
		// circuits before the partial-match ambiguity check (arg_parse.c's
		// long-option matcher flags ambiguity only among options it hasn't
		// already resolved with an exact match).
		{ 259, 0,   "edit",              false },
		// Hidden: level-editor archive-I/O self-test (Phase E0), not shown in --help.
		{ 258, 0,   "edit-roundtrip",    true },
		// Hidden: headless level-editor screenshot dump, not shown in --help.
		// Shares the "edit" prefix with the two options above, but "edit" is
		// listed first and is an exact match on its own, so it resolves (and
		// breaks out of the matcher) before this or edit-roundtrip are ever
		// considered -- see the ordering note above.
		{ 260, 0,   "edit-shot",         true },
		// Hidden: headless full-level PNG map export, not shown in --help.
		// Shares the "edit" prefix; same short-circuit ordering note as
		// edit-shot above applies here too.
		{ 261, 0,   "edit-export",       true },

		{ 'X', 'X', "xmas",              false },
		{ 'c', 'c', "constant",          false },
		{ 'k', 'k', "death",             false },
		{ 'r', 'r', "record",            false },
		{ 'l', 'l', "loot",              false },
		
		{ 0, 0, NULL, false}
	};
	
	Option option;
	
	for (; ; )
	{
		option = parse_args(argc, (const char **)argv, options);
		
		if (option.value == NOT_OPTION)
			break;
		
		switch (option.value)
		{
		case INVALID_OPTION:
		case AMBIGUOUS_OPTION:
		case OPTION_MISSING_ARG:
			fprintf(stderr, "Try `%s --help' for more information.\n", argv[0]);
			exit(EXIT_FAILURE);
			break;
			
		case 'h':
			printf("Usage: %s [OPTION...]\n\n"
			       "Options:\n"
			       "  -h, --help                   Show help about options\n\n"
			       "  -s, --no-sound               Disable audio\n"
			       "  -j, --no-joystick            Disable joystick/gamepad input\n"
			       "  -x, --no-xmas                Disable Christmas mode\n\n"
			       "  -t, --data=DIR               Set Tyrian data directory\n\n"
			       "  -n, --net=HOST[:PORT]        Start a networked game\n"
			       "  --net-player-name=NAME       Sets local player name in a networked game\n"
			       "  --net-player-number=NUMBER   Sets local player number in a networked game\n"
			       "                               (1 or 2)\n"
			       "  -p, --net-port=PORT          Local port to bind (default is 1333)\n"
			       "  -d, --net-delay=FRAMES       Set lag-compensation delay (default is 1)\n", argv[0]);
			exit(0);
			break;
			
		case 's':
			// Disables sound/music usage
			audio_disabled = true;
			break;
			
		case 'j':
			// Disables controller detection
			ignore_controller = true;
			break;
			
		case 'x':
			xmas = false;
			break;
			
		// set custom Tyrian data directory
		case 't':
			custom_data_dir = option.arg;
			break;
			
		case 'n':
			isNetworkGame = true;
			
			intptr_t temp = (intptr_t)strchr(option.arg, ':');
			if (temp)
			{
				temp -= (intptr_t)option.arg;
				
				int temp_port = atoi(&option.arg[temp + 1]);
				if (temp_port > 0 && temp_port < 49152)
					network_opponent_port = temp_port;
				else
				{
					fprintf(stderr, "%s: error: invalid network port number\n", argv[0]);
					exit(EXIT_FAILURE);
				}
				
				network_opponent_host = malloc(temp + 1);
				SDL_strlcpy(network_opponent_host, option.arg, temp + 1);
			}
			else
			{
				network_opponent_host = malloc(strlen(option.arg) + 1);
				strcpy(network_opponent_host, option.arg);
			}
			break;
			
		case 256: // --net-player-name
			network_player_name = malloc(strlen(option.arg) + 1);
			strcpy(network_player_name, option.arg);
			break;
			
		case 257: // --net-player-number
		{
			int temp = atoi(option.arg);
			if (temp >= 1 && temp <= 2)
				thisPlayerNum = temp;
			else
			{
				fprintf(stderr, "%s: error: invalid network player number\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}
		case 'p':
		{
			int temp = atoi(option.arg);
			if (temp > 0 && temp < 49152)
				network_player_port = temp;
			else
			{
				fprintf(stderr, "%s: error: invalid network port number\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}
		case 'd':
		{
			int temp;
			if (sscanf(option.arg, "%d", &temp) == 1)
				network_delay = 1 + temp;
			else
			{
				fprintf(stderr, "%s: error: invalid network delay value\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}
		case 258: // --edit-roundtrip (hidden)
		{
			int temp = atoi(option.arg);
			if (temp >= 1 && temp <= 4)
				edit_roundtrip_episode = temp;
			else
			{
				fprintf(stderr, "%s: error: invalid --edit-roundtrip episode (expected 1-4)\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}
		case 259: // --edit (hidden)
			edit_requested = true;
			break;
		case 260: // --edit-shot (hidden)
		{
			int temp_episode, temp_level;
			if (sscanf(option.arg, "%d,%d", &temp_episode, &temp_level) == 2 &&
			    temp_episode >= 1 && temp_episode <= 4 && temp_level >= 0)
			{
				edit_shot_episode = temp_episode;
				edit_shot_level = temp_level;
				edit_shot_requested = true;
			}
			else
			{
				fprintf(stderr, "%s: error: invalid --edit-shot value (expected <episode 1-4>,<level>=0)\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}

		case 261: // --edit-export (hidden)
		{
			int temp_episode, temp_level;
			if (sscanf(option.arg, "%d,%d", &temp_episode, &temp_level) == 2 &&
			    temp_episode >= 1 && temp_episode <= 4 && temp_level >= 0)
			{
				edit_export_episode = temp_episode;
				edit_export_level = temp_level;
				edit_export_requested = true;
			}
			else
			{
				fprintf(stderr, "%s: error: invalid --edit-export value (expected <episode 1-4>,<level>=0)\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			break;
		}

		case 'X':
			xmas = true;
			break;
			
		case 'c':
			/* Constant play for testing purposes (C key activates invincibility)
			   This might be useful for publishers to see everything - especially
			   those who can't play it */
			constantPlay = true;
			break;
			
		case 'k':
			constantDie = true;
			break;
			
		case 'r':
			record_demo = true;
			break;
			
		case 'l':
			// Gives you mucho bucks
			richMode = true;
			break;
			
		default:
			assert(false);
			break;
		}
	}
	
	// legacy parameter support
	for (int i = option.argn; i < argc; ++i)
	{
		for (uint j = 0; j < strlen(argv[i]); ++j)
			argv[i][j] = toupper((unsigned char)argv[i][j]);
		
		for (uint j = 0; j < COUNTOF(pars); ++j)
		{
			if (strcmp(argv[i], pars[j]) == 0)
			{
				switch (j)
				{
				case 0:
					richMode = true;
					break;
				case 1:
					record_demo = true;
					break;
				case 2:
					ignore_controller = true;
					break;
				case 3:
					constantPlay = true;
					break;
				case 4:
					constantDie = true;
					break;
				case 5:
					audio_disabled = true;
					break;
				case 6:
					xmas = false;
					break;
				case 7:
					xmas = true;
					break;
					
				default:
					assert(false);
					break;
				}
			}
		}
	}
}
