# Modern Controller Support (SDL_GameController)

**Goal:** replace the raw `SDL_Joystick` subsystem with `SDL_GameController`, so Xbox /
PlayStation / Switch pads work out of the box with *named* buttons, live hotplug, and a
remapping screen that speaks the controller's own vocabulary ("A", "LB", "L STICK UP")
instead of `AX 1+` / `BTN 3` / `H 1X-`.

**Decisions (settled with the user 2026-07-16):**
- **Pure replace.** The raw `SDL_Joystick` path is deleted, not kept as a fallback.
  Devices outside SDL's mapping DB (flight sticks, wheels, arcade sticks) stop working;
  mitigated by loading an optional user-supplied `gamecontrollerdb.txt`.
- **Hotplug is in scope.** `SDL_CONTROLLERDEVICEADDED` / `REMOVED` handled live.

## Why this is tractable

The consumer surface is tiny. Gameplay reads controller state in exactly **one** place
(`JE_playerMovement`, `mainint.c:3785-3814`). Everything else is the options screen
(`game_menu.c`) and a "pretend the pad is a keyboard" shim used by the menu/wait loops.
These struct fields are read nowhere outside the subsystem and are effectively private:
`confirm`, `cancel`, `input_pressed`, `joystick_delay`, `analog_direction[]`.

## Module

`src/joystick.{c,h}` → **`src/controller.{c,h}`** (rename + rewrite). The Makefile globs
`src/*.c`, so no Makefile edit is needed. Update the `#include "joystick.h"` in
`xmas.c`, `tyrian2.c`, `opentyr.c`, `network.c`, `game_menu.c`, `keyboard.c`,
`jukebox.c`, `nortsong.c`, `menus.c`, `starlib.c` (whichever actually have it).

### Data model

Keep the **same 10 action slots × 2 bindings** shape as today. This is what makes the
gameplay code a near-mechanical rename rather than a rewrite.

```c
typedef enum { CONTROLLER_BIND_NONE, CONTROLLER_BIND_BUTTON, CONTROLLER_BIND_AXIS }
Controller_bind_type;

typedef struct {
	Controller_bind_type type;
	int num;             // SDL_GameControllerButton or SDL_GameControllerAxis
	bool negative_axis;  // axis only
} Controller_binding;

typedef struct {
	SDL_GameController *handle;
	SDL_JoystickID instance_id;   // hotplug identity — NOT the device index
	SDL_GameControllerType type;  // drives Xbox vs PlayStation labels
	char name[64];                // config section key

	Controller_binding assignment[10][2];  // 0-3 directions, 4-9 actions

	bool analog;
	int sensitivity, threshold;

	signed int x, y;
	int analog_direction[4];
	bool direction[4], direction_pressed[4];
	bool confirm, cancel;
	bool action[6], action_pressed[6];

	Uint32 controller_delay;
	bool input_pressed;
} Controller;

extern int controllers;         // count, replaces `joysticks`
extern Controller *controller;  // replaces `joystick`
extern bool controllerdown;     // replaces `joydown`
extern bool ignore_controller;  // replaces `ignore_joystick`
```

Slot order is unchanged and load-bearing — `mainint.c` indexes it directly:
`0..3` = up, right, down, left; `4..9` = fire, change fire, left sidekick,
right sidekick, menu, pause.

### Binding evaluation

`check_assigned()` keeps its contract — returns `0..32767` — only the switch body
changes:

- `CONTROLLER_BIND_BUTTON` → `SDL_GameControllerGetButton()` ? 32767 : 0
- `CONTROLLER_BIND_AXIS` → `SDL_GameControllerGetAxis()`, negated if `negative_axis`

The `HAT` case disappears: the D-pad is now four ordinary buttons
(`SDL_CONTROLLER_BUTTON_DPAD_UP` etc.).

