# THE DAWNING ASSET PIPELINE SPECIFICATION

Phase 2 of `MASTER_ENGINE_SPEC.md`. Phase 0 (repository, CI, tests) and Phase 1
(engine correctness) are complete. This is the next pillar and it currently
blocks every one after it.

## Why this is the bottleneck

The engine cannot load a mesh from disk. Every mesh in existence today comes
from `GenerateCube`, `GeneratePlane`, or `GenerateSphere` in `src/render/mesh.cpp`.
`assets/textures/` ships a README and nothing else. There is no importer, no
asset format, no content directory, and no tooling.

That single gap gates the entire roadmap. Ships, stations, interiors, characters,
terrain props, and every acceptance criterion in the master spec that says
"integrated into gameplay" all require geometry the engine can load. No amount of
rendering polish moves the project forward while the only thing that can be drawn
is a procedural cube.

## Goal additions (2026-07-19)

Three requirements were added to the project goal and are now binding:

### 1. Generated assets via Meshy AI

Content is produced through the Meshy API rather than hand-authored or sourced
from asset stores. The pipeline is: prompt -> generate -> poll -> download GLB ->
import -> compile -> load.

Credentials: `MESHY_API_KEY`, read from the environment. A local `.env` supplies
it and is gitignored. **This repository is public. The key must never appear in a
tracked file, a log line, an error message, or a commit message.** Tools that
touch it must fail with "MESHY_API_KEY not set" rather than echoing any part of
its value.

Generation costs credits against a real account. Treat generation as expensive
and cache aggressively: every downloaded asset is kept, content-addressed, and
never regenerated for an unchanged prompt.

### 2. Fully realized interiors

Ships and stations are not exterior shells. Every one the player can board has a
walkable interior that connects to its exterior without a loading screen. This is
the master spec's target milestone restated as a hard requirement:

> Walk through station -> board ship -> fly -> fight -> land -> trade -> save/load.

Consequences that must be designed for rather than discovered:

- Interiors and exteriors coexist in one double-precision world. There is no
  separate "interior scene" with its own origin.
- Interior geometry is dense and enclosed, which is the opposite of what the
  current single-cascade directional shadow map is built for. Interiors need
  local lights and their own shadowing strategy.
- Occlusion matters. Rendering a whole station's interior because the player is
  standing in one corridor is not viable.
- Interiors are the primary consumer of modular, kit-based assets: an interior is
  assembled from parts, not generated as one mesh.

### 3. Realistic graphics

Photorealism is a stated goal, not an aspiration to be traded away. Concretely,
what already exists and what it implies:

Present: physically based Cook-Torrance/GGX in both paths, albedo/normal/packed
ORM/emissive maps, linear HDR with a single tone-map resolve, bloom, DXR path
tracing with NEE and VNDF importance sampling, and a directional shadow map.

Required and absent: image-based lighting from real environment maps, area and
local lights, cascaded shadows for large scenes, screen-space or ray-traced
ambient occlusion, anti-aliasing of any kind, temporal upscaling, volumetrics and
atmospheric scattering, and material layering (dirt, wear, decals).

The DXR path is the reference. Where raster and DXR disagree, DXR is right and
the divergence is a defect - that rule has already caught tangent handedness, the
normal transform, and sky evaluation, and it is the main thing keeping the two
paths honest.

## Design constraints inherited from the engine

These are not negotiable and any pipeline design that violates one is wrong:

- **Double precision world.** `ecs::Transform::position` is `Vec3d`. Imported
  assets carry float local geometry, but their placement is `Vec3d`. Never
  narrow an absolute world position before camera subtraction.
- **glTF material convention already matches.** The engine's ORM packing is
  occlusion=R, roughness=G, metallic=B, which is exactly glTF's. Emissive is a
  separate map with a colour and strength multiplier, also matching. The importer
  should therefore be close to a direct mapping rather than a translation layer.
- **Left-handed, +Z forward, CW front faces.** glTF is right-handed, +Y up, CCW
  front faces. The importer must convert, and the conversion must be tested -
  a handedness error shows up as inverted culling or mirrored geometry, both of
  which look like "the asset is broken" rather than "the importer is broken".
- **Texture tables are fixed-size and demo-scaled.** `kMaxRasterTextures = 128`
  with slot 0 the null SRV and slot 1 the shadow map; the DXR side has 64 per
  material channel. Real content will exhaust these immediately. Bindless (SM 6.6)
  is the intended answer and is currently blocked on heap consolidation.
- **No mesh loading means no regression risk, but also no reference.** The first
  importer has nothing to be compared against. Its correctness has to come from
  unit tests over known-good inputs plus visual comparison between the raster and
  DXR paths, which is the only oracle the project has.

## Staged plan

Each stage leaves the build green and is independently verifiable.

**Stage 1 - glTF/GLB import.** Parse GLB, extract positions, normals, UVs,
tangents, indices, and material references. Convert handedness. Produce the
engine's existing vertex format. Unit tests over hand-constructed GLB buffers,
plus a smoke assertion that a loaded mesh reaches the GPU with the expected
vertex and index counts.

**Stage 2 - Meshy client.** A standalone tool: prompt in, GLB on disk out.
Content-addressed cache keyed by prompt plus generation parameters. Never called
from the engine at runtime and never on the smoke path.

**Stage 3 - Asset compiler.** Offline conversion from GLB to a runtime binary
format that loads without parsing JSON. The master spec asks for this explicitly.
Includes texture conversion into the formats the engine already loads.

**Stage 4 - Content directory and manifest.** Data-driven scene definition so
adding an asset does not mean editing `app.cpp`. This is the "data-driven systems"
principle the master spec lists and the point at which the demo scene stops being
hardcoded.

**Stage 5 - Interior kit and the first walkable space.** The vertical slice the
master spec asks for, built from modular parts rather than one generated mesh.

## Acceptance criteria

Per the master spec, each stage is complete only when integrated, tested,
profiled, documented, serializable where appropriate, and debuggable. Added here
because the project has been bitten by it repeatedly:

**Every stage ships a negative test.** An assertion nobody has watched fail is
not evidence. The shadow map probe exists because deleting the entire shadow pass
otherwise broke nothing observable; the ORM and emissive demo content exists
because an unused material path is untested code. Any importer assertion must be
verified by feeding it a deliberately broken asset and confirming it fails.
