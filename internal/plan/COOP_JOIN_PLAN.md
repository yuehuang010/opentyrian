# COOP_JOIN_PLAN.md — drop-in co-op in the single-player campaign

Status: **implemented through C2, plus a device-choice menu on top** (C0
`f71f624`, C1 `71c3f17`, C2 — see git log). Full join → fly → drop/leave →
rejoin loop in place, and the join gesture now honors an explicit per-player
device choice (keyboard / mouse / controller N) made in the controller-config
menu, including joining via keyboard. See "Device menu" below. C3
(polish/balance) awaits the user's two-pad playtest. Note vs. the draft:
`inputDevice` joystick values are `3 + pad`, not `2 +` as sketched below.
Builds on [SHIP_MODE_SWITCH_PLAN.md](SHIP_MODE_SWITCH_PLAN.md) (M0–M3, done):
the solo Fighter ⇄ Dragonwing morph is the single-player experience; this plan
adds a second player **joining at the between-level upgrade screen**, flying
the Dragonwing, for the rest of the campaign — and dropping back out.

## Goal (user, 2026-07-17)

> The objective is for a second player to join (between ship upgrade menu)
> while in the campaign. When in 2 player mode, both players enjoy the game.

## Key exploration findings (file:line verified)

1. **`twoPlayerMode` is overloaded to mean "2-P arcade"**, not "two ships".
   Setting it true mid-campaign today would: clamp the menu away from the shop
   every frame (`game_menu.c:338-348`), force infinite power ignoring the
   purchased generator (`tyrian2.c:1293-1296`), make shield regen free
   (`tyrian2.c:1236-1268`), grant arcade respawns (`mainint.c:3726-3766`),
   reroute levels to arcade script sections (`'2'` cmd `tyrian2.c:2706-2719`;
   events 33/45/46/63/64 at `tyrian2.c:4949-5202`), and corrupt saves (see
   below).
2. **The Galaga runtime flip is the proven join/leave template**: collecting
   the 30000-value ball sets `twoPlayerMode = true` and activates `player[1]`
   *mid-level* (`mainint.c:4961-4981`); P2 death flips it back
   (`tyrian2.c:1216-1218`, `mainint.c:3737,3762`). The flight loop, HUD, and
   collision loops all key on the live global per frame — no reinit needed.
3. **P2's flight-relevant state is tiny**: only rear-weapon id (charge-gun
   table), rear power (charge rate; also the `lives` alias), shield, and
   sidekick fields of `player[1].items` are read in 2P flight. Front weapon /
   generator / ship on `player[1]` are inert (armor hard-forced to 10 in
   `JE_getShipInfo`, `varz.c:362`). So P2 can be **derived entirely from P1's
   purchased loadout** — nothing new to buy, store, or save.
4. **Save round-trip is the sharpest trap**: `JE_saveGame` branches on the
   *live* `twoPlayerMode` (`config.c:478,513`) but `JE_loadGame` derives the
   mode from *slot range* (slots 12-22 = 2P, `config.c:529`). Saving a joined
   campaign into a 1P slot today would corrupt the round-trip (P2 items into
   `lastItems`, split `power[]`, read back with 1P semantics).
5. The shop machinery is entirely `player[0]`-scoped (`playeritem_map`
   `game_menu.c:191-205`, cash `game_menu.c:207-231`) and works unchanged if
   the menu clamp is bypassed.

## Architecture

**New global `bool campaignCoop`** (beside `twoPlayerMode`, `config.c/h`):
"this two-player session is the full-game campaign, not arcade". While a
co-op session is active: `twoPlayerMode = true` (so every add-P2 loop works,
per the galaga precedent) **and** `campaignCoop = true` (so each arcade
semantic below is carved out). `campaignCoop` also stays true while P2 is
temporarily out (dead / not yet rejoined), because it marks the *session*.

