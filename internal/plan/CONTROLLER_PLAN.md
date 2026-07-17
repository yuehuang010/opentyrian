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

**Revised 2026-07-16 (superseded the original "compact on removal" design).** A removed
pad **keeps its slot**; only its handle is dropped. `controllers` is a count of *slots*
(connected + remembered), and `handle != NULL` is the connected flag.

Rationale: the user asked that the options screen keep showing the mapping after a
disconnect, which is impossible if the slot is freed. Retention also removes the
index-shift wart the original design accepted — `inputDevice`'s `- 3` pin now survives
an unplug/replug, and re-plugging the same pad restores its slot *and* any edits made
since, not just what was last written to the config.

- **Remove:** save assignments, close the handle, `handle = NULL`, `instance_id = -1`,
  and **zero the live input state** — `direction[]`, `direction_pressed[]`, `action[]`,
  `action_pressed[]`, `analog_direction[]`, `x`, `y`, `confirm`, `cancel`,
  `input_pressed`. A pad yanked mid-press otherwise leaves a direction latched true
  forever and the ship drifts. Keep `name`/`type`/`assignment`/`analog`/`sensitivity`/
  `threshold`. Do **not** shrink `controllers`, do not `realloc`/`free`.
- **Add:** first look for a slot with `handle == NULL` and a matching `name` and adopt
  it (restore handle/instance_id/type, **keep its existing bindings** — do not reload or
  reset). Otherwise append.
- The instance-id dedupe must **skip disconnected slots**: they all carry `-1` and would
  false-match each other once two pads are unplugged.
- `deinit_controllers()` must save **every** slot, not just connected ones.
- `poll_controller()` already early-returns on `handle == NULL`; that plus the zeroed
  state is what makes a disconnected slot inert.
- `detect_controller_assignment()` returns false immediately if the slot is
  disconnected — you cannot press a button on a pad that isn't there.

Consumers stay correct: `mainint.c` no-ops on a disconnected slot (plus the `c_max`
clamp below), and `game_menu.c:780-795`'s `inputDevice[i] > 2 + controllers` still holds
— selecting a disconnected pad simply yields no input until it is plugged back in.
Slot count grows with *distinct* pads seen in a session, which is bounded and fine.

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

`MENU_JOYSTICK_CONFIG` (12) → **`MENU_CONTROLLER_CONFIG`**.

**Revised 2026-07-16:** the screen shows only what is actually adjustable. Directions,
menu and pause are **fixed** (left stick + D-pad, START, BACK) and their rows are gone,
as is the multi-controller selector. The bindings themselves still exist, still load and
save, and still work — they are simply not user-editable. Do **not** force them to the
defaults on load; hand-editing the config stays the escape hatch.

Rows are driven by a **table**, not the hardcoded `case 2..17` ladder the original
screen used — that ladder is why removing a row means renumbering six separate blocks.

| Row | curSel | Content | Binding |
|---|---|---|---|
| 0 | 2 | `ANALOG STICK` | — |
| 1 | 3 | ` SENSITIVITY` | — |
| 2 | 4 | ` THRESHOLD` | — |
| 3 | 5 | `FIRE` (`menuInt[6][5]`) | `assignment[4]` |
| 4 | 6 | `CHANGE FIRE` (`menuInt[6][6]`) | `assignment[5]` |
| 5 | 7 | `LEFT SIDEKICK` (`menuInt[6][7]`) | `assignment[6]` |
| 6 | 8 | `RIGHT SIDEKICK` (`menuInt[6][8]`) | `assignment[7]` |
| 7 | 9 | `Reset to Defaults` (`menuInt[6][9]`) | — |
| 8 | 10 | `Done` (`menuInt[6][10]`) | — |

`curSel = 2 + row`; `menuChoices[curMenu] = COUNTOF(rows) + 1` (= 10).

> `menuInt[6][1..4]` are `UP`/`DOWN`/`LEFT`/`RIGHT` in **keyboard** order — the old
> screen deliberately reordered them (`[1],[4],[2],[3]`) to match the engine's
> up/right/down/left slot order. Both are gone now, but that mismatch is why the old
> label list looked scrambled; don't "fix" it if the rows ever come back.

