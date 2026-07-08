---
description: plan/STANDALONE_PLAN.md — plan to eliminate the external tyrian21 data requirement (phases S0–S5)
---

# Standalone plan

The standalone (zero-external-data) effort is planned in `plan/STANDALONE_PLAN.md` (created 2026-07-07), companion to [remaster-plan-doc.md](remaster-plan-doc.md) and [remaster-asset-tracker.md](remaster-asset-tracker.md). Strategy: remaster presentational assets (audio S1/S2, remaining art S3 — notably `shapes?.dat` tilesets), bundle game data byte-exact (S4) via a pak VFS fallback in `file.c` (S0), compress/host in S5 (two-tier pak: ~6 MB classic tier committed, HD tier as release artifact). Gameplay data is never recreated — "plays identical" invariant.
