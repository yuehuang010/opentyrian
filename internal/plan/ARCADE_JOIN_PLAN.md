# ARCADE_JOIN_PLAN.md — one "Arcade" row + drop-in P2 in arcade mode

Status: **implemented** (2026-07-26; A/B/C all landed, awaiting a two-pad
playtest for the controller gesture + disconnect paths). Sibling of
[COOP_JOIN_PLAN.md](COOP_JOIN_PLAN.md) (campaign co-op, implemented) — this
does the same thing for **arcade** mode, where 2P is already a native mode, so
no `campaignCoop`-style carve-out global is needed.

## Goal (user, 2026-07-26)

> Make the Arcade mode to be one line, instead of the 1P and 2P. Once in the
> ingame menu (preflight) add an option for a second player to join (or leave).
> Imagine someone with a controller joining into the game. If the controller
> disconnects, player 2 leaves and it returns back to 1 player game mode.

## Current state (file:line verified)

- **Start-game menu** `gameplaySelect()` ([../../src/menus.c:47](../../src/menus.c)):
  4 rows from `gameplay_name[1..4]` — Full Game, 1P Arcade, 2P Arcade, Network
  (Network is drawn disabled and beeps). Selecting a row sets
  `onePlayerAction = (row == 1P arcade)` and `twoPlayerMode = (row == 2P arcade)`
  (`menus.c:229-230`).
- `newGame()` ([../../src/tyrian2.c:3948](../../src/tyrian2.c)) then branches:
  1P arcade → `player[0].items.ship = 8` (Stalker); 2P arcade →
  `player[0].items.ship = 11` (Silver Ship), `difficultyLevel++`,
  `inputDevice[0]=1, inputDevice[1]=2`.
- **The two arcade modes are two disjoint flags**, never both set:
  `onePlayerAction` (solo arcade) vs `twoPlayerMode && !campaignCoop`
  (2P arcade). ~15 gates spell "arcade" as
  `(twoPlayerMode && !campaignCoop) || onePlayerAction`, so **either flag
  alone keeps every arcade rule true** — this is what makes the runtime flip
  cheap.
- **Preflight menu** = `JE_itemScreen()` ([../../src/game_menu.c:320](../../src/game_menu.c)).
  The clamp at `game_menu.c:433-443` reroutes `MENU_FULL_GAME` to
  `MENU_2_PLAYER_ARCADE` (rows 2..6: next level / P1 device / P2 device /
  Options / Quit) or `MENU_1_PLAYER_ARCADE` (rows 2..4: next level / Options /
  Quit). Row counts come from `menuChoicesDefault` (`game_menu.c:182`), labels
  from `menuInt[curMenu+1][x-1]`, drawn by `JE_drawMenuChoices()`
  (`game_menu.c:2260`).
- **P2 state is already arcade-shaped**: `JE_initPlayerData()`
  (`mainint.c:1104-1153`) initializes `player[1].items` (Vulcan rear,
  `sidekick_level = 101`), `player[1].is_dragonwing = true`, and aliases
  `player[p].lives = &player[p].items.weapon[p].power`. Arcade P2 buys nothing
  and saves nothing — weapons come from pickups.
- **Hotplug**: `controller_device_removed()` (`controller.c:450`) *keeps the
  slot* and only nulls `controller[c].handle` / `instance_id = -1`. So
  "P2's pad is gone" == `controller[p2_pad].handle == NULL`.
- **Existing campaign join machinery to reuse**: `coopJoinEligible()` /
  `coopJoinRowLabel()` / `coopJoinRowActivate()` (`game_menu.c:264-316`), the
  pad-press poll (`game_menu.c:1151-1244`), and the `coopJoinPollActive` gate
  in `push_controllers_as_keyboard()` (`controller.c:264`) that stops a join
  press from leaking into menu navigation.

## Design

### A. One "Arcade" row in the start-game menu

`gameplaySelect()` drops to **3** rows: Full Game / **Arcade** / Network.

- The tyrian.hdt data file ships no "Arcade" string, so the label is a local
  literal, exactly as `titleScreen()` overrides `menuText[4]` with `"Setup"`
  (`tyrian2.c:3551`). Row labels become
  `{ gameplay_name[1], "Arcade", gameplay_name[4] }`.
- Arcade always starts **solo**: `onePlayerAction = true`,
  `twoPlayerMode = false`. There is no longer a way to start directly in 2P.
- `newGame()`'s `twoPlayerMode` branch therefore becomes unreachable from the
  menu. **Delete it** (with it goes the 2P `difficultyLevel++` bump and the
  Silver Ship / `inputDevice` presets) — a mid-session join must not change
  difficulty, and P1 keeps the Stalker it started with. `inputDevice[]` keeps
  whatever the controller-config menu holds.

### B. Join / leave at the preflight menu

**Immediate flip, not ready-at-launch.** Unlike campaign co-op (where the shop
must stay 1P-rendered for the visit), the arcade menu has nothing
player-scoped to break, and flipping right away makes the menu itself the
feedback: the clamp swaps `MENU_1_PLAYER_ARCADE` ⇄ `MENU_2_PLAYER_ARCADE`, so
P2's device-cycler row appears/disappears. `coopJoinController` (the
ready-at-launch latch) is **not** used on the arcade path.

