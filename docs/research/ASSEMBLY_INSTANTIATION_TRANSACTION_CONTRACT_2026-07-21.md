# Assembly Instantiation Transaction Contract

Date: 2026-07-21
Workstream: WS-025
Status: implementation contract

## Purpose

WS-022 loads a complete immutable `.tdassembly` graph. WS-023 resolves every
authored locator to a typed catalog identity. WS-024 supplies immutable catalog
snapshots that translate exact identities to owner-system tokens and retain the
corresponding lifetime anchors.

WS-025 joins those boundaries without making resource lookup or scene mutation
implicit:

```text
CookedAssembly + one catalog snapshot + owner adapter
    -> PrepareAssemblyInstance
       -> resolve through that exact snapshot
       -> validate every owner binding
       -> compute every world transform
       -> publish immutable PreparedAssemblyPlan
    -> CommitPreparedAssembly
       -> stage root/module/moving-part entities
       -> publish AssemblyInstance, or destroy every staged entity
    -> DestroyAssemblyInstance
       -> reverse-order entity teardown
       -> release the retained plan and catalog lease
```

This is the first runtime entity transaction for authored ships and structures.
It deliberately does not pretend that an entity with a mesh is already a fully
simulated, pressurized, navigable interior.

## Resolution Provenance

Preparation accepts a `CookedAssembly`, not an independently supplied
`ResolvedAssemblyResources`. It invokes the shipped WS-023 resolver against the
supplied concrete WS-024 snapshot and retains both results in one plan.

This ordering is load-bearing. Catalog values are scoped operationally to one
store, while the frozen identity type has no catalog UUID. Accepting a resolved
graph and an unrelated snapshot as separate arguments could not prove they came
from the same store if two stores happened to assign identical value,
generation, kind, and digest tuples. Resolving inside preparation removes that
ambiguous caller contract.

Every resolved identity is then translated through the same captured snapshot.
A missing, stale, malformed, wrong-kind, conflicting, or exceptional resolver
result fails before the runtime adapter is called and before scene mutation.

## Owner Adapter

`AssemblyRuntimeResourceAdapter` is the owning-system preflight boundary. It
receives a typed identity and the exact nonzero owner token captured by the
lease.

- visual preparation returns a packed `MeshHandle` and complete ECS material;
- collision validation confirms the collision owner accepts its identity/token;
- navigation validation confirms the navigation owner accepts its binding;
- walkable-surface validation confirms the traversal owner accepts its binding.

The adapter must not mutate the scene. It may populate owner-side caches only if
their lifetime and rollback are independent of scene publication. Unknown
adapter statuses, exceptions, null visual handles, non-finite material values,
or out-of-range roughness/metalness reject the complete preparation.

The immutable plan retains each identity and owner token so later owner-system
commit lanes do not need to infer resources from locator strings.

## Limits Before Allocation

WS-025 checks its entity, binding, and per-module LOD budgets directly against
the cooked graph before invoking WS-023. The nested resolver has its own request,
binding, locator, and diagnostic limits. Both layers must pass.

The default WS-025 ceilings are:

- 250,000 root/module/moving-part entities;
- 1,000,000 typed runtime bindings;
- 32 LOD bindings per module.

Counts use checked unsigned arithmetic. A lower caller budget fails before the
larger resolver allocations or any owner-adapter call.

## Transform Contract

Cooked module transforms are asset-root-local. Preparation converts them to ECS
world transforms once:

1. validate finite root position, quaternion, and strictly positive scale;
2. normalize the root quaternion;
3. reduce authored Euler degrees to a stable single revolution;
4. scale local position by root scale and rotate it by root orientation;
5. add that offset to the double-precision root position;
6. compose root and local rotations;
7. multiply root and local scale component-wise;
8. reject float overflow, zero/negative scale, non-finite output, or a degenerate
   quaternion.

Moving-part meshes currently inherit the owning module world transform. Their
cooked pivot, axis, travel, interaction, and stable indices remain available
through the retained assembly for the later moving-part behavior system.

## Immutable Plan

