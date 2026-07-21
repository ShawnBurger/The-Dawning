# Interior Collision and Locomotion Contract

Date: 2026-07-21
Workstream: WS-028
Status: Stage 5B implementation contract

## 1. Product decision

The Dawning owns its physics stack. Runtime collision detection, character and
ship motion, rigid-body integration, constraints, reference-frame transitions,
and gameplay-specific physical rules must remain engine code designed for The
Dawning. PhysX, Jolt, Havok, Bullet, or another general-purpose physics runtime
must not become a required gameplay dependency.

External engines and papers are behavioral evidence, not implementation
dependencies. They are useful for identifying proven invariants and failure
modes; their APIs and internal architecture do not define ours.

This decision supports the end goal: seamless controllable ships with complete
interactive interiors, local artificial gravity, moving doors and machinery,
on-foot traversal, EVA, docking, and large-scale orbital simulation without
forcing those domains through assumptions made for a conventional terrestrial
game.

## 2. Scope of Stage 5B

Stage 5B publishes the first production-shaped interior collision boundary:

- deterministic authored collision source and cooked binary packages;
- authenticated, bounded, path-confined runtime loading;
- exact typed collision ownership in the assembly catalog;
- one immutable assembly-local collision world;
- upright capsule overlap and continuous sweep against authored boxes;
- bounded depenetration, collide-and-slide, grounding, slope classification,
  and explicit step traversal;
- deterministic ordering and tie-breaking;
- Debug, Release, malformed-input, and executable smoke evidence.

Stage 5B does not claim final production geometry, a complete player controller,
dynamic rigid bodies, pressure/atmosphere behavior, navmesh generation, portal-
driven collision updates, networking, or ship-to-world reference-frame
transitions.

## 3. Coordinate and ownership model

The collision world is assembly-local. Shape transforms include each module's
authored local translation, rotation, and positive scale, but exclude the scene
root transform. This is intentional:

1. Interior locomotion can remain numerically stable while a ship moves over
   astronomical distances.
2. A ship's root motion does not require rebuilding static interior collision.
3. Interior poses can later cross ship, station, world, and EVA reference
   frames through one explicit transform boundary.
4. Simulation state and rendering origin rebasing remain independent.

The current capsule is upright in assembly-local positive Y. Arbitrary gravity
and up directions are a later contract extension and must not be smuggled into
Stage 5B through implicit world-space assumptions.

## 4. Cooked collision contract

Source files use the strict `.tdcollision.json` schema:

```json
{
  "schema_version": 1,
  "collision_id": "reference.hull",
  "boxes": [
    {
      "id": "floor",
      "center_m": [0, -0.55, 0],
      "half_extents_m": [2.25, 0.05, 1.0],
      "walkable": true
    }
  ]
}
```

The compiler rejects unknown fields, duplicate IDs, non-canonical IDs,
non-finite values, zero or negative extents, excessive coordinates, and limits
violations. Canonical JSON is hashed as source provenance. Records are sorted
by stable ID before encoding.

The `.tdcollision` binary uses an authenticated 72-byte header containing:

- eight-byte `TDCOLL` magic;
- format version and header size;
- total and payload byte counts;
- SHA-256 of the exact payload;
- reserved bytes that must remain zero.

The payload contains the schema version, collision ID, canonical source hash,
and box records. The loader verifies the header, exact lengths, payload hash,
known flags, finite ranges, sorted unique IDs, and configured limits before
publishing an immutable `CookedCollision`.

## 5. Runtime-content contract

Runtime-content schema 2 makes collision bindings concrete:

```text
collision "collision://hull" "reference_hull.tdcollision"
```

The relative path is syntactically confined by the parser and canonically
confined to the manifest's content root by the host. Navigation and walkable
records remain typed contract bindings until their runtime packages exist.

The host performs this transaction before scene mutation:

1. Parse the manifest and load the authenticated assembly.
2. Verify exact typed-locator coverage.
3. Resolve, deduplicate, and authenticate all cooked collision packages.
4. Build the complete immutable assembly-local collision world.
5. Load visual resources.
6. Register exact content hashes and typed owner tokens.
7. Seal owners and acquire one catalog snapshot.
8. Prepare the assembly instance.
9. Commit ECS entities only after uploads retire.

Collision owner identity uses the cooked payload hash, never a hash of the
locator string. Any missing, corrupt, escaping, unbounded, or unpublishable
package fails the whole pending load and leaves no collision world behind.

