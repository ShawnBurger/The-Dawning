# Assembly Resource Resolution Contract

Date: 2026-07-21
Workstream: WS-023
Status: implementation contract

## Purpose

WS-022 established a deterministic, immutable runtime graph for a ship or
structure assembly. WS-023 converts every resource locator in that graph into a
typed catalog identity before any scene or ECS state is created.

The pipeline boundary is:

```text
.tdassembly bytes
    -> immutable CookedAssembly
    -> transactional resource resolution
    -> immutable ResolvedAssemblyResources
    -> future transactional scene/ECS assembly lane
```

Resolution is all-or-nothing. A caller receives a complete immutable binding
graph or a status and diagnostic with a null graph. There is no partially
resolved public object to clean up or accidentally instantiate.

## Scope

The resolver binds these cooked fields:

| Cooked field | Resource kind | Result mapping |
|---|---|---|
| `AssemblyModule.visualSource` | visual | module index -> visual identity |
| `AssemblyModule.collisionSource` | collision | module index -> collision identity |
| `AssemblyLod.source` | visual | module index + LOD index -> visual identity |
| `AssemblyZone.navmeshSource` | navigation mesh | zone index -> navigation identity |
| `AssemblyZone.walkableSurface` | walkable surface | zone index -> surface identity |
| `AssemblyMovingPart.visualSource` | visual | moving-part index -> visual identity |

The output vectors preserve the cooked vector indices exactly. Duplicate
resource requests can share one catalog identity without collapsing module,
zone, LOD, or moving-part identity.

## Identity And Trust

A locator is a deterministic catalog query key. It is not a resource identity.
The catalog must return all of the following:

- the requested resource kind;
- a nonzero catalog value;
- a generation;
- the SHA-256 digest of the cataloged content.

The tuple `(kind, value, generation, content SHA-256)` is the resolved identity.
The resolver retains the exact `shared_ptr<const CookedAssembly>` and copies its
source-manifest digest, so module provenance and graph identity remain attached
to every binding even where a nested locator has no independent provenance
record.

The resolver rejects a returned kind that differs from the requested kind. It
also rejects one `(kind, value, generation)` reused for different content
digests. Multiple same-kind locator aliases may resolve to one identity only
when the generation and content digest are identical.

## Catalog Contract

`AssemblyResourceCatalog::Resolve` is a const, query-only interface. A production
catalog implementation must:

1. Query an already indexed catalog; do not read or generate source assets in
   the resolver call.
2. Return `NotFound`, `Stale`, or `Error` explicitly instead of manufacturing a
   placeholder identity.
3. Return a content digest for the exact catalog revision represented by the
   value and generation.
4. Keep identical queries stable for the duration of one resolution call.
5. Avoid scene, ECS, GPU, navigation, physics, and gameplay mutation.

The const interface prevents ordinary mutation through the resolver boundary,
but C++ cannot prevent a catalog implementation from using hidden mutable or
external state. Such an implementation violates this contract. WS-023 tests use
a recording fake only to observe canonical call order.

## Determinism

The resolver first validates and gathers all field references. It then sorts
requests by:

1. numeric `AssemblyResourceKind`;
2. unsigned raw UTF-8 locator bytes;
3. locator byte length when one value is a prefix of another.

Exact `(kind, locator)` duplicates are removed. The catalog therefore sees a
stable total order independent of input insertion order, hash-table behavior,
or pointer address. The resolved identities are then mapped back to the original
cooked indices.

Using the same locator for visual, collision, navigation, or walkable data does
not deduplicate across kinds. Each kind is a separate request and a separate
typed output.

## Failure And Bounds

Resolution rejects:

- null assemblies or invalid limits;
- empty, oversized, control-bearing, or malformed UTF-8 locators;
- aggregate bindings or unique requests above configured limits;
- missing, stale, failed, or exceptional catalog queries;
- unknown or wrong resource kinds;
- zero-value or zero-digest identities;
- one typed generational identity associated with conflicting content.

Default limits are one million field bindings, 500,000 unique requests, and one
MiB per locator. Catalog error copying defaults to 1,024 bytes and has a 64 KiB
hard ceiling. Catalog error excerpts are included only when they are printable,
valid UTF-8 and are truncated at a code-point boundary. Locator previews are
capped at 128 bytes under the same boundary rule.

Allocation failure has a distinct status. Catalog exceptions are contained and
reported without publication. Internal construction happens in private local
state; `shared_ptr<const ResolvedAssemblyResources>` is created only after every
lookup, identity check, and stable-index remap succeeds.

## Explicit Non-Goals

WS-023 does not:

- open files or verify that a source path currently exists;
- parse glTF, `.tdmodel`, collision, navmesh, or walkable-surface bytes;
- upload meshes or allocate GPU descriptors;
- create entities, transforms, portal state, pressure state, interactions, or
  navigation links;
- mutate the existing `scene::ResourceManager`;
- promise that a cataloged resource can still be loaded after resolution.

The future runtime-assembly lane needs a concrete catalog adapter with a lifetime
or lease policy, then a separate transactional prepare/commit phase for scene
and ECS creation. A catalog identity can become a `scene::MeshHandle` only in
that later visual-loading bridge. Collision, navigation, and walkable-surface
identities remain owned by their respective runtime systems.

## Verification

Focused CPU tests cover:

- canonical request ordering and per-kind deduplication;
- stable module, LOD, zone, and moving-part index mappings;
- one locator used under several resource kinds;
- valid same-content aliases and conflicting-content rejection;
- not-found, stale, catalog-error, standard-exception, unknown-exception, and
  allocation-failure paths;
- wrong-kind, unknown-kind, zero-value, and zero-digest identities;
- unsafe UTF-8/control locators and aggregate resource limits;
- retained cooked assembly and source-manifest provenance.

The full Debug and Release target and CTest matrices remain required before
integration.
