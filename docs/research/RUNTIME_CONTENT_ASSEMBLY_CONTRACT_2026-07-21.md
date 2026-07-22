# Runtime Content Assembly Contract

Date: 2026-07-21
Status: implemented by WS-026; collision binding extended by WS-028; runtime
selection and production smoke profiles extended by WS-032
Manifest extension: `.tdcontent`

## Purpose

Stage 4 connects reviewed production content to the live ECS without putting
asset-specific construction back into `App`. One strict text manifest selects a
cooked assembly, maps every typed virtual locator in that assembly to a cooked
model primitive, a cooked collision package, or a contract owner, and supplies
the root transform.

```text
reviewed .tdasset.json -> deterministic .tdassembly
source GLB            -> deterministic .tdmodel
collision JSON        -> deterministic .tdcollision
                                  |
                         versioned .tdcontent
                                  |
                 AssemblyRuntimeHost::BeginLoad
                                  |
             immutable catalog lease + WS-025 prepare
                                  |
                  GPU upload execution and retirement
                                  |
          AssemblyRuntimeHost::CommitAfterUploadRetirement
                                  |
                       live all-or-nothing ECS graph
```

The application accepts a strict content ID through `--content=<id>` and maps it
to `assets/runtime/<id>.tdcontent`. The default remains `reference_ship`. IDs are
limited to 64 ASCII bytes and contain only letters, digits, `_`, or `-`; paths,
extensions, separators, whitespace, drive syntax, and traversal tokens are
rejected before manifest access. Moving a binding to another cooked model or
primitive remains a data change, not an `App` change.

## Manifest Grammar

The first non-comment line is `tdcontent 2`. Required singleton records are:

```text
scene "stable.scene.id"
assembly "relative/path.tdassembly"
root px py pz pitch yaw roll sx sy sz
```

Bindings use typed virtual locators:

```text
visual "visual://locator" "relative/model.tdmodel" primitiveIndex
collision "collision://locator" "relative/collision.tdcollision"
navigation "nav://locator"
walkable "walk://locator"
end
```

`source://` is also valid for an authored primary visual. Blank lines and lines
whose first non-whitespace byte is `#` are ignored. No content other than blank
or comment lines may follow `end`.

## Admission Rules

Parsing is fail-closed and bounded before publication:

- schema version, file bytes, line bytes, string bytes, and binding count are
  limited;
- strings are printable ASCII and embedded NUL bytes are rejected;
- scene IDs use only letters, digits, `.`, `_`, and `-`;
- content paths are relative, forward-slash paths with no root, drive, colon,
  `.` component, or `..` component;
- runtime canonicalization rejects symlink or path resolution outside the
  manifest directory;
- root values are finite, scale is positive, and runtime conversion also checks
  representable float scale;
- duplicate typed locators and wrong locator schemes reject;
- coverage must be exact: every module visual, collision, LOD, zone navigation,
  zone walkable surface, and moving-part visual authored in the cooked assembly
  appears once, with no unauthored binding;
- every visual selects an existing primitive in a verified cooked model.
- every collision binding resolves to an authenticated, bounded cooked package
  before owner registration or scene mutation.

The manifest is an engine-owned deployment description, not a security
signature. Downloadable content still requires an authenticated package catalog
before admission.

## Ownership And Publication

`BeginLoad` records model uploads but creates no entities. Each unique cooked
model and collision package is loaded once and each locator receives a stable
1-based owner token. Visual identities derive from source model content and
primitive index; collision identities use the authenticated collision payload
hash rather than a hash of the locator string.
Catalog identity includes kind, generation, and a SHA-256 content identity. The
owner table is sealed before WS-025 preparation and rejects stale, wrong-kind,
wrong-generation, or unbound access.

Publication is deliberately split. The caller closes and executes the startup
command list, waits for GPU retirement, then calls
`CommitAfterUploadRetirement`. Only then are upload buffers released and the
prepared assembly transaction committed to the live registry. Commit remains
all-or-nothing through WS-025.

Shutdown destroys the assembly graph before retiring the catalog, owner table,
meshes, textures, and descriptors. Partial load and entity-instantiation paths
roll back resources in reverse registration order.

## Shipped Witness

`assets/runtime/reference_ship.tdcontent` covers all 21 typed locators in
`reference_ship.tdassembly`. It currently uses one cooked corridor primitive for
all visuals and commits six entities: one assembly root, three modules, and two
moving parts. This is a lifecycle and ownership witness, not final ship art.

`assets/runtime/frontier_courier_mk1.tdcontent` is the first production witness.
It resolves 61 typed bindings across two cooked models and eight collision
packages, then commits one authoritative root, eight modules, and seven moving
parts. Its collision world contains 88 immutable assembly-local boxes. Seven
closed portal closures add one physical panel and one conservative traversal
guard each; opening every closure removes the seven guards while retaining the
seven translated physical panels. The production smoke profile proves all seven
portals become traversable, then exits the pilot seat, advances an on-foot step,
re-enters the ship, and advances the composed root/child flight hierarchy.

For the reference witness, the three collision bindings publish twelve authored
boxes into one immutable assembly-local interior collision world. Navigation
and walkable bindings still prove typed catalog preflight only. They do not
create navmesh instances or pressure volumes. Runtime LOD selection is also a
later stage.

## Verification

CPU tests cover strict parsing, unsafe paths and locators, resource limits,
exact assembly coverage, immutable owner sealing, typed identity validation,
and stale-token rejection. The smoke harness requires these exact live markers:

```text
runtime_content_prepared=ok scene=ship.reference.runtime bindings=21 models=1
runtime_assembly_committed=ok asset=ship.reference.fighter modules=3 moving_parts=2 entities=6
interior_collision_ready=ok packages=3 boxes=12 frame=assembly_local
```

The reference profile remains the calibrated renderer oracle for exact
fixture-specific material and shadow maxima. Production profiles retain every
live/control/reachability/identity/occlusion/Toksvig assertion that is independent
of that fixture and add exact topology, blocker, possession, and hierarchy
assertions for their own scene.

Raster, stable DXR, and full-quality DXR must all pass for both profiles. A
GPU-validation run checks the same startup, rendering, and teardown path under
the D3D12 validation layer.