The mapping **stays visible while the pad is disconnected** (that's the point of slot
retention). Only fall back to `"-"` when `controllers == 0`, i.e. no pad has connected
at all this session.

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

## Two bugs hotplug introduces — fixed, keep them fixed

Adding hotplug makes `controller[]` mutate at runtime, which turns two previously
**unreachable** code paths into live bugs. Both were caught in review, not by the
compiler or the smoke test. If this subsystem is ever refactored again, re-check them.

1. **Use-after-free in `detect_controller_assignment()`.** It caches
   `SDL_GameController *handle` and then calls `handleSdlEvents()` inside its blocking
   capture loop — which now dispatches `CONTROLLERDEVICEREMOVED`, closing that handle
   and `realloc`ing the array. A wireless pad sleeping or dying on the "press a button"
   screen is enough to hit it. **Fix:** re-resolve the handle by `instance_id` every
   pass (`handle_for_instance()`) and bail out if it's gone. Never cache a handle or a
   `Controller *` across a `handleSdlEvents()` call.
2. **Out-of-bounds read in `JE_playerMovement()`.** `inputDevice` pins a pad by index
   (`inputDevice - 3`), but `c_max` was only derived from `inputDevice`, never clamped
   to `controllers`. The existing clamp (`game_menu.c:780-795`) only runs while the
   2-player menu is *drawn*, so unplugging a pinned pad mid-game indexes past the end
   (assert in debug, OOB read in release). **Fix:** clamp `c_max` to `controllers` at
   the call site.

## Verification

No test suite. "Passing" = compiles clean under **both** `make` and `make debug`
(the latter is `-Werror`), and runs.

- `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 20 ./opentyrian --data ./tyrian21`
  — must reach the attract-mode demo with no controller attached and no crash. This is
  the main regression risk: **all the `controllers == 0` paths.**
- Grep for stragglers: no `SDL_Joystick`, `joydown`, `joysticks`, `ignore_joystick`
  should remain outside of comments.

### Testing controller paths with no hardware

`SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, SDL_CONTROLLER_AXIS_MAX,
SDL_CONTROLLER_BUTTON_MAX, 0)` conjures a virtual pad that SDL treats as a real game
controller, so the whole add → defaults → save → load → remove path is testable
headlessly. `src/controller.c` links standalone against `config_file.c` plus ~7 stubs
(`opentyrian_config`, `data_dir`, `dir_fopen`, `setFrameCount`, `delayUntilElapsed`,
`handleSdlEvents`, `hasInput`). This is how the config round-trip (all 20 binding slots
byte-identical), the ADDED-vs-REMOVED namespaces, and the dedupe were verified.

Verified this way, worth not re-litigating:
- Every `SDL_GameController` button/axis string round-trips through
  `GetStringFor*`/`Get*FromString`, and `BUTTON_A == 0` really is a valid value — the
  old codec's `if (num == 0) type = NONE` 1-based quirk **must not** be carried over.
- `SDL_GameControllerEventState(SDL_IGNORE)` + explicit
  `SDL_EventState(CONTROLLERDEVICE{ADDED,REMOVED}, SDL_ENABLE)` does deliver hotplug
  events. (For the *first* pad `device_index` and `instance_id` are both 0, which is
  exactly why confusing them survives casual testing.)

### Menu label width — settled, measured

The value column is `TINY_FONT` drawn at x=237 → **83px available**, and the font is
**proportional**, so character count is the wrong metric (the ~6px/char rule of thumb
overestimates badly). Measured with `JE_textWidth(s, TINY_FONT)`:
worst case **`"CIRCLE, SQUARE"` = 67px** (PlayStation labels are the long ones).
For reference the *old* joystick screen already drew `"AX 10-, AX 10-"` at 68px — the
new labels are narrower than what already shipped. **There is no width problem;** don't
shorten the PlayStation labels on suspicion.