This preserves a property worth keeping: because a button evaluates to full scale,
**the D-pad works in analog mode and the stick works in digital mode**. Digital mode
compares `analog_direction[d] > 32767/2`, so a stick past 50% deflection registers;
analog mode feeds `x`/`y`, which a D-pad button drives to full scale.

## Defaults — the key map

`reset_controller_assignments()`:

| Action | Slot 0 | Slot 1 |
|---|---|---|
| up / right / down / left | left stick (`LEFTY-`/`LEFTX+`/`LEFTY+`/`LEFTX-`) | D-pad |
| fire | `A` | `RIGHTTRIGGER+` |
| change fire | `B` | `Y` |
| left sidekick | `LEFTSHOULDER` | `LEFTTRIGGER+` |
| right sidekick | `RIGHTSHOULDER` | `X` |
| menu | `START` | — |
| pause | `BACK` | — |

This is chosen so the derived menu bindings land where a console player expects:
`confirm = action[0] || action[4]` → **A or START**; `cancel = action[1] || action[5]`
→ **B or BACK**. Do not change the confirm/cancel derivation; just pick bindings that
make it come out right.

**`analog` now defaults to `true`** (was `false`). A modern pad's primary input is an
analog stick; defaulting to digital wastes it. Safe because the D-pad still works in
analog mode (see above). `sensitivity = 5`, `threshold = 5` unchanged — threshold 5
is a ~15% deadzone (`threshold * 1000` out of 32767), which is right for a modern stick.

## Hotplug

Keep `controller[]` **contiguous** and compact on removal, so every existing
`for (j = 0; j < controllers; j++)` loop and the `inputDevice - 3` index arithmetic keep
working unchanged. Index shift on unplug is acceptable: bindings are keyed by *name* in
the config, so re-plugging restores the right map.

In `handleSdlEvents()` (`keyboard.c:187-384`), add two cases:

```c
case SDL_CONTROLLERDEVICEADDED:   controller_device_added(ev.cdevice.which);   break;
case SDL_CONTROLLERDEVICEREMOVED: controller_device_removed(ev.cdevice.which); break;
```

> **Trap:** `ADDED.which` is a **device index**; `REMOVED.which` is an **instance id**.
> They are different namespaces. Match removal against `instance_id`
> (`SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(h))`).

Init must use `SDL_INIT_GAMECONTROLLER`, and must explicitly enable the device events
while still ignoring per-frame state events (we poll state, we don't consume it from the
queue):

```c
SDL_GameControllerEventState(SDL_IGNORE);
SDL_EventState(SDL_CONTROLLERDEVICEADDED,   SDL_ENABLE);
SDL_EventState(SDL_CONTROLLERDEVICEREMOVED, SDL_ENABLE);
```

> **Trap:** SDL queues `CONTROLLERDEVICEADDED` for devices already present when the
> subsystem initializes. `init_controllers()` also enumerates. **Dedupe by
> `instance_id`** in the added-handler or pads get added twice.

Callers that must stay correct across a removal:
- `game_menu.c`'s `joystick_config` (the pad being configured) — clamp after removal.
- `game_menu.c:780-795` already clamps `inputDevice[i] > 2 + joysticks`. Keep it.
- `deinit_controllers()` must not double-free a pad removed at runtime.

## Config

New section `[controller "<SDL_GameControllerName()>"]` in `opentyrian.cfg`. Old
`[joystick "..."]` sections are left on disk and ignored — harmless, no migration.

Encode bindings with **SDL's own string vocabulary** so they round-trip exactly:
`SDL_GameControllerGetStringForButton()` / `GetStringForAxis()` and the matching
`GetButtonFromString()` / `GetAxisFromString()`. Axis codes carry a sign prefix:

```
[controller "Xbox Series X Controller"]
analog = yes
sensitivity = 5
threshold = 5
up = -lefty, dpup
fire = a, +righttrigger
left sidekick = leftshoulder, +lefttrigger
```

