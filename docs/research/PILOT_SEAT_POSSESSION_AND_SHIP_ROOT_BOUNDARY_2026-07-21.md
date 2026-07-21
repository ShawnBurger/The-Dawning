# Pilot Seat Possession And Ship-Root Boundary Contract

Date: 2026-07-21
Workstream: WS-030, Stage 5D

## Decision

Stage 5D makes control ownership explicit. The application starts with one
player possessing one ship. The authored pilot seat transfers that possession
to the Stage 5C on-foot controller, and the same seat transfers it back. There
is no implicit free camera, duplicate input consumer, or unowned transition.

The implementation remains engine-native. It adds no external physics or
character-controller dependency. Collision, locomotion, seat state, root
composition, and camera basis all use The Dawning's existing deterministic
contracts.

## Authored Topology

The runtime resolves exactly one `pilot_seat` interaction and exactly one
`pilot_exit_spawn` socket from the cooked assembly. A valid binding requires:

- a nonzero cooked source-manifest digest;
- a `seat` interaction owned by an interior module;
- an interaction socket owned by that same module;
- a `spawn` socket owned by that same module;
- exactly two unique states named `available` and `occupied`;
- no portal or moving-part binding on the seat;
- finite, orthonormal, assembly-local socket frames;
- local +Y-up seat and spawn frames suitable for the upright controller;
- bounded module/socket positions.

The spawn is a floor anchor, not a capsule center. Exit derives the capsule
center by adding radius, vertical half-segment, and floor clearance. This keeps
character dimensions in gameplay configuration and prevents an asset from
silently changing the collision body through an arbitrary center offset.

Duplicate IDs, wrong socket categories, wrong module ownership, non-interior
ownership, malformed bases, wrong state vocabulary, and missing IDs reject the
complete binding before possession initializes.

## Possession State

`PlayerPossessionState` has three contexts:

1. `Uninitialized`: no gameplay input owner;
2. `Ship`: the existing `FlightControl` and `ThrusterSet` own movement;
3. `OnFoot`: one assembly-local capsule and the on-foot command own movement.

The on-foot state retains the accepted collision topology digest and revision,
local velocity, grounding identity, jump latch, and local view yaw/pitch. Ship
state may retain the last on-foot payload, but only the context determines input
ownership. `OwnsShipInput` and `OwnsOnFootInput` cannot both be true.

## Exit Transaction

Exit is staged as a pure operation before the host changes the seat:

1. validate the binding, configuration, source state, occupied seat state, and
   collision snapshot;
2. construct a complete assembly-local on-foot candidate at the authored spawn;
3. bind that candidate to the live collision topology and revision;
4. reject the candidate if a capsule overlap query reports any obstruction;
5. return the complete candidate without mutating the source or seat;
6. ask `AssemblyRuntimeHost` to change `occupied` to `available`;
7. commit the staged player state only after the host reports the exact target
   state.

A blocked spawn, stale topology, invalid collision world, allocation failure,
or rejected host mutation leaves ship possession intact. On successful exit,
all flight demand and thruster throttles are cleared before on-foot input is
accepted.

## Entry Transaction

On-foot use first performs a query-only nearest-interaction selection. This is
important: a nearby door or console wins when it is closer than the seat. The
query cannot cycle the seat while merely deciding what the player targeted.

When the selected target is the pilot seat, entry requires:

- on-foot possession;
- the exact authored `available` state;
- matching player, binding, and collision topology;
- a collision snapshot at least as new as the player state;
- eye-to-seat distance within the configured range;
- view-to-seat dot product at or above the configured facing threshold.

The pure entry operation stages ship possession. The host then changes the seat
from `available` to `occupied`, and only that exact successful mutation commits
the player context. Other selected interactions continue through the existing
host activation transaction.

## Input And Fixed-Step Ownership

`WASD` and arrow keys continue to share the modern movement binding:

- in `Ship`, they produce thrust and strafe demand;
- in `OnFoot`, they produce view-relative planar movement;
- in any other context, ship demand and thruster throttles are cleared.

Mouse counts accumulate between accepted simulation steps. Ship possession
converts them to pointer steering demand. On-foot possession applies them once
to bounded local yaw/pitch at the first accepted fixed step and clears the batch
before any catch-up step. One render sample therefore cannot be replayed across
multiple simulation ticks. `Space` is rising-edge jump through the controller's
accepted-state latch; `Shift` selects sprint.

