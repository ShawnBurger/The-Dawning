# Interactive Interior Runtime Contract

Date: 2026-07-21
Workstream: WS-027, Stage 5A

## Decision

The cooked assembly is the authority for interaction identity, state
vocabulary, socket placement, portal ownership, and moving-part motion. The
runtime executes that data; it does not infer door topology from render meshes
or encode a reference-ship layout in C++.

Stage 5A provides the first executable interior behavior boundary:

- exact authored-ID and stable-index activation;
- bounded nearest-use queries from a world position and view direction;
- reversible door, hatch, airlock, and elevator transitions;
- deterministic normalized motion progress;
- linear and rotational moving-part transforms;
- portal traversal only when its closure is fully open;
- topology-bound, validation-first snapshot capture and application;
- explicit host publication and teardown ordering.

It does not provide player collision, walking, navmesh execution, pressure
simulation, cockpit possession, prompts, network replication, a persistent save
file envelope, or final ship art.

## Runtime Boundary

`scene::AssemblyInteriorRuntime` is CPU-only. It owns no ECS entity and no GPU
resource. Initialization consumes:

1. one immutable validated `asset::CookedAssembly`;
2. the exact prepared module transforms from WS-025;
3. the exact prepared moving-part closed transforms from WS-025.

All records are copied into private staged storage before publication.
Initialization rejects malformed stable indices, missing state vocabulary,
nonreciprocal part/portal ownership, invalid transforms, unknown motion types,
nonfinite pivots/travel, zero axes, zero travel, and topology/count mismatch.
Failure leaves the runtime uninitialized.

`scene::AssemblyRuntimeHost` owns the runtime beside the committed assembly
instance. Its transaction order is:

```text
load and verify cooked content
  -> prepare immutable WS-025 plan
  -> retire GPU uploads
  -> commit complete ECS assembly
  -> initialize interior runtime from the same plan
  -> validate and publish closed moving-part transforms
  -> expose live host
```

If interior initialization or transform publication fails, the newly committed
assembly instance is destroyed and the host does not report success. During
shutdown, interior state is cleared before assembly entities and their owning
resources are destroyed.

## State Model

Moving interactions require the authored states:

```text
closed -> opening -> open -> closing -> closed
             ^          |
             +----------+  activation reverses in flight
locked                         activation rejects
```

Activation toggles toward the opposite endpoint. A second activation while a
closure is moving reverses direction without snapping or resetting progress.
Nonmoving interactions, such as the current pilot seat, cycle through their
authored states in stable order.

Motion progress is normalized to `[0, 1]`. Linear speed is meters per second;
rotational speed is degrees per second. The authored travel magnitude divided
by the applicable positive speed determines duration. Fixed-step partitioning
therefore reaches the same state and pose for the same total elapsed time.

## Transform Reconstruction

Every moving pose is reconstructed from its immutable closed world transform.
No frame applies an incremental delta to the previous frame.

For linear parts, the authored local axis and travel are scaled by the owning
module's world scale, rotated by its world rotation, and added to the closed
world position.

For rotational parts, the authored module-local pivot is transformed to world
space. The local axis-angle delta is conjugated through the module world
rotation, then applied to both the closed pivot offset and closed rotation.

This prevents accumulated drift across long runs, repeated reversals, and save
restores. It also preserves nonuniform module scale for local travel.

## Query And Portal Rules

Nearest activation evaluates authored interaction sockets, not render bounds.
A query must provide finite world position, a nonzero forward vector, a positive
radius, and a forward-dot threshold in `[-1, 1]`. Candidates outside the radius
or view cone are ignored; shortest distance wins and stable order resolves an
exact tie.

A portal is traversable only when:

- the portal owns a valid navigation link;
- its reciprocal closure interaction is in exact `open` state;
- closure progress is exactly `1.0`.

Opening or closing portals remain blocked. This output is the future dynamic-nav
link and pressure-seal input; Stage 5A does not itself run either system.

## Snapshot Contract

An interior snapshot stores the assembly source-manifest SHA-256 plus one
`(stateIndex, motionProgress)` record per stable interaction. Apply validates the
complete snapshot before the first mutation:

- exact topology digest and interaction count;
- finite progress in `[0, 1]`;
- state index in the authored vocabulary;
- endpoint/progress agreement for closed and open;
- valid in-flight ranges for opening and closing;
- zero progress for nonmoving interactions.

Only then are all records committed and moving transforms rebuilt. This is
save-ready state, not a persistent save-game format. A later game-save envelope
must include this snapshot atomically beside the simulation snapshot and world
identity.

## Production Wiring

`F` activates the nearest authored interaction within 2.5 meters and the camera
view cone. `App::AdvanceSimulation` advances interiors on the same fixed step as
the simulation scheduler. An actual moving-pose change invalidates path-tracing
history; idle states do not.

Smoke startup activates `outer_hatch` by authored ID, advances it to `open`, and
requires its owned `outer_entry` portal to become traversable. The harness also
requires exact topology counts. Stable IDs are resolved at runtime; source JSON
array order is never assumed.

## Acceptance Evidence

The GPU-free suite directly covers initialization and repeated shutdown,
rotational and linear motion, reversal, module scale, deterministic time
partitioning, range/view queries, nonmoving and locked states, portal gating,
snapshot round trip, topology mismatch, atomic invalid-snapshot rejection, and
malformed-topology rejection.

The GPU smoke covers cooked manifest load, transactional ECS publication,
authored activation, moving transform publication, exact open state, and portal
passability through the shipped application path.

## Required Follow-On

1. Cook collision shapes and add deterministic capsule sweep/slide/step queries.
2. Publish walkable surfaces and navmeshes, consuming portal passability as
   dynamic links.
3. Add an on-foot/EVA controller and explicit ship/on-foot control contexts.
4. Couple portal closure and seal state to pressure zones and atmosphere flow.
5. Add cockpit possession and boarding transitions around the authored seat.
6. Put interior snapshots inside the versioned atomic game-save envelope.
7. Replace the corridor witness with reviewed modular production geometry while
   preserving these authored identities and pivots.
