# Research & Design Docs

Design and research material for The Dawning, recovered from the
`TheDawning_Codex_Handoff_v3_unpacked/` handoff bundle so it is versioned and
greppable instead of living only inside an unversioned archive folder.

## Read this first

Most files in this directory are aspirational design targets, research notes,
or historical specifications; several were written against V1/V2 codebases
that no longer exist. The dated audit files are the exception. For current
runtime wiring and validation, start with
`RUNTIME_INTEGRATION_AUDIT_2026-07-21.md`.

The live engine now includes tested flight, gravity, passive orbit, collision,
atmosphere, FTL, relativity, reference-frame, and simulation-snapshot systems in
addition to D3D12 raster and DXR rendering. Combat, economy, AI, networking,
terrain streaming, animation, audio, and production authoring tools remain
design work. For the user-facing baseline, also read the root `README.md` and
`CLAUDE.md`.

## What's here

- **`RUNTIME_INTEGRATION_AUDIT_2026-07-21.md`** — current production call graph,
  wiring matrix, corrections, validation evidence, and residual risks.
- **`RUNTIME_CONTENT_ASSEMBLY_CONTRACT_2026-07-21.md`** - current Stage 4
  manifest grammar, admission rules, immutable ownership, GPU-retirement
  boundary, transactional publication, and explicit limitations.
- **`INTERACTIVE_INTERIOR_RUNTIME_CONTRACT_2026-07-21.md`** - current Stage 5A
  interaction states, nearest-use query, moving transforms, portal gating,
  snapshot atomicity, production wiring, and follow-on boundaries.
- **`INTERIOR_COLLISION_AND_LOCOMOTION_CONTRACT_2026-07-21.md`** and
  **`STATEFUL_INTERIOR_BLOCKERS_AND_ON_FOOT_CONTROLLER_2026-07-21.md`** - current
  Stage 5B/5C assembly-local collision, moving blocker, and fixed-step movement
  contracts.
- **`PILOT_SEAT_POSSESSION_AND_SHIP_ROOT_BOUNDARY_2026-07-21.md`** - current
  Stage 5D seat transaction, exclusive input ownership, live ship-root camera,
  and possession acceptance contract.
- **`AUTHORITATIVE_ASSEMBLY_ROOT_PRESENTATION_2026-07-21.md`** - current Stage
  5E playable-root identity, immutable local-pose model, transactional fixed-step
  child publication, and smoke-probe isolation contract.
- **`FRONTIER_COURIER_RUNTIME_ACCEPTANCE_2026-07-22.md`** - accepted production
  content, traversal, possession, flight, renderer, and cockpit testing-baseline
  evidence for the first boardable ship kit.
- **`HIGH_FIDELITY_SHIP_INTERIORS_AND_COCKPITS_2026-07-22.md`** - binding visual,
  human-factors, asset, state, lighting, UI, atmosphere, resource-network,
  audio, damage, and production acceptance direction for future ship interiors.
- **`MESHY_PRODUCTION_INTERIORS_2026-07-21.md`** - generated-source boundaries,
  modular asset method, provenance requirements, and content acceptance policy.
- **`CODEX_CLAUDE_INTEGRATION_REVIEW_2026-07-20.md`** and
  **`CLAUDE_PUBLISHED_CHANGES_DEEP_DIVE_2026-07-20.md`** — dated collaboration
  and change reviews; useful provenance, not timeless status.
- **`UNIFIED_ANALYSIS_RESPONSE.md`** — the most useful document in this folder.
  A historical audit that scored the earlier renderer baseline and prescribed
  repository/CI/test work. Its recommendations must be checked against current
  source before acting on them.
- **`MASTER_ENGINE_SPEC.md`**, **`FULL_3D_SPACE_SIM_ROADMAP.md`**,
  **`TheDawning_MASTER_INDEX.md`** — scope and roadmap framing.
- **`TheDawning_Batch*_Deep_Dive.md`** (13 files) — per-domain research: orbital
  mechanics, physics/collision, ship flight model, D3D12 rendering, audio,
  combat, economy, networking, AI behavior trees, ECS architecture,
  save/serialization, UI/UX, animation, crafting, story/quest, terrain streaming.
- **`TheDawning_Session*_*.md`** (4 files) — topology, shader recipes, creature
  anatomy, particles.
- **`README_FOR_CODEX.md`**, **`UNIFIED_ENGINE_ANALYSIS.md`**,
  **`TheDawning_V2_Improvement_Guide.docx`** — older context.

## What was deliberately left out

- `CLAUDE(1).md` — the **V2** context file. It instructs agents to read it at the
  start of every session and then describes a `src/` tree (`physics/`, `ship/`,
  `gameplay/`, `audio/`, `ui/`, `npc/`, `world/`, `net/`, `nav/`, `procgen/`) that
  does not exist in V3, plus a different terminology rename map than the root
  `CLAUDE.md`. Committing it would actively mislead both humans and agents.
- `CONVERSATION_CONTEXT(1).md`, `DC_HANDOFF(1).md` — byte-identical duplicates of
  the files already at the repository root.
- ~31 `SpaceEngineD3D12_*.zip` / `TheDawning_*.zip` snapshots (69 MB). Historical
  build archives; several pairs are byte-for-byte identical. Kept out of git
  deliberately — see `.gitignore`.
