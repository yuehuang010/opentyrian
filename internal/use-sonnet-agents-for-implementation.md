---
description: "Delegate implementation to subagents; pick the model tier by task difficulty vs cost (Haiku/Sonnet/Opus/Fable)"
---

# Use subagents for implementation, tier by difficulty

For any non-trivial implementation work (not just the HD remaster), the user wants me to **orchestrate and review, then spawn a subagent to do the coding** — and to **pick the subagent's model tier by task difficulty vs cost**, not default to one model. The tiering is documented in the repo's `CLAUDE.md` → "Agent usage" section.

**Never use Haiku** — `claude-sonnet-5` is the floor. Model by difficulty (`Agent` tool `model:` override): **`claude-sonnet-5`** = the default for standard implementation *and* mechanical grunt work; **`claude-opus-4-8`** for hard/architectural/risky work and for *reviewing* a subagent's invasive diff; **`claude-fable-5`** only when a task genuinely exceeds Opus.

**Why:** User explicitly wants implementations delegated to subagents with the cheapest capable model — matching intelligence to the task keeps cost proportional to difficulty without babysitting quality.

**How to apply:** Start one tier down and escalate on evidence (default Sonnet). Keep orchestration/decisions/integration on my side; hand off tightly-scoped jobs with the design decided up front. The reviewer should out-rank the implementer — review Sonnet's correctness-sensitive diffs from Opus. See [remaster-plan-doc.md](remaster-plan-doc.md) and [remaster-asset-tracker.md](remaster-asset-tracker.md) for remaster scoping.

Note: for precision-critical work the user has accepted direct implementation over delegation (e.g. the HD text-vanish audit) — use judgement.

## Gotcha: agents stalling on "waiting for notifications" (2026-07-10)

Two subagents in a row ended their run with "I'll wait for the background
task / monitor notification" — subagents receive no such notifications, so
they just stop with the work half-done. Prompt subagents to run long
commands as plain FOREGROUND Bash calls with a large explicit timeout
(600000 ms), and say explicitly: "do not background anything; do not wait
for notifications". If an agent still stops early, resume it (SendMessage)
with that instruction rather than respawning.