`F` is the use command. In ship context it requests pilot exit. In on-foot
context it selects the nearest authored interaction, then either requests seat
entry or activates that non-seat interaction. User conditions such as blocked
spawn, unavailable seat, range, and facing reject without terminating the app.
Topology, ownership, and transaction failures fail closed and stop the run.

## Ship-Root Boundary

Interior collision, capsule state, velocity, look angles, seat frames, and
interaction reasoning stay assembly-local. The live ship entity supplies a
double-precision world translation and unit quaternion. Its current prototype
cube scale is render geometry, so the possession root explicitly uses unit
scale.

Public root helpers reject:

- nonfinite translation, rotation, scale, or tolerance;
- nonunit quaternion;
- negative, effectively singular, excessively large, or nonuniform scale;
- unbounded local values and unsafe float-offset conversion.

Local offsets rotate in float only after subtracting or before adding the
double-precision world origin. The world translation is never narrowed.
Point and direction output parameters remain unchanged on rejection.

The on-foot camera derives a local eye, forward, right, and up, then composes
position and basis through the live ship root. `Camera::InitBasis` consumes the
complete orthonormal basis, so a rolled or pitched ship does not force global
+Y camera up. Invalid bases leave the prior camera unchanged.

The interaction runtime currently stores module poses in the authored root
frame. The application therefore maps the live camera through the live root to
assembly-local coordinates, then into the authored root before running the
query. Rigid translation and rotation cancel, preserving the same local range,
facing, and stable nearest ordering while the ship moves.

## Failure And Atomicity

Pure possession operations return the source player state field-for-field on
failure. They do not own ECS entities, authored interaction state, renderer
objects, or input devices. The application follows a stage, host mutation,
commit order so failed validation cannot transfer input and failed seat
mutation cannot publish a staged player.

Stable status names distinguish invalid input, invalid topology, wrong context,
seat availability, blocked spawn, range, facing, topology mismatch, stale
collision, collision-query failure, and internal failure. This surface is
covered by CPU tests and is suitable for later UI and save-envelope mapping.

## Production Wiring

Interactive runs align the prototype player-ship entity's position and rotation
with the authored assembly root. The rendering smoke fixture retains its legacy
center-stage transform because the GPU cascade-transition negative control
depends on that exact witness. Root math is independently tested with translated,
rotated, rolled, uniformly scaled, nonuniform, singular, and planetary-distance
cases.

Smoke startup performs a real seat roundtrip through the shipped cooked asset,
collision snapshot, host mutation, possession state, and camera composition. It
emits:

```text
[SMOKE] pilot_possession_ready=ok context=ship seat=occupied spawn=pilot_exit_spawn
[SMOKE] pilot_possession=ok exit=on_foot reentry=ship seat=occupied root=composed
```

The harness asserts both markers and the individual transition/root fields.

## Acceptance Matrix

- cooked source and artifact contain the exact spawn socket;
- valid authored seat/spawn topology resolves deterministically;
- malformed, duplicate, missing, cross-module, or wrong-type topology rejects;
- exit produces the exact capsule and collision identity;
- obstruction prevents exit and preserves ship ownership;
- entry requires nearest selection, availability, range, facing, and topology;
- rejected transitions preserve every player field;
- exactly one gameplay context owns input;
- raw look input is consumed once per accepted fixed step;
- ship demand and exhaust clear on exit;
- local/world points and directions round-trip at planetary translations;
- rolled roots preserve camera local up;
- invalid roots and camera bases do not mutate outputs;
- Debug and Release CPU, CTest, raster, stable-DXR, full-DXR, artifact, and GPU
  validation gates pass.

## Deliberate Limitation And Required Follow-On

The live player ship is still a prototype cube, while the instantiated module
and moving-part render entities retain their authored world transforms. Stage
5D makes possession and camera movement correct around the live logical root;
it does not falsely claim that the production presentation hierarchy follows
that root.

The next isolated lane must:

1. make one production assembly root the authoritative ship entity;
2. preserve immutable module/moving-part local transforms;
3. compose every presentation transform from the live root each frame or fixed
   publication point without drift;
4. connect the rigid body, thrusters, collision, interior, and render hierarchy
   to that same identity;
5. remove the prototype cube only after raster and DXR capture gates prove the
   cooked hierarchy is visible, framed, and moving correctly.

EVA, arbitrary gravity, pressure, navmesh, animation, multiplayer authority,
and save/load possession remain separate contracts.