New helpers beside the co-op ones in `game_menu.c`:

- `arcadeJoinEligible()` — `!isNetworkGame && !superTyrian &&
  superArcadeMode == SA_NONE && !campaignCoop && (onePlayerAction || twoPlayerMode)`.
  (Super-arcade and SuperTyrian are single-ship modes; network arcade is out of
  scope until NETWORK_MATCH_PLAN lands.)
- `arcadeJoinRowLabel()` — `"2 PLAYER: LEAVE"` when `twoPlayerMode`, else
  `"2 PLAYER: JOIN"`.
- `arcadeJoinActivate(int pad)` — the flip, both directions:
  - **join**: `twoPlayerMode = true; onePlayerAction = false;` reset P2 to a
    fresh 2P-arcade start (mirror `JE_initPlayerData`'s `player[1]` block:
    `player[1].items = player[0].items` then Vulcan rear id 15,
    `sidekick_level = 101`, `sidekick_series = 0`, both weapon powers 1 — which
    also sets `*player[1].lives = 1` through the alias — `weapon_mode = 1`,
    `armor = ships[player[1].items.ship].dmg`, `cash = 0`,
    `is_dragonwing` stays true). If `pad >= 0`, adopt it:
    `inputDevice[1] = 3 + pad`, and if that collides with `inputDevice[0]`,
    move P1 off it (prefer keyboard, `inputDevice[0] = 1`).
  - **leave**: `twoPlayerMode = false; onePlayerAction = true;`
  - then switch `curMenu` to the matching arcade menu and clamp
    `curSel[curMenu]` into range, since the menu-clamp at `game_menu.c:433`
    only fires from `MENU_FULL_GAME`.
  - `JE_playSampleNum(S_CLICK)`.

**Menu row.** Insert the row into the **arcade menus themselves** (not the
Options submenu, where the campaign row lives) — the user asked for it in the
preflight menu, and it is where a joining player will look. Follow the
existing literal-row pattern (`game_menu.c:2294-2302`): when
`arcadeJoinEligible()`, bump `menuChoices[MENU_1_PLAYER_ARCADE]` 4→5 and
`menuChoices[MENU_2_PLAYER_ARCADE]` 6→7 per frame, insert the literal one row
**above Quit** (so Quit stays last), and shift the `menuInt[]` lookup for the
rows at/after it. Mirror the same index shift in:
- `JE_drawMenuChoices()` (`game_menu.c:2264`), including the
  `MENU_2_PLAYER_ARCADE` extra-`tempY` spacing rules at `2276-2286`;
- the mouse-hotspot mapping (`game_menu.c:2276`-adjacent hit test used by the
  click handler) and the help-text lookup path (`game_menu.c:2717-2748`);
- `JE_menuFunction()`'s `case MENU_1_PLAYER_ARCADE:` / `case
  MENU_2_PLAYER_ARCADE:` (`game_menu.c:3250-3305`) — activate the new row,
  and renumber Quit.

**Pad gesture.** Extend the existing item-screen poll (`game_menu.c:1151`) so
that in an eligible *arcade* session a fire press on a pad that is not P1's
joins (or, when already 2P, a press on P2's pad leaves) — same shape as the
campaign branch, but calling `arcadeJoinActivate()` instead of setting
`coopJoinController`. `coopJoinPollActive` must be set for arcade too, so
`push_controllers_as_keyboard()` swallows the press.

### C. Controller disconnect → P2 leaves

A single helper, e.g. `arcadeP2PadLost()` in `game_menu.c` (declared in a
shared header, or a small `player.c`/`varz.c` home if flight code can't
include `game_menu.h`): true when `twoPlayerMode && !campaignCoop &&
inputDevice[1] >= 3` and the referenced slot is gone
(`p2_pad >= controllers || controller[p2_pad].handle == NULL`).

Checked in two places:

1. **Preflight menu**, once per frame in `JE_itemScreen`'s loop next to the
   join poll → run the leave path (`arcadeJoinActivate` leave branch).
2. **In flight**, in the per-frame update alongside the existing galaga P2
   drop (`mainint.c:3727` / `tyrian2.c:1277` region) → `twoPlayerMode = false;
   onePlayerAction = true;` P1 flies on solo, exactly like the galaga drop.
   Do **not** touch P2's items — a reconnect + rejoin at the next preflight
   menu re-inits them.

## Deliberate behavior changes (call out to the user)

- No 2P difficulty bump any more (was `difficultyLevel++` on a 2P start).
- P1 flies the **Stalker** in 2P arcade, not the Silver Ship — the join can't
  retroactively change the ship P1 started the run with.
- 2P arcade can no longer be started from the title menu; it is always reached
  by joining from the preflight menu.

## Verification

`make` **and** `make debug` clean, then headless smoke
(`SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy timeout 30 ./opentyrian --data ./tyrian21`).
Note `sdl-dummy-video-hides-controllers.md`: the dummy video driver reports no
controllers on macOS, so the **pad** gesture and the disconnect path cannot be
exercised headlessly — verify the menu row (keyboard/mouse activation) that
way and leave the pad paths to the user's two-pad playtest, or use
`SDL_JoystickAttachVirtual`.
