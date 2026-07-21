# Authoritative Assembly-Root Presentation Contract

Date: 2026-07-21
Stage: 5E / WS-031
Status: implemented; validation evidence is recorded at closure

## Decision

`AssemblyInstance::RootEntity()` is the sole live identity of the playable
ship. It owns the ship `Transform`, `RigidBody`, `ThrusterSet`, `FlightControl`,
`ReferenceFrameBody`, and `GravitationalBody`. The root is intentionally
meshless. Render meshes belong to the committed assembly module and moving-part
entities.

The interactive `PlayerShip` cube no longer exists. Runtime input, physics,
possession, chase-camera composition, thruster feedback, and on-foot local/world
conversion all resolve through the committed assembly root.

## Frame Ownership

| Data | Authoritative frame | Owner |
| --- | --- | --- |
| Ship translation, rotation, scale | World | Assembly root ECS transform |
| Module closed pose | Assembly local | `PreparedAssemblyModule` |
| Moving-part closed pose | Assembly local | `PreparedAssemblyMovingPart` |
| Moving-part current pose | Assembly local | `AssemblyInteriorRuntime` |
| Static and dynamic interior collision | Assembly local | Interior collision runtimes |
| Render-child transform | World, derived | `AssemblyRuntimeHost` presentation batch |
| On-foot player state | Assembly local | Pilot-possession/on-foot state |

Prepared local poses are immutable. A world pose is never fed back as the next
step's local input. This is the central no-drift rule.

## Composition Law

For each module or current moving-part local transform:

```text
world.position = root.position
               + normalize(root.rotation)
                   * (root.scale * local.position)
world.rotation = normalize(normalize(root.rotation) * local.rotation)
world.scale    = root.scale * local.scale
```

The root translation remains double precision. Scaled local offsets are checked
before narrowing to the float quaternion rotation domain. Every component of
the resulting position, rotation, and scale must remain finite and
representable.

The live playable root requires positive, bounded, approximately uniform scale.
Uniform scale prevents a rotated child under a nonuniform root from requiring a
shear matrix that `ecs::Transform` cannot represent. Generic one-time assembly
publication retains the prior nonuniform-root capability through an explicit
configuration opt-out; the playable runtime host does not.

## Preparation And Initial Commit

`PrepareAssemblyInstance` now records module and moving-part transforms in
assembly-local space. It separately validates that the authored root and local
asset transform can produce a representable initial world transform.

`CommitPreparedAssembly` stages the complete initial module and moving-part
world batch before creating any ECS entity. Only a successful stage is used for
entity assignment. Existing generic root limits are preserved at this boundary,
including finite nonuniform roots outside the stricter playable-ship range.

## Fixed-Step Publication Transaction

The accepted fixed-step order is:

1. `Scene::UpdateSystems` advances the authoritative root physics.
2. `AssemblyRuntimeHost::AdvanceInterior` advances local interaction motion.
3. Dynamic interior collision refreshes only when local interaction state moved.
4. The host validates the root, every child entity, plan counts, stable indices,
   local transforms, and caller-owned scratch sizes.
5. `StageAssemblyPresentation` composes every module and moving part into scratch
   world transforms without touching ECS state.
6. Only after the complete batch succeeds does the host assign all child
   transforms.

If any validation or arithmetic operation fails, no child ECS transform is
published. If local interaction state changed before presentation failed, the
interior and dynamic-collision snapshots are restored. Scratch data may be
discarded because it is never authoritative.

The renderer does not currently traverse `ecs::Parent`, so the host publishes
derived world transforms explicitly. `Parent` remains useful topology metadata;
it is not a hidden second transform authority.

## Interior, Collision, And Possession

Interior sockets, portals, moving parts, static collision, dynamic blockers,
and on-foot controller state remain assembly-local. Root motion therefore does
not rebuild collision data or accumulate large-world translation error.

The camera's world point and direction are converted once through the live root
before an interaction query. `AssemblyInteractionQuery` carries those local
values directly. The previous round trip through the authored root is removed.

Pilot exit and re-entry still use the Stage 5D transaction. Their world camera
poses are composed through the same root that physics moves and presentation
consumes.

## Smoke Isolation

Renderer smoke tests retain a center-stage `SmokeShipProbe` cube because the
GPU draw-record and cascade-blend negative controls depend on its stable framing.
It exists only in `--smoke` runs, owns no gameplay or physics components, and is
never assigned to `m_playerShip`.

Smoke mode also uses that probe as its chase-camera target so renderer test
geometry stays in the established cascade transition bands. This does not alter
interactive behavior: ordinary runs chase the authoritative assembly root.

The opt-in `-Flight` smoke profile proves the separation:

- gameplay input drives the assembly root in decoupled mode;
- a real module moves by the same displacement as the root;
- the renderer camera probe remains isolated;
- legacy raster/DXR renderer oracles retain their signal.

## Rejection Contract

Presentation rejects:

- null or mismatched spans/configuration;
- module or moving-part counts above configured limits;
- nonfinite, nonunit, zero, tiny, negative, oversized, or nonuniform live roots;
- nonfinite, nonunit, nonpositive-scale, or oversized local transforms;
- stable-index or moving-part ownership mismatches;
- missing, destroyed, or recycled root/module/moving-part entities;
- unrepresentable scaled offsets, positions, rotations, or scales.

A rejection cannot transfer gameplay identity to the smoke probe or recreate a
placeholder ship.

## Verification Surface

The CPU suite covers large double translations, rolled/scaled roots, near-unit
root normalization, root A/B/A drift rejection, moving-part motion composed with
root motion, malformed roots, stale topology, invalid locals, arithmetic
overflow, and generic nonuniform-root publication.

The smoke harness asserts:

```text
assembly_root_presentation=ok
player_ship=same
root_mesh=absent
assembly_children=coherent
smoke_camera_probe=isolated
gameplay_identity=assembly_root
```

With `-Flight`, it additionally requires:

```text
assembly_root_motion=ok
hierarchy_motion=coherent
playable_ship=ok
flight_mode=decoupled
```

Closure requires Debug and Release all-target builds and CPU suites,
deterministic cooked-asset inspection, raster, stable-DXR, full-DXR, D3D12 GPU
validation, and manual capture inspection.

## Deliberate Follow-Ons

This stage does not add a general ECS transform hierarchy, center-of-mass
authoring, per-module physical damage, articulated physics, production ship art,
navmesh, pressure, EVA, multiplayer authority, or save-format expansion. Those
remain separate lanes. The next ship-content lane may replace the reference
asset without changing this identity and frame contract.
