# Interior State and Local Lighting Contract

Date: 2026-07-22
Workstream: WS-033
Status: CPU vertical slice implemented; GPU consumption deferred

## Purpose

The first production interior-lighting milestone establishes one authoritative
path from authored fixtures to renderer-ready records:

```text
schema-v2 assembly manifest
  -> strict validator and deterministic cooker
  -> authenticated cooked assembly loader
  -> ship-owned interior state coordinator
  -> immutable revisioned snapshot
  -> camera-relative renderer-facing light frame
```

It does not add a second room graph, direct gameplay writes into renderer
memory, or asset-specific C++ wiring. The existing assembly module identities
remain authoritative.

## Authored Contract

Schema v1 remains supported and compiles byte-for-byte to the existing reference
assembly. Schema v2 appends a required `light_fixtures` table after navigation.
Each record contains:

- stable fixture and owning interior-module identity;
- point or spot type;
- module-local position and normalized direction;
- color temperature in kelvin;
- lumens for point fixtures or candela for spot fixtures;
- range and point/spot cone contract;
- group and circuit identities;
- normalized importance and shadow policy;
- emergency behavior, color temperature, and intensity scale.

Emergency behavior is explicit:

| Policy | Normal | Emergency | Blackout |
| --- | --- | --- | --- |
| `unchanged` | normal output | normal output | off |
| `off` | normal output | off | off |
| `emergency_only` | off | emergency color/scale | off |
| `override` | normal output | emergency color/scale | off |

The validator and runtime loader reject duplicate IDs, exterior ownership,
unknown enums, non-unit directions, invalid point or spot cones, nonfinite or
nonphysical photometric values, out-of-range emergency scales, unsafe text,
record-count overflow, and trailing payload data. Schema v2 requires at least
one fixture; schema v1 cannot carry fixture bytes.

Production manifests are capped at 4,096 fixtures, 1,024 groups, 1,024
circuits, and 256 bytes per fixture/group/circuit ID. These match the default
coordinator limits. The cooked loader keeps a separate higher abuse ceiling for
safe tooling and forward-compatible inspection; loading a file alone does not
guarantee admission into a lower-budget ship runtime.

## State Ownership

`ship::InteriorStateCoordinator` owns copied immutable fixture topology and the
mutable alert, group, circuit, and fixture controls. It publishes
`shared_ptr<const ShipInteriorSnapshot>` instances in canonical fixture-ID
order. Consumers cannot mutate published state.

Updates are simulation-tick ordered and transactional. Older ticks fail; an
identical update at the same tick reuses the published snapshot; a changed
control or later tick increments `stateRevision`. Failed lookup, validation,
allocation, or snapshot construction leaves both owned state and the previously
published snapshot untouched.

Resolved intensity is:

```text
authored intensity
  * fixture health
  * group intensity scale
  * circuit available-power scale
  * applicable emergency scale
```

Boolean fixture/group/circuit state and blackout policy can force that result to
zero. Zero-output fixtures remain in the resolved table with their stable index,
so gameplay, saves, diagnostics, and later GPU selection share identity.

The coordinator is intentionally single-writer simulation state. Immutable
snapshot lifetime permits readers to retain a complete generation; concurrent
publication policy belongs to the future ship-runtime owner that schedules the
coordinator.

## Renderer-Facing Boundary

`BuildInteriorLightFrame` consumes one immutable snapshot, a typed module-pose
view carrying the same topology hash/revision, and the double-precision camera
origin. It validates topology identity, snapshot identity/order, transform
finiteness, unit quaternions, uniform scale, physical values, and hard limits
before replacing the output. A pose span from a different assembly therefore
cannot pass merely because it has enough elements.

For every active fixture, it computes:

```text
(double module world position - double camera world position)
  + rotated module-local fixture offset
```

Only that camera-relative result narrows to float. This preserves meter-scale
interior placement at astronomical coordinates. The compact frame also carries
linear chromaticity, physical intensity, scaled range, direction, cone cosines,
importance, stable fixture index, module index, type, and shadow policy. Disabled
fixtures are omitted from the compact frame but retain stable authored indices.

The CPU frame is an interface, not a claim of visible lighting. GPU buffer
ownership, descriptor lifetime, clustered/tiled selection, BRDF evaluation,
shadow scheduling, and equivalent DXR sampling remain renderer work.

## Frontier Courier Baseline

The Courier now authors twelve fixtures across cockpit, corridor, airlock,
cargo, crew, and engineering modules. Groups separate normal room/task lights,
console lights, and emergency fixtures. Circuits separate `main_bus_a`,
`main_bus_b`, and `battery_bus`. Cockpit consoles and the engineering task light
use emergency overrides; key/room lights shed; battery fixtures activate only in
emergency mode.

Deterministic identities:

- cooked assembly bytes: `9,720`;
- cooked SHA-256:
  `bded6f90c8a29cf2b4deae4fadebf062cabdcb2c566aa7b45a66335c0789ad42`;
- embedded canonical manifest SHA-256:
  `1b713519f5261fe3dbc34938602c735d325cb22599532753f4225268cd14883b`.

Two independent cooks were byte-identical. The schema-v1 reference assembly is
also checked against its existing runtime bytes. Raw JSON bytes are not an
identity because Git line-ending policy can differ by checkout; the embedded
hash is calculated from canonical JSON.

## Verification

The completed CPU slice passed:

- 61 Python asset, cooker, collision, Meshy-client, and Courier pipeline tests
  with 58 passing and 3 environment-dependent cases skipped as designed;
- 539 C++ unit cases and 19,487 checks with zero failures;
- complete Debug and Release builds;
- Debug and Release CTest, 6/6 in each configuration;
- Courier Release raster, stable DXR, and full DXR smoke with flight;
- Courier Debug stable DXR with flight and D3D12 GPU validation;
- reference Release raster, stable DXR, and full DXR regression smoke;
- nonblank 1920 x 1080 capture checks in every smoke profile.

## Follow-On Renderer Milestone

After the active atmosphere lane releases shared renderer files:

1. upload `InteriorLightFrame` through a bounded frame-owned GPU buffer;
2. build a tiled or clustered visible-light list;
3. evaluate the same punctual-light BRDF in raster and DXR;
4. allocate shadow work from visibility, importance, policy, motion, and state
   revision rather than giving every practical light a shadow map;
5. expose normal, emergency, blackout, group, and circuit smoke probes without
   adding Courier-specific renderer logic;
6. capture matched raster/stable-DXR/full-DXR cockpit and cabin evidence.

The GPU phase must consume this contract rather than reinterpret manifest data
or own mutable ship state.
