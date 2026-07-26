# NETWORK_MATCH_PLAN.md — LAN quick match for arcade mode

Status: **design draft** (2026-07-26), discussed and scoped with the user;
not yet implemented.

## Goal (user, 2026-07-26)

> Quick match making system to join any open game. An open game is not while
> in-flight but during the menu between missions. No chat system, keep it
> light. Leave it lockstep for now, but add an option to abandon mission back
> to the menu before the mission start as a safeguard from network issue —
> player won't feel a loss of progress. Final product will be internet, but
> start with LAN for testing.

Scoping decisions (user, same discussion):

- **Arcade mode only** — campaign is explicitly dropped for now. The model is
  arcade-cabinet drop-in: P2 can conceptually join "any time", realized here
  as *between flights only*.
- **Host opts in from the game menu** (the between-mission arcade menu), not
  the title menu. A solo arcade game is not joinable unless the player opens
  it.
- **On abandon/disconnect the host continues solo**; the joiner returns to
  their own title screen. Nobody loses progress.

## Current state (file:line verified)

The existing `WITH_NETWORK` code ([../../src/network.c](../../src/network.c))
is a symmetric peer-to-peer UDP lockstep with a real reliability layer:
ack/retry queues (`packet_out[]`/`packet_in[]`), and an XOR
forward-error-correction scheme over the per-tick input/state packets
(`PACKET_STATE` / `PACKET_STATE_XOR`, `network_delay` deep). Keep all of it.

What blocks the goal:

1. **Networking is a launch-time app mode.** `--net HOST
   --net-player-number N` on both sides; `networkStartScreen()`
   (`tyrian2.c:3449`) *replaces* `titleScreen()` in the main loop
   (`opentyr.c:1096`). No runtime entry.
2. **Every failure is fatal.** `network_tyrian_halt()` (`network.c:709`)
   ends in `JE_tyrianHalt(5)` — timeout, peer quit, even the local ESC
   during connect all **exit the process**. "Back to menu" is impossible
   until disconnect is recoverable.
3. **No discovery.** Peers must know each other's IP; player number is
   assigned by hand. `NET_VERSION 2`, port 1333.
4. **The sync barriers are infinite blocking loops** — `network_connect()`
   (`network.c:641`), the details wait (`tyrian2.c:3499`), the
   between-level `PACKET_WAITING` barrier (`game_menu.c:1937-1971`). These
   are exactly where the abandon option belongs.

Useful existing seams:

- `PACKET_DETAILS` (`tyrian2.c:3479`) already transfers episode+difficulty
  host→joiner at session start — the natural packet to extend with the
  drop-in state snapshot.
- The between-mission menu **exists in arcade mode**: `JE_itemScreen` clamps
  `curMenu` to `MENU_1_PLAYER_ARCADE` (`game_menu.c:438`) — the host's
  opt-in row and the joiner-accept polling live in this loop.
- [COOP_JOIN_PLAN.md](COOP_JOIN_PLAN.md) proved the two patterns this plan
  reuses: the **galaga-style runtime solo⇄2P flip** (no reinit needed;
  everything keys on live globals per frame) and **"ready at the menu,
  activate at level launch"** (`coopJoinPending`).
- 2P arcade P2 state is self-contained: standard arcade loadout, weapons
  from pickups — the join snapshot is small (no shop/save state).

## Architecture

### Session model

- **Host** = player 1. Plays normal 1P arcade. In the between-mission menu,
  a new row toggles the game **open**; while open *and in the menu*, the
  host answers LAN discovery probes and accepts the first compatible
  joiner. The moment the mission-start sync begins (or the row is toggled
  off), the game is closed.