Keep the option names in `assignment_names[]` exactly as they are (`"up"`, `"fire"`,
`"change fire"`, …) — they're the config keys.

## Display labels (Xbox vs PlayStation)

`SDL_GameControllerGetType()` selects a label table:

- `PS3`/`PS4`/`PS5` → PlayStation: `CROSS`, `CIRCLE`, `SQUARE`, `TRIANG`, `L1`, `R1`,
  `L2`, `R2`, `SHARE`, `OPTION`, `L3`, `R3`
- `NINTENDO_SWITCH_PRO` → Nintendo. **Note the A/B/X/Y positions are swapped vs Xbox.**
  SDL reports *positions*, so `SDL_CONTROLLER_BUTTON_A` is the bottom button, which
  Nintendo labels "B". Label by what's printed on the pad.
- everything else → Xbox: `A`, `B`, `X`, `Y`, `LB`, `RB`, `LT`, `RT`, `BACK`, `START`,
  `LS`, `RS`

> **Width constraint:** the value column draws at x=236 on a 320px screen (~84px, small
> font ≈ 6px/char → **~14 chars**), and two bindings render per row separated by `", "`.
> That caps each label at ~6 chars — the same budget the old `char name[7]` had.
> Verify the longest realistic row (e.g. `CIRCLE, TRIANG`) actually fits before calling
> this done.

D-pad and stick directions need direction-suffixed labels within the same budget:
`D-UP`/`D-DN`/`D-LT`/`D-RT`, `LS-UP`/`LS-DN`/`LS-LT`/`LS-RT`, `RS-…`.

## Options screen

`MENU_JOYSTICK_CONFIG` (12) → **`MENU_CONTROLLER_CONFIG`**. Keep the row layout 1:1 —
the screen is proven, only the vocabulary changes.

| Row | Content |
|---|---|
| 0 | which controller (index; name in the help line) |
| 1 | `ANALOG STICK` (was `ANALOG AXES`) |
| 2 | ` SENSITIVITY` |
| 3 | ` THRESHOLD` |
| 4-13 | the 10 action bindings (labels from `menuInt[6][…]`, keep as-is) |
| 14 | reset to defaults |
| 15 | done |

Blocks to change, all in `game_menu.c`:

| Lines | What |
|---|---|
| 449-507 | draw + label table + `menuChoices[curMenu]` |
| 1353-1360, 1383-1390 | up/down skip-disabled-when-digital |
| 1404-1435 | LEFT value edit |
| 1502-1528 | RIGHT value edit |
| 2971-3053 | select dispatch + `detect_*_assignment()` capture |
| 2417-2421 | help text table |
| 2725-2727, 2959-2961 | menu entry points (context only, no change) |
| 780-795, 1437-1457, 1530-1550, 2899-2920 | `MENU_2_PLAYER_ARCADE` device selector — uses the same count, must keep compiling |

`detect_joystick_assignment()` → `detect_controller_assignment()`: same blocking
capture loop, but scan `SDL_CONTROLLER_BUTTON_A..MAX` and `SDL_CONTROLLER_AXIS_LEFTX..MAX`
instead of raw axes/buttons/hats.

## Optional: `gamecontrollerdb.txt`

At init, if `gamecontrollerdb.txt` exists in `data_dir()`, feed it to
`SDL_GameControllerAddMappingsFromRW()`. Go through `dir_fopen()` per the repo's IO rule
(`SDL_RWFromFP(fp, SDL_TRUE)`), not a bare path. Absent file = silent no-op, not a warning.

## Verification

No test suite. "Passing" = compiles clean under **both** `make` and `make debug`
(the latter is `-Werror`), and runs.

- `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 20 ./opentyrian --data ./tyrian21`
  — must reach the attract-mode demo with no controller attached and no crash. This is
  the main regression risk: **all the `controllers == 0` paths.**
- Grep for stragglers: no `SDL_Joystick`, `joydown`, `joysticks`, `ignore_joystick`
  should remain outside of comments.
