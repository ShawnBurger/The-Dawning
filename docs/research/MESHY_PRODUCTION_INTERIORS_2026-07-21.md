# Meshy Production Assets and Interactive Interiors

Date: 2026-07-21

## Executive decision

Meshy is a source-art generator, not the authority for gameplay topology.

Use it to generate reviewed visual modules and texture candidates. Do not let a
generated mesh define scale, collision, walkability, navigation, pressure,
portals, attachment sockets, interaction anchors, rigid-part pivots, LOD policy,
or whether an interior connects to the exterior. Those facts belong to The
Dawning's versioned assembly manifest and offline cooker.

A ship or structure is production-eligible only when the validator can prove:

- exterior and interior modules both exist;
- every interior module belongs to a gameplay zone;
- every zone has walkable, nav, collision, and pressure data;
- an explicit portal graph reaches every required zone from outside;
- each portal is sealable, navigable, socketed, and controlled by an interaction;
- doors, hatches, airlocks, and elevators have authored states, rigid parts,
  pivots, axes, and travel;
- module scale/axes, provenance, and at least three LODs are explicit.

The reference contract is
`assets/manifests/reference_ship.tdasset.json`; the gate is
`tools/validate_asset_manifest.py`.

## What the official Meshy APIs provide

Meshy's current Text to 3D API is a two-stage workflow: preview creates an
untextured mesh, refine textures an accepted preview. `latest` currently resolves
to Meshy 6. Meshy 6 can preserve its highest-precision triangular result when
`should_remesh` is false, or produce triangle/quad/adaptively decimated output
when remesh is requested. The API can return only GLB to avoid unnecessary
exports, generate PBR maps, generate a 4K base-color map for selected hero
assets, remove baked lighting from base color, and estimate size/origin.

- https://docs.meshy.ai/en/api/text-to-3d
- https://docs.meshy.ai/en/api/changelog

Image to 3D is more shape-specific than text. Multi-Image to 3D accepts one to
four views of the same object and is the preferred generation route after a
design is approved. It supports Meshy 6, PBR, de-lighting, optional HD base
color, remesh/decimation, cardinal thumbnails, and GLB-only output.

- https://docs.meshy.ai/en/api/quick-start
- https://docs.meshy.ai/en/api/multi-image-to-3d
- https://docs.meshy.ai/en/webapp/guides/choosing/generation-method

Meshy also exposes remesh, UV unwrap, retexture, rigging, and animation. Its
automatic animation workflow is oriented toward humanoid/quadruped characters
and preset skeletal motion. It is not an authority for vehicle doors, elevators,
landing gear, turrets, or pressure hatches; those are separate rigid meshes with
engine-authored pivots and state machines.

- https://docs.meshy.ai/en/webapp/guides/choosing/post-processing
- https://docs.meshy.ai/en/webapp/guides/animate
- https://docs.meshy.ai/en/api/remesh

Generation is prepaid and metered. Current pricing lists Meshy 6 text preview at
20 credits and text refine at 10 credits, with Meshy 6 multi-image generation at
20 credits untextured or 30 textured. Pricing is an external policy and can
change, so the client exposes a hard projected-cost ceiling and records actual
`consumed_credits` returned by the task.

- https://docs.meshy.ai/en/api/pricing

Meshy provides an MCP integration for interactive ideation, but the repository
client remains the production source of truth because it supplies content
addressing, request review, linkage to assembly modules, durable provenance, and
CI-testable behavior.

- https://docs.meshy.ai/en/api/ai

## Correct production unit: modules

Never prompt for “a complete ship with a fully functional interior” as one
model. A plausible render does not establish matching wall thickness, door
clearance, watertight pressure boundaries, manifold openings, coherent UV scale,
collision, navigation, or movable-part pivots.

Generate and approve discrete families instead:

- Exterior: hull centerline, wing/engine pods, landing gear doors, ramps,
  docking collars, turrets, damage panels.
- Interior architecture: straight/corner/T corridor modules, pressure doors,
  airlock chambers, ladders, stairs, lifts, cockpit shell, crew rooms, cargo
  bays, engineering spaces, window liners.
- Systems and props: consoles, seats, lockers, pipes, breakers, handles,
  med/galley fixtures, cargo restraints.
- Moving rigid parts: one mesh per door leaf, hatch, lift car, lever, turret,
  gear assembly, and ramp, authored around a known pivot.
- Collision/navigation: simplified authored meshes and volumes, never derived
  blindly from render triangles at runtime.

All ships are longitudinal vehicles. Before concept generation, each hull class
must declare a minimum length-to-beam ratio and an acceptable height-to-beam
band. The hard ordering is `length > beam > height`; the class-specific height
band prevents both pancake hulls and implausibly tall ships. Width-dominant,
saucer, disk, and post-hoc stretched silhouettes fail concept review regardless
of surface quality. Stations and structures use separate proportion contracts.