- **Joiner** = player 2. From the **title screen** (the joiner has no game
  running, so their entry point must be the title menu — the user's
  "game-menu, not title-menu" constraint applies to the *host's* opt-in):
  `Join LAN Game` → broadcast probe → take the first compatible offer →
  existing `network_connect()` handshake → wait at the host's menu barrier
  → fly the next mission as P2.
- **In-flight**: unchanged lockstep (`PACKET_STATE` exchange). Not touched
  by this plan.
- **Session end** (abandon, timeout, peer quit): host does the galaga-style
  flip back to solo arcade (`twoPlayerMode = false`, `isNetworkGame =
  false`, socket torn down) and keeps playing; joiner gets a message and
  returns to the title screen. `network_delay` stays a config/CLI knob;
  both sides must agree (already enforced in the connect handshake).

### Discovery protocol (LAN)

New session-less packet pair, outside the ack machinery:

- `PACKET_FIND` (joiner → broadcast:1333): version.
- `PACKET_OFFER` (host → prober, unicast reply): version, host name,
  episode, difficulty, next level — enough for a future browser UI, even
  though quick match just takes the first compatible one.

Compatibility = `NET_VERSION` match (bump to 3 with the protocol changes
below) + joiner has the episode. Stale offers are harmless — the follow-up
`network_connect()` simply fails/times out (non-fatally, post-N0) if the
host closed meanwhile.

**Technical risk — SDL2_net cannot send UDP broadcast.** It never sets
`SO_BROADCAST` and exposes no socket option API, so `SDLNet_UDP_Send` to
255.255.255.255 fails on most platforms. Decision: do **discovery on a tiny
raw-OS-socket shim** (`network_discover.c`, ~100 lines, BSD sockets +
`#ifdef _WIN32` winsock; only bind/sendto/recvfrom/`SO_BROADCAST`,
nonblocking) and keep all *session* traffic on SDL2_net unchanged. The shim
is also the piece that later swaps for the internet rendezvous client.

**Internet later:** only discovery changes — `PACKET_FIND/OFFER` against a
tiny rendezvous service instead of broadcast, plus NAT hole punching (the
existing connect already fires symmetric packets to punch firewalls,
`network.c:598`). Lockstep protocol, session flow, and UI are unchanged.
Design the discovery call surface (`discover_probe()`, `discover_listen()`,
`discover_offer()`) so the LAN backend is swappable.

### Join state snapshot

Extend `PACKET_DETAILS` (host→joiner after accept): episode, difficulty,
**next level (`mainLevel`)**, host score. Joiner runs `JE_initEpisode`,
seeds arcade P2 defaults (same init the 2P-arcade new-game path uses), and
proceeds straight to the between-level barrier — it never sees the earlier
levels. Verify during implementation exactly which globals the mid-episode
arcade start needs (`mainLevel`, `nextLevel`, `saveLevel`, score/lives) by
reading the `'2'`-command arcade script flow (`tyrian2.c:2706`).

Difficulty: **no +1 bump on join** (the host's run keeps its difficulty;
the new-game 2P bump at `tyrian2.c:3477` is a session-start rule, not a
drop-in rule). Open to revisit after playtest.

### Abandon before mission start (the safeguard)

Every pre-flight blocking barrier gets the same treatment: poll input, ESC
→ "Abandon network game?" confirm → send `PACKET_QUIT` (existing packet,
existing peer handling — but the *response* becomes non-fatal) → teardown →
host continues solo / joiner to title. Barriers to convert:

- `PACKET_WAITING` menu-exit barrier, `game_menu.c:1937-1971`
- details wait + final connect sync, `tyrian2.c:3499`, `3524`
- `network_connect()` internal waits, `network.c:641`, `678`

In-flight network trouble still uses today's behavior (lockstep stalls,
timeout) except the outcome is `network_disconnect()` — host flips solo
mid-level galaga-style, joiner exits to title — never process exit.

## Implementation phases

Delegate per phase to Sonnet subagents (repo convention); review invasive
diffs from the main session. Each phase compiles clean (`make` + `make
debug`) and leaves existing modes untouched.

- **N0 — non-fatal, runtime networking** (Sonnet; the enabling refactor):
  `network_init()`/`network_shutdown()` callable at runtime (idempotent,
  frees queues — superset of `network_state_reset`); split
  `network_tyrian_halt` into `network_disconnect(reason)` (message +
  teardown + *return*) with role-dependent continuation; `--net` CLI path
  kept working through it (still useful for testing the session layer
  before N1/N2 exist). Zero behavior change apart from errors no longer
  exiting the app.
- **N1 — LAN discovery shim** (Sonnet): `network_discover.c` raw-socket
  broadcast shim + `PACKET_FIND`/`PACKET_OFFER`; `NET_VERSION 3`. Testable
  standalone with two headless instances on localhost/loopback broadcast.
- **N2 — host open + quick match UI** (Sonnet): host: `NETWORK: OPEN/CLOSED`
  row in the arcade between-mission menu (pattern: the "2 PLAYER" Options
  row, `game_menu.c:276-294`), discovery listener + accept polled from the
  `JE_itemScreen` loop (never blocking), on accept run the connect
  handshake with host=P1/joiner=P2 assigned automatically; joiner: `Join
  LAN Game` title-screen entry → probe → connect → joined-wait screen.
- **N3 — drop-in join/leave semantics** (Sonnet): extended
  `PACKET_DETAILS` snapshot, joiner mid-episode init, activation at level
  launch; disconnect paths → host solo-continue flip / joiner to title;
  in-flight P2 death & quit paths re-verified under the networked flip.
- **N4 — abandon UX** (Sonnet): ESC+confirm in all pre-flight barriers,
  peer notification, message screens ("Player 2 left — continuing solo",
  "Host abandoned — returning to title").

Suggested checkpoints: after N0, two `--net` instances on localhost still
play a full session and ESC/timeout returns to title instead of exiting.
After N2, a real LAN quick match reaches the host's menu barrier. After
N3/N4, the full join → fly → abandon/disconnect → host-continues loop.

## Open decisions

- Player name: add a config item (`network_player_name` already exists as a
  CLI flag); default to system username or "PLAYER".
- Multiple joiners racing: first `PACKET_CONNECT` wins, others get
  `PACKET_BUSY` (packet already defined, currently unused).
- Should the host's open toggle persist across missions of one arcade run
  (stay open after P2 leaves)? Leaning yes — cabinet feel.
- Retire the `--net` CLI path once N2 lands, or keep as a debug door?

## Risks

- **The transport is self-described "HERE BE DRAGONS"** (`network.c:44`).
  Strategy: wrap, don't rewrite — N0 only re-routes its exits and adds
  teardown; the queue/FEC internals stay byte-identical.
- SDL2_net broadcast limitation (addressed by the N1 shim; verify on macOS
  + Windows).
- Symmetric-peer assumptions: `thisPlayerNum` was CLI-set on both ends;
  now assigned by role at accept time. Audit its readers.
- Blocking anywhere in the menu loop would freeze the host's UI — all
  host-side discovery/accept work must be strictly poll-per-frame.
