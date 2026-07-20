# THE DAWNING MASTER ENGINE SPECIFICATION

## Purpose
This document is the canonical specification for The Dawning engine and game.

## Vision
Build a seamless space simulation inspired by Star Citizen and Starfield while remaining achievable for an independent team.

## Guiding Principles
- V3 is the canonical engine.
- Favor correctness over feature count.
- Complete vertical slices before expanding scope.
- Data-driven systems.
- Double-precision world simulation.
- Modular ECS architecture.
- Deterministic fixed-timestep simulation where appropriate.

## Major Development Pillars
1. Engine Foundation
2. Rendering
3. Asset Pipeline
4. Flight & Physics
5. Character Gameplay
6. Ships & Interiors
7. World Streaming
8. AI
9. Economy
10. Narrative
11. Networking
12. Tooling
13. Production Infrastructure

## Goal Additions (2026-07-19)

- **Generated content via Meshy AI.** Assets are produced through the Meshy API
  rather than hand-authored. Credentials come from `MESHY_API_KEY` in the
  environment; the repository is public and must never contain the key.
- **Fully realized interiors.** Every boardable ship and station has a walkable
  interior continuous with its exterior, in the same double-precision world. No
  loading screens, no separate interior origin.
- **Realistic graphics.** Photorealism is binding, not aspirational. The DXR path
  is the reference; raster/DXR divergence is a defect.

See `ASSET_PIPELINE_SPEC.md` for the staged plan. Phase 2 is the current phase
and it blocks every phase after it, because the engine cannot load a mesh from
disk today.

## Scope Improvements
- Ship physicalization with modular subsystems.
- Complete station-to-space-to-station gameplay loop.
- Offline asset compiler and editor.
- Automated testing and CI.
- Canon terminology enforcement.
- Performance budgets for CPU/GPU.
- Clear separation of Engine, Game, Tools, Data.

## Acceptance Criteria
Each subsystem is considered complete only when:
- Integrated into gameplay.
- Tested.
- Profiled.
- Documented.
- Serializable where appropriate.
- Debuggable in-engine.

## Long-Term Roadmap
Phase 0: Repository, CI, tests.
Phase 1: Engine correctness.
Phase 2: Asset pipeline.
Phase 3: Rendering polish.
Phase 4: Ship combat vertical slice.
Phase 5: Stations and economy.
Phase 6: Planetary gameplay.
Phase 7: Persistent simulation.
Phase 8: Multiplayer.

This specification is intended to be expanded continuously and serve as the single source of truth for future Codex development.
