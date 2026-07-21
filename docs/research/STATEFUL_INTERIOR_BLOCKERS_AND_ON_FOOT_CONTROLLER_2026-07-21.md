# Stateful Interior Blockers And On-Foot Controller Contract

Date: 2026-07-21
Workstream: WS-029, Stage 5C

## Decision

Stage 5C connects the authored Stage 5A interior state machine to the custom
Stage 5B capsule collision kernel and adds the first deterministic on-foot
controller. It does not introduce an external physics engine.

The runtime now has three explicit layers:

1. the immutable assembly-local static collision world from cooked module
   packages;
2. an immutable, revisioned dynamic snapshot containing moving closure panels
   and conservative portal guards;
3. a pure fixed-step controller that consumes one published collision snapshot
   and returns a complete proposed player state.

No layer reads render meshes, ECS draw bounds, or frame-to-frame visual deltas.

## Coordinate And Ownership Boundary

All interior collision and on-foot state is assembly-local. This is deliberate:
the runtime-content root may be translated through a large world or reference
frame, while capsule dimensions and door clearances must retain local precision.
World/root conversion belongs at possession and presentation boundaries.

`AssemblyRuntimeHost` owns the static world, dynamic collision runtime, and
authored interior state together. A dynamic snapshot retains a shared lease on
the static world. Publication is transactional: the previous snapshot remains
live if topology validation, arithmetic validation, allocation, or world build
fails.

Shutdown order is controller users first, then dynamic collision, authored
interior state, assembly entities, and resource owners.

## Dynamic Closure Geometry

The current cooked assembly schema has moving-part motion, portal sockets, and
global minimum door dimensions, but does not yet contain per-moving-part
collision shapes. Stage 5C therefore publishes a conservative prototype panel:

- closed center: the interaction's authored portal socket;
- width: `minimumDoorWidthMeters`;
- height: `minimumClearanceMeters`;
- thickness: a bounded runtime constant;
- orientation: the socket's authored forward/up basis;
- motion: the owning moving part's exact authored linear or rotational motion
  reconstructed from its immutable assembly-local closed transform.

The panel remains physical at every progress value, including fully open. A
second stationary guard occupies the portal aperture whenever
`IsPortalTraversable` is false. The guard disappears only at exact authored
`open` state and progress `1.0`; it reappears immediately when closing begins.
This prevents traversal through a visually moving closure even when a panel's
conservative AABB clears the centerline early.

The provisional shape is an explicit limitation, not a production-art claim.
The asset pipeline follow-on must cook per-part convex/compound shapes and
portal aperture geometry. Replacing the prototype source shape must not change
snapshot identity, query ordering, gating, or controller contracts.

## Stable Identity And Query Ordering

Static Stage 5B IDs retain their `(module index, shape index)` encoding.
Dynamic IDs reserve the high bit and encode the stable moving-part index plus a
panel/guard discriminator. Initialization rejects any topology or limit that
could alias these identities.

Each refresh rebuilds a complete candidate list, combines it with the immutable
static boxes, and asks the Stage 5B builder to validate, sort, and publish the
new world. Equal-time hits and equal-depth overlaps therefore retain the
existing lowest-stable-ID rule across static and dynamic geometry.

The snapshot carries the assembly source-manifest digest, an incrementing
revision, and its dynamic boxes. Refresh rejects an uninitialized or different
interior topology. A no-op refresh preserves the revision and pointer identity.

## On-Foot State And Command

The on-foot state contains:

- one upright assembly-local capsule;
- assembly-local velocity;
- grounded state and stable ground shape ID;
- whether jump was held on the preceding accepted fixed step.

The command contains bounded right/forward movement demand, a finite view
forward vector, sprint demand, and jump-down state. `WASD` and arrow keys are
resolved by the existing shared movement binding before this boundary. Mouse
look supplies the view direction; the controller never reads Win32 input.

Planar movement is view-relative. View forward is projected onto the local XZ
plane and normalized, with local right derived from the left-handed +Y-up
basis. Diagonal demand is clamped to unit magnitude. A degenerate view is
allowed only when there is no planar demand.

