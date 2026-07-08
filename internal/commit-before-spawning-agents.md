---
description: "Multi-agent git hazard: uncommitted work gets silently wiped when a later subagent runs destructive git; commit reviewed work first"
---

# Commit before spawning agents

When orchestrating multiple subagents that touch files, **uncommitted changes are
unsafe**. In the Track 2a session, one subagent's `git reset`/`git checkout` (reflog
showed `reset: moving to HEAD`) silently reverted another agent's *unstaged* changes
to `tools/hd_extract.py` — the whole sprite-extraction implementation vanished from the
working tree (only the gitignored `.dat` output survived). It was not recoverable from
git objects (never staged) nor cleanly from transcripts (built via many small `Edit`s).

**Why:** subagents share the same working tree. A destructive git command in one
(often run innocently to inspect state) clobbers everyone's uncommitted work.

**How to apply:**
- **Commit reviewed work before spawning the next round of agents** — a committed diff
  is protected from any later agent's git mishap. Prefer several small commits over one
  big end-of-session commit.
- In agent prompts, **explicitly forbid state-changing git** (`reset`, `checkout`,
  `stash`, `clean`, `restore`, `add`, `commit`) — read-only git is fine. Also forbid
  sub-delegation unless intended (an "implementation" agent quietly spawned its own
  nested agent that did the real writing, which fragmented the transcript).
- If work is lost, the generated *output* may still exist as a byte-exact **oracle** to
  re-implement against (here the `.dat` files let a fresh agent reproduce the source and
  validate md5-identical). See [use-sonnet-agents-for-implementation.md](use-sonnet-agents-for-implementation.md).
