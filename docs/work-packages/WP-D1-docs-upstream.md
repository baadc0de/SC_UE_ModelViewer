# WP-D1 — Docs upkeep and upstream PRs

**Agent:** Opus 5.5. **Depends on:** WP-R2, WP-R3 landed. **Build lock:** no.

## Goal
Keep this repo's docs truthful and push submodule patches upstream so we stop
carrying them.

## Tasks
- For each `scviewer/*` branch in `vendor/StarBreaker`: squash to a reviewable
  commit series following `vendor/StarBreaker/AGENTS.md` (no trailers, `//!`
  headers, tests first), open a PR on `diogotr7/StarBreaker` from a fork under the
  user's GitHub account (`gh repo fork --remote`), link it in
  `docs/01-starbreaker-analysis.md` under a new "Upstream status" table.
- Update `docs/02-implementation-plan.md` milestone table with actual dates and
  any decision changes; record every decision reversal with the reason.
- Keep `docs/03-unreal-export-design.md` in sync with what WP-U1..U3 actually built
  (the "open questions" section must end up answered).
- Write `docs/04-user-guide.md`: install, open, browse, animate, export to UE.

## Done criteria
- PR links recorded; submodule pointer moves to an upstream commit once merged.
- No doc references a file or command that does not exist (grep-check script in
  `scripts/check_docs.sh`).
