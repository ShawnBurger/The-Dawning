# Frontier Courier Runtime Acceptance

Date: 2026-07-22
Workstream: WS-032
Status: runtime selection and production smoke gate complete

## Scope

This checkpoint proves that the Frontier Courier Mk1 can be selected, admitted,
instantiated, traversed, possessed, flown, and rendered independently of the
reference-ship fixture. It does not claim final interior art. The authored
topology, collision, clearances, portal behavior, and interaction identities are
the protected foundation for the production dressing pass.

## Selection Contract

The application option `--content=<id>` accepts only an asset ID. The ID is
limited to 64 ASCII bytes, permits only letters, digits, `_`, and `-`, and maps
to `assets/runtime/<id>.tdcontent`. Empty IDs, extensions, separators, drive
syntax, whitespace, and traversal forms reject before any manifest is opened.
Omitting the option selects `reference_ship`, preserving the existing regression
fixture.

The production profile is launched with:

```powershell
tools\smoke_test.ps1 -Config Release -Content frontier_courier_mk1 -Flight
```

## Accepted Runtime Topology

- Scene: `ship.frontier.courier_mk1.runtime`
- Asset: `ship.frontier.courier_mk1`
- Authored envelope: 28 m long, 16 m wide, 7 m high
- Runtime bindings: 61
- Unique cooked models: 2
- Collision packages: 8
- Assembly modules: 8
- Moving parts: 7
- Runtime entities: 16
- Interactions: 8
- Portals: 7
- Static collision boxes: 88
- Closed-state dynamic boxes: 14
- Open-state dynamic boxes: 7

Each closed portal contributes one moving physical panel and one conservative
traversal guard. Opening all seven closures removes only the guards; translated
panels remain physical and every portal becomes traversable.

## Authority Correction

The original `pilot_exit_spawn` put the camera eye roughly 4 m from the pilot
seat, beyond the 2.5 m interaction range. It was moved from local
`[0.0, -0.85, -1.8]` to `[0.0, -0.85, 0.5]`. The corrected spawn is 1.6 m behind
the seat, remains collision-safe, and faces the interaction. A pipeline test now
mirrors the runtime distance and facing admission rules so this defect cannot
return silently.

## Deterministic Identity

- Cooked assembly SHA-256:
  `f396d99f7bb42b560868df0766612d6657dc7961364de91d4e17ce4bbfe07143`
- Embedded source-manifest SHA-256:
  `60b18297bc41bb8fdde09084faa661e6b58160bbc4cd3458d4ec9efd17a54b5c`
- Design-manifest file SHA-256:
  `c8140d330166123d016bb75a99707feafcf6accd61478909c6bb76262969437e`

The accepted Meshy source remains Git LFS-backed. Model, collision, assembly,
and runtime-content cooks reproduce byte-for-byte from their reviewed inputs.

## Fixture Boundary

The reference ship remains a deliberately calibrated renderer oracle. Assertions
that compare exact maxima over its known hit population stay reference-only.
The courier renders more draw records and a different visible material
population, so it is tested through path-independent renderer invariants plus
its own exact content, topology, blocker, possession, and hierarchy markers.
This preserves strict checks without pretending two different scenes have the
same aggregate pixel extrema.

## Verification Matrix

The checkpoint passed:

- 23 focused Python asset and Meshy-client tests;
- Debug and Release builds;
- Debug and Release CTest, 6/6 in each configuration;
- courier Release raster, stable DXR, and full-quality DXR smoke with flight;
- courier Debug stable DXR with flight and D3D12 GPU validation;
- reference Release raster, stable DXR, and full-quality DXR regression smoke;
- Git LFS integrity verification;
- deterministic root and child motion of 0.523611 m with 0 hierarchy error;
- pilot exit, on-foot fixed-step advance, re-entry, occupied seat, and composed
  root presentation.

Local visual evidence is retained in the ignored build tree as
`build/Release/frontier_courier_raster.png`,
`build/Release/frontier_courier_stable_dxr.png`, and
`build/Release/frontier_courier_full_dxr.png`.

## Remaining Production Boundary

The runtime gate is complete. WS-032 still owes reviewed production interior
dressing: consoles, pilot and crew seating, storage, tools, piping, displays,
materials, and habitation props. That work must preserve the certified 2.1 m
standing clearance, 0.95 m minimum clear door width, collision packages,
walkable surfaces, portal frames, interaction sockets, moving-part pivots, and
continuous exterior-to-interior reachability.