Use a shared dimensions sheet before generation: grid size, deck height, hull
thickness, corridor width, clear door width, handrail height, docking collar,
seat/console envelope, and player capsule. Multi-view references should use the
same orthographic scale and neutral lighting.

## Five-stage content pipeline

### 1. Design authority

Create orthographic/concept references and the assembly manifest first. Freeze
dimensions, module sockets, zone graph, portals, interactions, and gameplay
clearances before spending generation credits.

### 2. Meshy source generation

Use Text to 3D for exploration and generic props. Use multi-image generation for
approved silhouettes and hero modules. Request Meshy 6/latest, GLB only, PBR,
and de-lighting. Preserve a high-detail master; request remesh only when the
desired topology budget is known. Reserve 4K base color for hero assets that
survive close inspection. Do not accept automatic size as authoritative.

The repository client supports a zero-credit `--dry-run`, a `--preview-only`
geometry review stage, current Meshy 6 request fields, hard credit ceilings,
content-addressed output, assembly/module linkage, downloaded-file hashes, and
task/credit provenance. It intentionally does not issue a paid request during
tests or runtime.

### 3. DCC cleanup

In Blender or equivalent:

- enforce meters, +Y up, +Z forward after importer conversion, and frozen scale;
- repair nonmanifold geometry, thickness, intersections, normals, and UVs;
- separate every moving part and place exact pivots;
- replace generated text/logos and baked highlights;
- enforce texel-density/material-channel rules;
- author collision, walk surfaces, portal/socket empties, and LODs;
- inspect interior/exterior continuity at every hatch/window/ramp.

Generated source is sampled and is not bit-reproducible. The downloaded bytes,
hashes, task IDs, parameters, and cleanup revision are therefore provenance, not
a promise that the API can recreate the same asset later.

### 4. Engine assembly and cook

Validate the `.tdasset.json` graph, import each cleaned GLB, cook render geometry
into `.tdmodel`, and package engine-owned assembly metadata. The runtime package
must preserve module identity, sockets, zones, portals, interactions, rigid
parts, collision/nav references, LODs, and provenance. Flattened render
primitives alone are insufficient for an interactive interior.

### 5. Runtime acceptance

An asset is not production-ready until automated and in-engine checks pass:

- import/cook determinism and source hashes;
- LOD and material budgets;
- collision clearances and no spawn penetration;
- every required zone reachable from outside;
- door/airlock nav-link and pressure-state transitions;
- cockpit possession and ship control transition;
- local-light/shadow/occlusion behavior inside closed spaces;
- raster/DXR material parity and close-range visual review;
- save/load of every interaction and moving-part state.

## Repository status and gaps

Available now:

- GLB/glTF import with handedness conversion, generated tangents, PBR material
  ingestion, source snapshots, limits, and deterministic `.tdmodel` cooking;
- runtime mesh/material/texture loading;
- a credential-safe, content-addressed Meshy text client;
- the versioned interior assembly contract and CI validator added by WS-021;
- deterministic cooked assembly loading, typed resource resolution, immutable
  catalog leases, transactional ECS instantiation, and data-driven runtime
  ownership from WS-022 through WS-026;
- the WS-027 Stage 5A interaction runtime for nearest use, reversible authored
  moving parts, exact-open portal passability, and save-ready interior state.

Still absent:

- preservation of authored nodes/pivots/sockets in `.tdmodel`;
- collision mesh cooking and scene queries;
- navmesh generation/streaming and dynamic portal links;
- player/EVA controllers and boarding/seat possession;
- persistent game-save envelopes containing interior state;
- pressure zones, atmosphere exchange, seals, and decompression;
- interior local lights, clustered light culling, local shadows, and portal/PVS
  occlusion;
- asset budget reports, automated Blender cleanup/export, LOD generation, and a
  visual assembly editor.

## Prioritized implementation roadmap

1. Add collision sources, player capsule queries, and walkable/nav data; consume
   WS-027 portal passability as dynamic nav-link state.
2. Add pressure zones and couple WS-027 closure state to atmosphere simulation.
3. Add cockpit possession, on-foot/EVA movement, and boarding transitions.
4. Add a game-save envelope that atomically contains simulation and interior
   snapshots plus world/gameplay identity.
5. Add clustered local lighting, local shadows, and portal/PVS interior culling.
6. Add deterministic Blender import/cleanup/export scripts and multi-image Meshy
   ingestion after approved reference sheets exist.
7. Build an assembly/editor view only after the data contracts above stabilize;
   the editor should edit authoritative manifests, not invent a second format.

## Credential and cost policy

- Never put API keys in source, prompts, logs, command history, manifests, or
  screenshots. `.env` remains local and ignored.
- Any credential pasted into chat or another durable communication channel must
  be rotated before production use.
- Always run `--dry-run` first and review the content hash, request, linkage, and
  projected cost.
- Use `--preview-only`; spend refine credits only after geometry, silhouette,
  module boundaries, and dimensions are accepted.
- Never generate during engine startup, CMake configure/build, tests, CI, or a
  smoke run.
