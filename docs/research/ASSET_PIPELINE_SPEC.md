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

## Known scaling limits (measured, not estimated)

Found by instrumenting the engine rather than reading it. They bound how much
real content it can hold today, and the asset pipeline is what will hit them.

**Constant ring: RESOLVED. The ceiling is removed, not raised.**

*Historical, for anyone reading a pre-fix commit:* every shadowed entity used to
cost 768 bytes of the per-frame constant ring each frame - 256 for
`CBPerObject`, 256 for `CBMaterial`, 256 for the shadow pass's copy, each
rounded up to D3D12's 256-byte constant-buffer alignment. Against a fixed 256 KB
`kCBRingSize` that put the ceiling at `(262144 - 256) / 768` = 341 entities, and
a four-cascade shadow pass roughly halved it to about 170. Measured on current
main at 10% of the ring with the demo scene's 17 renderables and 57% with the 97
the smoke growth stress creates - against a harness that fails at 75%.

Per-object and per-material data now live in growable, `kFrameCount`-instanced
structured buffers bound as ROOT SRVs, with the per-draw record indices and
smoke-probe enable supplied as a 3-uint root constant (`b3`). The
view-projection moved to a per-PASS
cbuffer (`b4`), uploaded once per pass rather than premultiplied into every
per-object record. One draw call per entity is unchanged; only where the data
lives changed.

The ring now carries `CBPerFrame` plus one `CBPerPass` per pass - one per shadow
cascade and one for the main pass: **1,792 bytes per frame, FLAT, independent of
entity count** - measured at 1% on both smoke modes, down from 26,624 (raster)
and 149,504 (DXR). Ring occupancy has stopped being a function of scene size at
all, which is why this is a removal rather than a raise.

New cost model, per shadowed entity, per frame slot:

    96 (shadow object record) + 96 (main object record) + 80 (material) = 272 bytes

Across `kFrameCount` = 3 slots that is 816 bytes per entity of persistently
mapped UPLOAD memory. 5,000 entities - a station interior - costs 1.30 MB per
slot, 3.9 MB total. That is not a constraint worth designing around, which is
the point.

Because the view-projection is per-pass rather than per-record, each additional
shadow cascade costs one more 256-byte cbuffer, FLAT, rather than another 256
bytes of ring per entity per cascade. That is not theoretical: the four cascades
already on main share ONE set of object records. `RenderShadowCasters` runs once
per cascade, but `BeginShadowCascade` rewinds the object cursor, which is legal
precisely because `ObjectData` holds only the world matrix and its
inverse-transpose - the cascade-dependent part is the light matrix, and that is
what moved into `CBPerPass`. So object capacity stays at 2x the draw count
rather than becoming (cascades + 1) x, and the cascade count no longer appears
in the per-entity cost model at all.

The remaining constraints are (1) CPU draw-call submission cost, since one
`DrawIndexedInstanced` per entity is deliberately retained - instancing and
merging are the next lever, and this change is a prerequisite for both rather
than an obstacle; and (2) the 128-slot raster texture heap below, which real
content exhausts far sooner. This change spends root DWORDs (8 -> 15 of 64,
including the smoke-only root UAV and probe-enable constant)
precisely so it spends ZERO heap slots and does not make (2) worse.

Note the failure character changed rather than disappearing: from a hard cap at
341 entities to upload memory that grows with draw count and never shrinks. The
75% ring gate in `tools/smoke_test.ps1` is retained, and now doubles as proof
that per-draw traffic has not leaked back into the ring.

**Texture tables: 128 raster slots, 64 per DXR channel.** Fixed size, allocated
from a generational allocator with slot 0 the null SRV and slot 1 the shadow map.
One PBR asset consumes albedo, normal, ORM and possibly emissive, so 128 slots is
roughly 32 materials. Real content exhausts this immediately.

## What real Meshy output actually looks like

Measured from the first generated asset (`corridor_section_05445d3a804f4d42`,
15 credits). These are the things a hand-written test GLB would not have
surfaced, and every one is a requirement on the importer:

- **No TANGENT attribute.** Attributes are POSITION, NORMAL, TEXCOORD_0 only.
  The raster path derives tangents from screen-space derivatives so it copes,
  but the importer cannot assume tangents exist, and anything needing them must
  generate them.
- **32-bit indices** (componentType 5125). `CreateMesh` does have a `uint32_t`
  overload alongside the `uint16_t` one, so this is supported - but
  `Mesh::indexFormat` defaults to `R16_UINT`, so the importer has to select the
  overload and set the format deliberately. glTF also permits 8-bit indices,
  which nothing here accepts and which must be widened on import.