Only `PrepareAssemblyInstance` can construct a `PreparedAssemblyPlan`. The plan
contains:

- the exact immutable resolved resource graph;
- the exact concrete catalog snapshot and all of its lifetime anchors;
- normalized root transform;
- stable-index ordered module transforms and visual/collision/LOD bindings;
- stable-index ordered zone navigation/walkable bindings;
- stable-index ordered moving-part transforms and visual bindings;
- checked entity and binding totals.

Preparation performs no entity creation. Failure never publishes a plan.

## Entity Commit

`AssemblyEntityTarget` is a narrow, GPU-free entity-write interface. The shipped
`RegistryAssemblyEntityTarget` applies it directly to the production ECS
registry; callers may construct it from `Scene::GetRegistry()`. CPU tests use the
same contract with deterministic capacity and exception injection.

Each target exposes a non-null opaque identity. The committed instance captures
that identity, and teardown rejects a different target without touching either
entity store. The registry target uses the registry address rather than the
temporary wrapper address, so a fresh wrapper around the same live registry is
valid. `CreateEntity` must be atomic: it either returns the unique live entity it
created or leaves no unreported entity behind. Target destruction is no-throw
and idempotent.

Commit allocates the instance and reserves all rollback/mapping storage before
the first entity is created. Each new entity is added to the rollback list
immediately, before assigning any component.

The committed graph is:

- one root with `Transform` and `Name`;
- one entity per cooked module with `Transform`, `MeshInstance`, `Material`,
  `Name`, and `Parent(root index)`;
- one entity per cooked moving part with the same render components and
  `Parent(owning module index)`.

Entity vectors preserve cooked stable-index order. The instance is published
only after all required components and parent links exist.

If creation returns `NullEntity`, the target reports a dead entity, assignment
throws, allocation fails, or plan invariants disagree, commit destroys every
staged entity in reverse order and returns no instance. The target contract
requires no-throw, idempotent destruction so rollback itself cannot be
interrupted.

## Lifetime And Destruction

A live `AssemblyInstance` retains its immutable plan. The plan retains the
catalog snapshot, so replacement/removal in the live catalog or destruction of
the store cannot release resources used by the instance.

`DestroyAssemblyInstance` is explicit because the instance does not own the
registry lifetime. It destroys moving parts in reverse order, then modules,
then the root. Only afterward does it clear mappings and release the plan and
catalog lease. Repeated destruction is a no-op. Already externally destroyed
generational entities are harmless to the registry target. Passing a target for
a different registry returns `InvalidArgument` and leaves the instance alive.

## Explicit Non-Goals

WS-025 does not yet:

- create collision bodies or broad-phase proxies;
- publish navmeshes, walkable polygons, or pathfinding links;
- create pressure volumes or atmosphere boundaries;
- instantiate portal, airlock, console, seat, ladder, elevator, or state-machine
  behavior;
- animate moving-part pivots or apply their collision/nav updates;
- choose LODs at runtime;
- upload, stream, replace, or destroy GPU resources;
- add authored lights, audio, damage, inventory, crew, or networking state;
- automatically register the returned instance in a higher-level world manager.

Those systems must consume stable plan bindings through their own prepare/commit
adapters. They must extend the transaction rather than perform hidden work in
the loader or catalog.

## Verification

Focused CPU tests drive the shipped implementation through:

- all visual, collision, LOD, navigation, walkable, and moving-part bindings;
- root/local transform composition and stable-index mapping;
- real WS-023 resolution through a real WS-024 snapshot;
- production ECS registry commit and explicit idempotent teardown;
- wrong-target teardown rejection without entity mutation;
- catalog owner lifetime across store destruction and through instance teardown;
- adapter rejection, unknown status, runtime exception, allocation exception,
  malformed visual binding, and invalid material;
- deterministic mid-commit assignment failure and entity-capacity failure with
  complete reverse rollback;
- transaction and nested resolver limits before adapter invocation;
- invalid root and authored transforms;
- null assembly/lease and missing catalog resources.

Debug and Release all-target builds and the complete CTest matrix remain
required before integration.