## 6. Query semantics

### 6.1 Overlap

`OverlapCapsule` computes the exact distance between an upright capsule segment
and an axis-aligned box. It reports stable shape ID, outward normal,
penetration depth, and surface flags. Results are stable-ID ordered.

### 6.2 Sweep

`SweepCapsule` is continuous for supported boxes. It partitions the capsule-
box distance function at axis-boundary crossings, solves the resulting
piecewise quadratic intervals, and reports the earliest time of impact. It
does not approximate the rounded capsule with only an expanded AABB. Equal
fractions resolve by stable shape ID.

### 6.3 Locomotion

`MoveCapsule` treats requested movement as displacement, not velocity. It:

1. Validates all pose and configuration values.
2. Performs bounded overlap recovery from the deepest contact with stable-ID
   tie-breaking.
3. Sweeps the remaining displacement with skin width.
4. Advances to contact and removes only the inward normal component.
5. Repeats for a bounded number of slide iterations.
6. Attempts a bounded up-forward-down step transaction when a horizontal
   blocker is encountered.
7. Probes downward and reports grounded state only for explicitly walkable
   surfaces within the configured slope limit.

The result reports achieved displacement, final center, grounding identity and
normal, blocked/stepped/depenetrated flags, and iteration counts. Failure never
publishes a non-finite pose.

## 7. Determinism and safety invariants

- Shape IDs are stable: upper 32 bits are module index, lower 32 bits are shape
  index in the canonical cooked package.
- Worlds are immutable after publication.
- All loops that respond to contact are iteration-bounded.
- Input files and runtime world sizes are explicitly bounded.
- Comparisons use stable tie-breakers; container insertion order is not a
  gameplay input.
- Movement is continuous for the supported shape set and cannot tunnel through
  a box during a single query.
- Starting penetration is explicit and bounded; unresolved penetration is a
  failure status.
- Walkability requires both an authored flag and geometric slope acceptance.
- Build or query failure cannot partially mutate the live scene.

## 8. Current geometry limitation

Stage 5B consumes authored boxes. A rotated module's local box is conservatively
published as an assembly-local axis-aligned bound. That is safe against missed
collisions but can create false blocking around rotated detail. Production ship
interiors require the next narrowphase stages:

1. exact oriented boxes and capsules;
2. deterministic convex hulls and compound shapes;
3. static triangle meshes with welded adjacency and one-sided policy;
4. a Dawning-owned broadphase over immutable static cells and dynamic proxies;
5. moving portal/blocker shapes tied to interior state;
6. arbitrary gravity/up orientation and reference-frame handoff;
7. dynamic rigid bodies, contacts, constraints, and character-body coupling.

These extensions must preserve the cooked identity, bounded publication,
stable ordering, and assembly-local ownership established here.

## 9. Acceptance evidence

Required checks for this stage are:

- byte-identical recooking of all three reference packages;
- Python compiler contract tests;
- C++ loader integrity and malformed-payload tests;
- analytic overlap, sweep, corner, slide, slope, step, depenetration, and
  partitioning tests;
- concrete-package assembly publication and missing-resource rejection;
- runtime-content schema, path traversal, and coverage tests;
- Debug and Release all-target builds and CTest suites;
- raster, stable-DXR, full-DXR, and GPU-validation smoke runs with:

```text
[SMOKE] interior_collision_ready=ok packages=3 boxes=12 frame=assembly_local
```

## 10. Primary behavioral references

These references informed behavior only. The implementation remains original
The Dawning engine code.

- NVIDIA PhysX 5.3 Character Controllers: kinematic collide-and-slide,
  capsule preference, contact offset, overlap recovery, and explicit step
  offset. <https://nvidia-omniverse.github.io/PhysX/physx/5.3.0/docs/CharacterControllers.html>
- Jolt Physics `CharacterVirtual`: bounded collision and constraint iterations,
  contact padding, stable contact identity, penetration recovery, and the
  up-forward-down stair transaction exposed by `ExtendedUpdate` and
  `WalkStairs`. <https://jrouwe.github.io/JoltPhysicsDocs/5.3.0/class_character_virtual.html>
- Jolt Physics 5.5.0 source snapshot used for read-only implementation research:
  tag `v5.5.0`, commit `23dadd0e603f1b321142d4c74df07fce85064989`.