- **`doubleSided: true`.** The engine culls back faces with CW winding. A
  double-sided material rendered under back-face culling loses geometry. The
  importer must either carry the flag through to a no-cull pipeline state or
  explicitly decide to ignore it - silently dropping it produces holes that look
  like a broken asset.
- **`metallicRoughnessTexture` is a single packed texture**, glTF-standard with
  roughness in G and metallic in B. That is the engine's ORM layout with only the
  occlusion channel unused, so it drops in almost directly. Meshy also returns
  unpacked `metallic_0.png` and `roughness_0.png` separately; the packed one in
  the GLB is the one to use.
- **No occlusion and no emissive map.** AO defaults to 1.0. Both engine paths
  already handle absent maps.
- **Scale.** 15,562 vertices and 19,193 triangles for one corridor section, with
  an 8.7 MB GLB and about 12 MB of PNGs. Against the ~340-entity constant-ring
  ceiling above, content of this density reaches engine limits quickly.

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

### Stage 3 cooked model contract

The first runtime format is `.tdmodel`, magic `TDMODEL\0`, format version 1. It
is deliberately an engine-owned format rather than serialized C++ structs. All
integers are fixed-width little-endian values, all offsets and sizes are 64-bit,
and sections begin at 8-byte-aligned offsets. This makes layout independent of
compiler padding and gives the loader enough information to reject overflow,
overlap, truncation, and unsupported versions before allocating model arrays.

The header contains source SHA-256, file size, header size, section count, and a
CRC32 over the entire file with the CRC field treated as zero. Its six required
versioned sections are:

1. metadata and canonical dependency identities
2. primitives, vertices, indices, and bounds
3. glTF PBR materials and texture bindings
4. image metadata and embedded source bytes
5. sampler state
6. texture-to-image and texture-to-sampler references

Dependencies are sorted by URI before serialization. Each records URI, byte
size, and SHA-256; duplicate source references are canonicalized. External
buffer and image URIs are percent-decoded and hashed, and external images are
embedded from those same captured bytes so moving the cooked artifact does not
sever its textures. Dependencies are snapshotted before import and hashed again
after import. External buffers are imported directly from the immutable captured
bytes, so even a change-and-restore race cannot pair model data with a different
dependency identity. Repeated image references are charged each time their
payload is materialized, while external buffer and image snapshots have separate
aggregate budgets. The source file also has a SHA-256 in the header. The offline
compiler rejects outputs that alias the source or any external dependency,
validates memory, writes a process-unique temporary sibling, reloads that
temporary file, and only then atomically replaces the destination. Concurrent
publishers retry bounded Windows sharing conflicts. The loader has configurable caps for file and
section sizes, strings, geometry, tables, dependencies, and embedded images;
the builder enforces the same caps before constructing its sections.

Texture bytes are currently preserved in their source PNG/JPEG/KTX/DDS form,
which the existing engine loaders understand. GPU block compression and mipmap
generation are intentionally not claimed by this slice; they belong in a later
texture-cooking revision and must bump the relevant section version when added.
The runtime now loads `.tdmodel` directly, verifies its layout and integrity,
and feeds the recovered `ImportedModel` through the same D3D12 geometry, PBR
image, material, and entity bridge as source glTF. Raw GLB remains available for
offline tools and development inspection but is not parsed by the executable.
The production corridor `.tdmodel` ships through Git LFS; its raw Meshy source
remains in the ignored content-addressed generation cache.

Production measurements on 2026-07-20:

- corridor section: 15,562 vertices, 19,193 triangles, 9,533,104 cooked bytes
- corridor wall prototype: 100,644 vertices, 71,843 triangles, 34,929,120 cooked bytes
- repeated corridor compilation: byte-identical SHA-256
  `ff03a907e3c09e64bf4d56434e879955abcf0810a9206cdaff69a17088ec6375`

CPU tests cover known SHA-256 vectors, deterministic canonical serialization,
complete data round trip, whole-file corruption, truncation, bad magic,
unsupported versions, symmetric builder/loader limits, CRC-correct allocation
bombs, atomic preservation, external dependency identity, and invalid source
models, aggregate external snapshot and materialization limits, immutable-buffer
imports, dependency output aliases, and concurrent source changes.
Both production Meshy GLBs compile and reload successfully without D3D12. A
two-process publication stress test produced a valid byte-identical artifact
with no orphan temporary files.

Runtime smoke requires `model_source=cooked` and the corridor's exact primitive,
vertex, index, and decoded-image counts in raster, stable DXR, and full DXR. A
missing or corrupt shipped artifact stops scene initialization, so the clean-clone
content path cannot silently fall back to procedural geometry or an ignored GLB.

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
