# Runtime Content Assembly Contract

Date: 2026-07-21
Status: implemented by WS-026
Manifest extension: `.tdcontent`

## Purpose

Stage 4 connects reviewed production content to the live ECS without putting
asset-specific construction back into `App`. One strict text manifest selects a
cooked assembly, maps every typed virtual locator in that assembly to a cooked
model primitive or a contract owner, and supplies the root transform.

```text
reviewed .tdasset.json -> deterministic .tdassembly
source GLB            -> deterministic .tdmodel
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

The application names only `assets/runtime/reference_ship.tdcontent`. Moving a
binding to another cooked model or primitive is a data change, not an `App`
change.

## Manifest Grammar

The first non-comment line is `tdcontent 1`. Required singleton records are:

```text
scene "stable.scene.id"
assembly "relative/path.tdassembly"
root px py pz pitch yaw roll sx sy sz
```

Bindings use typed virtual locators:

```text
visual "visual://locator" "relative/model.tdmodel" primitiveIndex
collision "collision://locator"
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

The manifest is an engine-owned deployment description, not a security
signature. Downloadable content still requires an authenticated package catalog
before admission.

## Ownership And Publication

`BeginLoad` records model uploads but creates no entities. Each unique cooked
model is loaded once and each locator receives a stable 1-based owner token.
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

Collision, navigation, and walkable bindings currently prove typed catalog
preflight only. They do not create physics shapes, navmesh instances, pressure
volumes, interaction state, or animated doors. Runtime LOD selection is also a
later stage.

## Verification

CPU tests cover strict parsing, unsafe paths and locators, resource limits,
exact assembly coverage, immutable owner sealing, typed identity validation,
and stale-token rejection. The smoke harness requires these exact live markers:

```text
runtime_content_prepared=ok scene=ship.reference.runtime bindings=21 models=1
runtime_assembly_committed=ok asset=ship.reference.fighter modules=3 moving_parts=2 entities=6
```

Raster, stable DXR, and full-quality DXR must all pass. A GPU-validation run
checks the same startup, rendering, and teardown path under the D3D12 validation
layer.
