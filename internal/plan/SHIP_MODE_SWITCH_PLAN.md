# SHIP_MODE_SWITCH_PLAN.md — single-player Fighter ⇄ Dragonwing mode switch

Status: **implemented** (M0 `78905fc`, M1 `ccf2cf3`, M2 `dff2ddb`, M3 — see git
log). Awaiting user playtest for visuals/feel; balance pass (charge rates,
lockout length) deliberately deferred until after playtest.

## Goal

In the single-player campaign there is no player 2, so the Dragonwing (P2's
charge-cannon ship) is unreachable content. Redesign single player so the one
ship can **switch between two modes** in flight:

- **Fighter mode** — today's single-player behavior: front + rear weapons
  fire together, normal ship sprite.
- **Dragonwing mode** — P2's behavior: the charge cannon (hold fire released
  → charged blast, level builds while not firing), Dragonwing twin-hull
  sprite, sidekick behavior as in 2P.

"Plays identical" rule from the remaster does **not** apply here — this is a
deliberate gameplay redesign — but 2-player mode must remain byte-for-byte
identical in behavior.

## Why this is cheap (exploration findings)

Full maps: agent reports summarized here; key facts verified at the cited lines.

1. **Dragonwing = player index, not ship type.** `is_dragonwing` is set as
   `(p == 1)` (`mainint.c:1142`); in-flight forks key on `playerNum_ == 2`
   (charge cannon `mainint.c:4474-4529`, sidekick ownership `mainint.c:4662`,
   pickup routing `mainint.c:4851+`) and on the sprite sentinel
   `shipGr_ == 0` (twin-hull draw layout `mainint.c:4323-4327`, sidekick
   positioning `mainint.c:4380`).
2. **`player[0]` already owns both weapon ports.** The full-game shop edits
   `player[0].items` with FRONT and REAR slots (`player.h:37-58`,
   `game_menu.c:188-202`). The Dragonwing's actual guns are *derived from the
   rear-weapon id*: `chargeGunWeapons[rear.id-1] + chargeLevel`
   (`mainint.c:4521`, tables `varz.c:100-105`). So the rear weapon the player
   already buys doubles as the charge cannon — **no second inventory, no shop
   redesign, no save-format change.**
3. **Firing bays don't collide.** `shotRepeat[]`/`shotMultiPos[]` are global
   but indexed by distinct `SHOT_*` bays (`config.h:105-120`); Fighter uses
   `SHOT_FRONT`/`SHOT_REAR`, Dragonwing uses `SHOT_P2_CHARGE`. The charge
   state (`chargeLevel/chargeWait/chargeMax/chargeGr`, `varz.c:296`) is a
   single global set — fine, only one Dragonwing exists at a time; reset on
   switch.
4. **Everything outside the flight loop keys on `twoPlayerMode`,** which stays
   `false`: HUD panel choice (`tyrian2.c:817`, `hd_hud.c:660`), collision/
   target loops (`tyrian2.c:1822`, `mainint.c:3185`), episode-script gates
   (`'2'` at `tyrian2.c:2703`), save-slot ranges (`config.c:512`). None of it
   needs to change.

## Architecture decision: mutate `player[0]`, do NOT activate `player[1]`

Two shapes were considered:

- **(a) Chosen: one `Player` (`player[0]`), toggle its mode.** Flip
  `player[0].is_dragonwing` + the ship-graphic argument at the call site. All
  per-player state (position, armor, shield, cash, sidekicks) is naturally
  continuous across the switch because it's the same struct.
- **(b) Rejected: activate `player[1]` and "possess" one ship at a time.**
  Every 2-player loop (collision, enemy targeting `tyrian2.c:552-569`,
  shield regen, HUD bars, save slots, `all_players_dead()`) would need a
  "present but same ship" special case; the `lives`-aliases-
  `items.weapon[p].power` trap (`mainint.c:1143`) and per-player cash/armor
  split logic make this strictly worse. No benefit — mode (a) reaches the
  identical behavior through `is_dragonwing`.

## Design

### Mode semantics

| | Fighter | Dragonwing |
|---|---|---|
| Sprite | `ships[ship].shipgraphic` | `shipGr_ = 0` twin-hull (sprites +13/+51) |
| Main fire | **front port only** (decided 2026-07-17; the old 1P both-ports loop at `mainint.c:4443-4471` goes away) | charge cannon from rear-weapon id (`SHOT_P2_CHARGE` path) |
| Charge level | n/a (reset to 0) | builds while not firing, `chargeMax = 5` |
| Sidekicks | fire as today (`!twoPlayerMode` branch, `mainint.c:4662`) | fire, dragonwing-style positioning (`mainint.c:4380`) |
| Purple-ball power-up target | FRONT port | REAR port (via `is_dragonwing`, `player.c:60`) |
| Rear port-config cycling (change-fire) | works as today | disabled (charge cannon has no port configs) |
| Armor/shield/position/cash | shared — same `Player`, carries across | shared |

The trade the player makes: sustained twin-port DPS vs. burst charge damage.
Numbers (charge rate, whether Fighter keeps the rear port at all) are balance
knobs to tune in playtest, not architecture.

**Not included:** the linked-turret mechanic (`twoPlayerLinked`,
`linkGunDirec`, `mainint.c:4019-4226`) — it's inherently two-ship; stays
2P-only.

### Mode state & availability

- New field `Uint8 ship_mode` isn't needed — **`player[0].is_dragonwing`
  becomes the live mode flag** (it already routes power-ups and sidekick
  behavior). Rename hazard: it's documented as "i.e., is player 2"
  (`player.h:66`); update the comment, and keep `player[1].is_dragonwing`
  permanently true.
