---
title: Front Vulcan Cannon fires left of the ship centerline (stock-data asymmetry)
status: Fixed
component: gameplay / weapon data (src/episodes.c JE_loadItemDat)
affects: classic AND HD (data-level; most visible in HD + the shop weapon preview)
reapply-target-branch: hd-remaster
---

# Front Vulcan Cannon fires left of center

**Status: FIXED — applied in `JE_loadItemDat` (src/episodes.c). Re-verified at
runtime before committing: the recenter loop touched only port 7 (opnum 1),
weapons 221..231, with pre-fix `bx[0]` = -4,-6,-6,-6,-8,-8,-6,-6,-8,-8,-8, and
NOT port 15 — exactly as documented below. `make` + `make debug` (`-Werror`)
clean. The one thing still open is the purely-visual centering A/B on a real
display.** The three-part in-flight/shop compositor batch was committed earlier
(`5241cff`); this note's `episodes.c` Vulcan recenter is now applied too.

## Symptom

In the ship shop weapon preview (and in flight), the **front Vulcan Cannon**'s
shot stream is offset ~4–8px to the **left** of the ship instead of firing from
center. Obvious in HD (4px → ~16 on-screen) and especially in the shop, where the
ship is parked so the offset is static and glaring. Reported alongside a "should
wave" expectation — see "Not a bug" below; the Vulcan has no built-in wave.

## Root cause (confirmed against `tyrian21/tyrian.hdt`)

The front Vulcan Cannon is the **only** front weapon in the stock data whose
barrel offset `bx` is non-zero: `bx = -4` at power 1, growing to `-8` at max.
Every other front weapon (Pulse, Multi, Mega, Laser, Zica, Protron Z, Lightning,
Starburst, Mega Pulse, HotDog) has `bx == 0` (spawns on the ship centerline —
`bx==0` makes the shot-sprite center coincide with the 2x2 ship-sprite center,
both `PX+7`). So the Vulcan alone spawns left of center. This is stock 1995 data,
reproduced faithfully — identical in flight and in the shop.

Verified by parsing `tyrian.hdt` directly (record layout is in `JE_loadItemDat`,
`src/episodes.c`): weapon record = 80 bytes, port record = 82 bytes. Front Vulcan
= **port 7** (`op[0]` → weapons **221..231**), `multi==1` at every power,
`circlesize==0`, `accelerationx==0`. Rear Vulcan = **port 15** (weapons 355..365).

### Not a bug (do not "fix"): the missing wave
The Vulcan has no horizontal oscillation (`circlesize==0`, `accelerationx==0`,
`sx` constant). The "waving" seen in flight is the **moving ship** fanning the
stream; the shop parks the ship (`player[0].x` fixed at 72 in `JE_initWeaponView`,
`game_menu.c`) so there is nothing to fan. Adding a wave would diverge from the
original; leave it.

### Note on the earlier committed fix
`5241cff` also mirrored the shop shot simulator's `shotXM > 100` X-wrap onto the
in-flight one (`simulate_player_shots` in `src/shots.c`). That does **not** touch
the Vulcan (it uses neither `shotXM>100` nor `circlesize`) — it correctly fixes
the **Laser** (`sx=101`) and **Zica Laser** (`sx=120`) shop previews. Keep it.

## The fix to re-apply (src/episodes.c)

Add `#include <string.h>` to the include block, and insert this block in
`JE_loadItemDat` **immediately after the `weaponPort` loading loop** (the
`for (int i = 0; i < PORT_NUM + 1; ++i) { ... }` that ends right before the
`special[]` loop), i.e. after `weapons[]` and `weaponPort[]` are both loaded:

```c
	// Recenter the front Vulcan Cannon's shots. The stock data ships it as the
	// only front weapon whose barrel offset is off the ship centerline (bx =
	// -4..-8; every other front weapon has bx == 0), so its stream fires visibly
	// left of the ship -- obvious in HD and in the shop preview where the ship is
	// parked. Zero the bx of its power-level weapons so it fires from center like
	// the rest. Scoped strictly to the FIRST port named "Vulcan Cannon" (the
	// front one; the rear Vulcan is a later port and its high-power form uses a
	// deliberately symmetric bx spread we must not touch), and only its active
	// firing modes (opnum) so unused op[] slots can't drag in other weapons. The
	// front Vulcan is multi==1 at every power, so bx is a pure offset -- zeroing
	// it changes nothing but the horizontal spawn position. Fails closed: if no
	// such port exists in the loaded data, nothing changes.
	for (int i = 0; i < PORT_NUM + 1; ++i)
	{
		if (strncmp(weaponPort[i].name, "Vulcan Cannon", 13) != 0)
			continue;

		for (int mode = 0; mode < weaponPort[i].opnum && mode < 2; ++mode)
		{
			for (int pw = 0; pw < 11; ++pw)
			{
				JE_word wi = weaponPort[i].op[mode][pw];
				if (wi != 0 && wi <= WEAP_NUM)
					for (int s = 0; s < 8; ++s)
						weapons[wi].bx[s] = 0;
			}
		}
		break;  // only the first (front) Vulcan Cannon
	}
```

## Regression safety (why this scoping)

- **Rear Vulcan (port 15) must NOT be touched**: its high-power form uses a
  deliberately *symmetric* `bx = [-2, +2]` spread; zeroing it would collapse the
  spread. The `break` after the first matched port ("Vulcan Cannon" first appears
  at port 7 < 15) guarantees only the front one is changed.
- Names are space-padded to 30 in the data (`nameLen==30`), so match by prefix
  (`strncmp(..., 13)`), not `strcmp` — `strcmp` against `"Vulcan Cannon"` fails.
- Iterate only `opnum` modes so unused `op[1]` slots (stale indices) can't drag
  in unrelated weapons. Front Vulcan `opnum==1`.
- Only `bx` is modified (spawn x). Velocity/damage/graphics and all other weapons
  are untouched.

## Verification done (before revert)

- `make` and `make debug` (`-Werror`) both compiled clean.
- Temporary `fprintf` inside the loop confirmed at runtime it touched **only**
  port 7, mode 0, weapons 221..231, with `bx0_before = -4,-6,-6,-6,-8,-8,-6,-6,
  -8,-8,-8` — and **no port 15**, no other weapon.
- HD attract-demo ran headless with asserts on, 0 crashes.
- Visual centering A/B still needs a real display (no display in dev env) — see
  [data-dir-flag-is-t-not-d.md](../data-dir-flag-is-t-not-d.md) for run args.

## Caveat / open question

Recentering changes the actual shot spawn position (real gameplay), so it is a
*deliberate* deviation from the original data — the user chose "fix the bug"
over strict fidelity. If strict "plays identical" fidelity is later preferred,
this is the single edit to drop. The rear Vulcan's smaller `bx=-2` single-shot
offset (pow 1–2) was intentionally left alone.