## Fixed-Step Motion Law

Every step first validates the complete state, command, configuration, time
step, and collision snapshot. It then probes/depenetrates a working copy before
using grounded state, so a stale caller-provided grounded bit cannot authorize
an airborne jump.

Ground and air horizontal velocity approach their target using separate bounded
accelerations. Releasing input uses a bounded braking rate. Sprint changes only
the target speed. Velocity and displacement use an analytic accelerate-to-target
integration, including the exact time at which a speed cap is reached; constant
input in empty space is therefore invariant to subdivision of the same elapsed
time.

Jump is rising-edge triggered and requires the collision probe to report a
walkable ground surface. Holding jump cannot retrigger. Gravity and terminal
fall speed use the same bounded analytic integration. The Stage 5B continuous
sweep/slide/step query resolves the combined displacement, so a closure cannot
be tunneled through by a large permitted step.

After collision, blocked velocity components are derived from achieved motion.
A descending grounded result clears vertical velocity. The returned pose,
velocity, grounded state, jump latch, and ground identity describe one complete
accepted state.

## Failure And Atomicity

The step API is pure: it receives a source state and returns a result. On any
failure, the result repeats the source state byte-for-field and reports a stable
status. Callers commit only successful results.

Rejected conditions include:

- nonfinite or out-of-range state, command, configuration, or time step;
- movement demand outside `[-1, 1]`;
- degenerate view with nonzero movement;
- invalid capsule dimensions or speed/acceleration ordering;
- stale/mismatched dynamic topology;
- malformed socket bases, moving-part ownership, transforms, axes, or travel;
- dynamic shape/combined-world limit overflow or duplicate identity;
- Stage 5B unresolved penetration or query failure.

Authored interaction transitions in the host use rollback. If rebuilding the
dynamic snapshot fails after an activation, advance, or snapshot apply, the
prior authored snapshot is restored before failure is returned. ECS moving-part
transforms are published only after the new collision snapshot succeeds.

## Production Wiring

The default application possession remains the playable ship. Stage 5C does not
fake cockpit exit, teleport the camera, or create an implicit player entity.
The host exposes the live combined collision snapshot and the pure controller
is ready for the explicit possession transition workstream.

Smoke mode drives a deterministic assembly-local capsule toward the closed
outer hatch, verifies it is blocked, opens the authored hatch through the host,
refreshes collision, and verifies the same capsule can cross the former guard.
It emits:

```text
[SMOKE] on_foot_controller=ok closed=blocked open=traversable blockers=2
```

The marker proves host lifecycle, authored state coupling, dynamic publication,
and controller consumption through the shipped application path. CPU tests
provide the detailed dynamics evidence.

## Acceptance Matrix

- closed, opening, closing, and locked portals retain a guard;
- exact open removes only the guard while retaining the moved panel;
- reopening and snapshot restore rebuild the correct blocker set;
- static and dynamic equal-time hits obey one stable ordering;
- no permitted fixed step tunnels through a closed guard;
- movement accelerates, brakes, sprints, and clamps speed;
- view-relative diagonal input is normalized;
- gravity lands, terminal speed clamps, and jump fires once per press;
- airborne jump and stale grounded state cannot jump;
- ground/air control and wall slide preserve finite bounded state;
- constant unobstructed input is invariant to time partitioning;
- invalid input, topology, limits, or unresolved penetration is atomic;
- repeated initialization/shutdown does not retain stale snapshots;
- Debug and Release CPU suites, raster smoke, stable/full DXR smoke, and
  serialized D3D12 GPU validation all pass.

## Required Follow-On

1. Add cooked per-moving-part collision shapes and authored portal apertures.
2. Add explicit ship/on-foot possession around the pilot seat and spawn socket.
3. Parent assembly-local player presentation to the large-world/reference-frame
   root and test moving ships.
4. Add crouch, slope projection, moving-platform velocity inheritance, ladders,
   EVA, and zero-gravity contact movement as separate controller contexts.
5. Couple the same exact-open closure state to navigation links and pressure.
6. Put on-foot state and dynamic interior revision in the atomic save envelope.