- Availability: **on by default in the 1-player full game and 1-player
  arcade** (`!twoPlayerMode` flight paths), no new menu entry. A
  config-file bool (`opentyrian.cfg` custom section, like `highfps`) can
  disable it. Since mode is transient flight state and the feature doesn't
  fork the campaign, **saves need no new flag and no format change** (the
  109-byte fixed-slot assert at `config.c:902` stays untouched). Level start
  always begins in Fighter mode.

### Switch input

- **Controller:** the assignment table (`controller.h:52`) already reserves
  action slots beyond the 4 exposed in the remap screen
  (`game_menu.c:131-134`); claim an unexposed slot ("Ship Morph") and add a
  row to `controller_rows[]`. Defaults: suggest left-stick click or D-pad-
  unused button; decide at implementation with the remap screen.
- **Keyboard:** add a 9th `KeySettings` slot (`config.h:48-58`). **Trap:** the
  DOS-compat save block serializes `dosKeySettings[8]` fixed-size
  (`config.c:263-264,754,821`) — keep that block at 8 and persist the new key
  in the OpenTyrian custom config section instead, so neither save nor cfg
  format breaks.
- Not reusing "change fire": it already means rear-port-config cycling in 1P
  (`mainint.c:4408`) and special-toggle in Super Arcade
  (`mainint.c:4413-4426`); overloading it would collide in both.

### Switch rules

- Allowed while alive and not exploding; ignored in `twoPlayerMode`,
  `galagaMode`, and Super Arcade/SuperTyrian (those modes own their own
  special mechanics; SA already uses a toggle idiom at `mainint.c:4413`).
- On switch: reset `chargeLevel = 0`, `chargeWait`, and the outgoing mode's
  `shotRepeat` bays; brief global fire lockout (~0.5 s via `shotRepeat`) so
  switching isn't a free reload; optional white-flash morph (reuse the
  invulnerable-blend draw variant, `mainint.c:4271-4320`) — cosmetic,
  phase 3.
- `weapon_mode` (rear port config) is preserved across switches.

### HUD

`twoPlayerMode` stays false → both classic and HD HUDs already render the
correct 1P panel with front+rear power bars and sidekick gauges; nothing
breaks with zero HUD work. Additions (phase 3):
- A small mode icon/glyph near the power bar (HD: `hd_hud.c` flight overlay;
  classic: 1P panel free pixels).
- In Dragonwing mode, hide/grey the rear port-config buttons
  (`JE_drawPortConfigButtons`, `mainint.c:210-224`; HD twin at
  `hd_hud.c:708-736`) and optionally draw the charge level (the in-world
  charge sprite above the ship, `mainint.c:4478`, already shows it — HUD copy
  is nice-to-have).

## Known latent trap (found in M1 audit, deliberately not fixed)

`mainint.c:4133` — `if (!twoPlayerMode || shipGr2 != 0)  // if not dragonwing`
reads the **global** `shipGr2`, so in single player it always takes the
"not dragonwing" path regardless of `player[0].is_dragonwing`. Benign today:
the correct dragonwing sidekick placement at `mainint.c:4380` runs later in
the same call and overwrites the position. If that ordering ever changes,
style-0 sidekicks will misplace in 1P Dragonwing mode — the real fix is to
key it on `this_player->is_dragonwing && shipGr_ == 0` like 4380.

## Implementation phases (delegate per CLAUDE.md agent policy)

- **M0 — mechanical discriminator refactor, zero behavior change** (Sonnet;
  Opus review — correctness-sensitive):
  replace in-flight `playerNum_ == 2` dragonwing forks with
  `this_player->is_dragonwing` (charge cannon, sidekick ownership/positioning,
  pickup routing at `mainint.c:4443/4474/4662/4380` + `JE_playerCollide`),
  and the hard-coded `player[1]`/`player[twoPlayerMode?1:0]` weapon reads
  (`mainint.c:4521`, `JE_portConfigs` `varz.c:1106`) with `this_player`.
  2P must be provably unchanged (both builds compile `-Werror`; attract-mode
  + manual 2P smoke).
- **M1 — the switch itself** (Sonnet): toggle function (resets, lockout,
  eligibility guards), ship-graphic selection at the
  `JE_mainGamePlayerFunctions` call site (`mainint.c:4798-4802` — pass 0 vs
  `shipGr` per mode; audit uses of the *global* `shipGr` vs the param inside
  `JE_playerMovement`), Fighter-mode fire keeps the existing
  `min=1,max=2` both-ports loop, Dragonwing-mode routes to the charge path.
- **M2 — input plumbing** (Sonnet): controller slot + remap row, 9th keyboard
  binding in the custom cfg section, config toggle bool.
- **M3 — polish** (Sonnet): HUD mode icon, port-config-button hiding, morph
  flash/sfx, balance pass on charge rates.

Verification: headless attract mode never presses the new button, so M1+
needs real-video manual play; lldb ignore-count recipe (CLAUDE.md) for the
charge path. The M0 refactor *is* attract-testable (2P demo unaffected).

## Decisions

1. ~~**Fighter mode fires the FRONT port only** (user decision, 2026-07-17).~~
   **REVERSED after playtest (user decision, 2026-07-18): Fighter mode fires
   both ports, exactly like vanilla single player.** The front/rear split now
   applies only in two-player mode (each player owns one port). Dragonwing
   mode still fires REAR-only via its charge cannon. Firing condition at the
   `min`/`max` selection in `JE_playerMovement` is now
   `!twoPlayerMode && !is_dragonwing → both ports`. Rear `weapon_mode`/
   port-configs are meaningful again in Fighter flight; the M3 HUD hiding of
   the rear-config buttons applies only while morphed into Dragonwing.
2. **Default-on vs. opt-in config flag** — default-on chosen above.
3. **Switch control defaults** — decide at M2 with the remap screen in front
   of us.
