# Research & Design Docs

Design and research material for The Dawning, recovered from the
`TheDawning_Codex_Handoff_v3_unpacked/` handoff bundle so it is versioned and
greppable instead of living only inside an unversioned archive folder.

## Read this first: these are aspirational, not status

**Nothing in this directory describes what the engine currently does.** These are
design targets, research notes, and specifications — several were written against
V1/V2 codebases that no longer exist. The live engine is roughly 11k lines
implementing a D3D12 raster path and a DXR path tracer; most systems described
here (flight, combat, economy, AI, networking, save/load, terrain streaming,
animation, audio) have **no code at all** yet.

For actual implemented status, read the root `README.md` and `CLAUDE.md` — and
treat even those with care, since they too claim some things ahead of the code.

## What's here

- **`UNIFIED_ANALYSIS_RESPONSE.md`** — the most useful document in this folder.
  A clear-eyed audit that scores subsystems honestly and prescribes a Phase 0 of
  repository/CI/test work before more features. Start here.
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