**Co-op is transient — never persisted.** A joined campaign saves as a plain
1P campaign save (P2's state is derived, so nothing is lost); on load it's a
1P campaign and P2 rejoins at the next upgrade screen. This sidesteps the
save trap entirely: no format change, no slot migration, no new save flag.

### Arcade-gate carve-outs (all inert until campaignCoop is set)

| Site | Today (2P) | With campaignCoop |
|---|---|---|
| Menu clamp `game_menu.c:338-348` | reroutes to arcade menu | keep `MENU_FULL_GAME` (full shop) |
| Power `tyrian2.c:1293-1296` | forced 900 | campaign rule (`power += powerAdd` from purchased generator) |
| Shield regen `tyrian2.c:1236-1268` | free, timer | campaign rule for both players: regen consumes `power` per player regenerated |
| Respawn `mainint.c:3726-3766` | arcade lives/respawn | no respawn: dead player stays out for the level (revived by next level's init); P2 death additionally does the galaga-style `twoPlayerMode = false` drop |
| Difficulty `tyrian2.c:3873` +1 | (new-game only) | no bump on join — campaign difficulty unchanged |
| Script `'2'` `tyrian2.c:2706-2719` | arcade sections | campaign flow (treat as 1P) |
| Events 33/45/46/63/64 `tyrian2.c:4949-5202` | arcade behaviors | campaign behaviors (treat as 1P) |
| Save `config.c:478,513` + autosave slot `tyrian2.c:993,750` | 2P branches, slot 22 | 1P branches, slot 11 — save is a pure P1 campaign snapshot |
| End-of-episode / high-score totals `tyrian2.c:2808-2818`, `mainint.c:2371` | per-player cash lines | keep the 2P per-player display (harmless, informative) |

Known pre-existing oddity found during exploration (fix opportunistically):
the `'b'` autosave script command computes slot 22/11 but always saves slot
11 (`tyrian2.c:2752-2758`).

### Join (at the upgrade/item screen)

- **Gesture (v1): press fire on a game controller not assigned to P1**
  (`inputDevice[0]`). Polled in `JE_itemScreen`'s input loop
  (`game_menu.c:1089-1135`, `controller[j]` already iterated there).
  Keyboard/mouse join is deferred — the mouse shops and the keyboard is
  usually P1; a pad is the realistic second player. On press:
  `inputDevice[1] = 2 + pad index`, arm `coopJoinPending`, draw a
  "PLAYER 2 READY" indicator on the menu. Pressing again un-readies (leave
  before launch).
- **Activation at level launch** (not in the menu itself, so the shop stays
  1P-rendered during the visit): when the level starts with
  `coopJoinPending`, set `campaignCoop = twoPlayerMode = true` and init P2:
  `player[1].items = player[0].items`; rear weapon id/power stay P1's
  purchased rear (charge gun + charge rate derive from it — same rule as the
  solo morph); `sidekick_level = 101, sidekick_series` per sidekick;
  shield from items; `is_dragonwing` is already true for `player[1]`.
  Do NOT touch the `lives` aliasing (`mainint.c:1182`) — campaign co-op has
  no lives.
- **P2 cash**: accumulates in `player[1].cash` during flight; **pooled into
  `player[0].cash` on item-screen entry** (least-invasive option — the shop
  has exactly one buyer, and pooling also makes the save complete).

### Leave

- **P2 death mid-level**: galaga-style `twoPlayerMode = false` (P1 flies on
  solo, morph re-enables automatically since its guard is `!twoPlayerMode`);
  `campaignCoop` stays set; P2 rejoins at any later upgrade screen.
- **At the upgrade screen**: the join gesture toggles — a joined P2 can
  un-ready/leave. Leaving clears `campaignCoop` when P2 is not in flight.
- **P1 death while P2 alive**: level continues (2P loops already handle one
  dead player); both dead → normal campaign death/game-over path. Next
  level's init revives both.

### Interaction with the solo morph

While `twoPlayerMode` is true the morph trigger is unreachable (existing
guard) and P1 is Fighter-mode; `player[0].is_dragonwing` is forced false at
level start already (`tyrian2.c:788-793`). When P2 drops mid-level, P1's
morph becomes available again — intended.

## Implementation phases

- **C0 — carve-outs, zero behavior change** (Sonnet; main-session review):
  add `campaignCoop` (+ reset in `JE_initPlayerData`), thread it through
  every row of the carve-out table as `twoPlayerMode && !campaignCoop` (or
  equivalent), fix the `'b'` autosave slot oddity. All behavior identical
  while the flag is false — attract/2P/1P must be untouched.
- **C1 — join** (Sonnet): item-screen pad polling + READY indicator +
  `coopJoinPending`, activation/init at level launch, cash pooling on
  item-screen entry.
- **C2 — leave/death semantics** (Sonnet): P2 drop on death, un-ready/leave
  at the shop, P1-dead-continues verification, save/load round-trip check
  (save while joined → load → 1P campaign + rejoin).
- **C3 — polish/playtest** (after user feedback): HUD nits in co-op (2P
  panel appears on join — automatic), indicator styling, balance.

## Decisions confirmed / open

- Fighter fires front-only (carried over from SHIP_MODE plan).
- Open for user: join gesture scope (controller-only v1 OK?), P2 death =
  drop-out-until-next-shop (vs. some respawn cost), difficulty unchanged on
  join (vs. arcade's +1), shield regen costing power for both players.

## Device menu (post-C2 follow-up)

C1's join gesture ("press fire on a pad that isn't P1's") worked, but gave
no way to join from the keyboard and no way to pin a specific pad to P2
when more than one is connected. This follow-up adds a menu to choose each
player's device up front, and makes the join/leave gesture honor it.

- **Two new rows in `MENU_CONTROLLER_CONFIG`** (`src/game_menu.c`,
  `controller_rows[]`): `ROW_DEVICE` rows "PLAYER 1" / "PLAYER 2", inserted
  at the top of the table, ahead of `ROW_ANALOG`. Each shows/edits
  `inputDevice[0]` / `inputDevice[1]` respectively (the row's `assignment`
  field doubles as the player index for `ROW_DEVICE`, distinct from its
  `ROW_BINDING` meaning). Value text mirrors the existing 2P-arcade device
  display: `KEYBOARD` / `MOUSE` / `CONTROLLER n` (n = value − 2, only shown
  once more than one pad is connected) / plain `CONTROLLER`. Cycling (fire,
  left, right) mirrors the 2P-arcade device cycler exactly, including its
  uniqueness do-while (`inputDevice[0]` can never equal `inputDevice[1]`,
  and the `controllers == 0` swap-quirk is preserved) — and, unlike every
  other row in this menu, stays usable with zero pads connected, since
  keyboard/mouse cycling doesn't need one. Cycling either row clears
  `coopJoinController` back to `-1`, since a readied pad/keyboard may no
  longer match the newly-chosen device.
- **`COOP_JOIN_KEYBOARD` sentinel** (`-2`, `src/config.h`): `coopJoinController`
  now holds `-1` (none), `COOP_JOIN_KEYBOARD` (keyboard readied), or a pad
  index (`>= 0`). It never collides with a real pad index, so every
  pre-existing `== coopJoinController` pad comparison
  (`src/controller.c` mute check) stays correct unchanged; every `>= 0` read
  gating join/joined state (`src/game_menu.c` indicator,
  `src/tyrian2.c` activation) became `!= -1` to include the keyboard case.
- **Device-aware join/leave gesture** (`src/game_menu.c`, the item-screen
  join poll): the gesture now branches on `inputDevice[1]`:
  - `>= 3` (explicit pad): only that pad (and only while it's connected and
    isn't P1's) may claim/toggle P2 — no other pad reacts.
  - `== 1` (keyboard) and `inputDevice[0] != 1`: the ship-morph key
    (`KEY_SETTING_SHIP_MORPH`) toggles ready state, consumed the same way
    the in-flight morph trigger's key press is consumed elsewhere (cleared
    from `keysactive` so a held key doesn't repeat every frame). The morph
    key is otherwise inert in menus, so there's no double-action risk.
  - otherwise (mouse or "any"): unchanged legacy behavior — any pad that
    isn't P1's claims/toggles P2.
  The joined-at-the-shop leave branch follows the same split (morph key for
  keyboard P2, pad check otherwise).
- **Activation** (`src/tyrian2.c`, level-launch block): if
  `coopJoinController == COOP_JOIN_KEYBOARD`, sets `inputDevice[1] = 1`
  directly (no pad-collision fixup needed) with a defensive fallback if
  `inputDevice[0]` somehow also ended up on keyboard; otherwise the
  pre-existing pad path is unchanged.
- **No new persistence**: `inputDevice[]` already rides the existing
  per-save-slot fields (`saveFiles[].input1`/`.input2`, `src/config.c`), so
  a chosen device survives save/load with zero format changes.
