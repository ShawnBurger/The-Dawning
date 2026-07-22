# Frontier Courier Runtime Acceptance

Date: 2026-07-22
Workstream: WS-032
Status: runtime selection, production smoke, and cockpit testing-baseline gates complete

## Scope

This checkpoint proves that the Frontier Courier Mk1 can be selected, admitted,
instantiated, traversed, possessed, flown, and rendered independently of the
reference-ship fixture. The cockpit now also carries a reviewed generated-source
testing baseline. It does not claim final production interior art. The authored
topology, collision, clearances, portal behavior, interaction identities, and
accepted fixture provenance are the protected foundation for future dressing.

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

## Cockpit Testing Baseline

The visible cockpit enclosure is assembled from four accepted Meshy geometry
masters while exact gameplay geometry retains authority over the aft portal,
walkable volume, collision, displays, and interaction locations:

| Fixture | Request SHA-256 | Accepted source SHA-256 | Authored use |
| --- | --- | --- | --- |
| Pilot seat | `b1f6b68b36fb14e15154daaa89012c86538935e5a70c43ce71710b9d638951c1` | `c73501d4e678b15386da7c27261baa2237677835effc99b0478df927a94836f6` | one normalized seat and bounded LOD family |
| Flight station | `0eb8bdcb89c3dd3753744b9b44ae6e15b65ed0b30771d8885604e67a50d41497` | `a7d8ef497d56a6ace415433d2b163f8e0f3308306b53c38a1163225ff4b166cc` | one curved console and bounded LOD family |
| Wall panel | `cf8dd773d5d656592bd80942c1ed6554d78565af2387b53a45a6983eb8230682` | `a7d7e5febba3c72be24c8a183ca534e99a22fa9aa19b7659a90a3c429af78ae5` | 29 sidewall, chamfer, and bulkhead instances |
| Deck panel | `08c94c3825dcd328455fd9577d3c3a085821d53bf5748d30f3ff8a4513b62dd9` | `7ace54dda437f6dbf8019a98a5362aeedb8516a1f516fbb2831551bdf4175fd5` | eight deck and eight inverted overhead instances |

The accepted sources consumed 120 credits. Another 110 credits are retained in
review evidence for rejected previews, refinements, and provider failures; those
results were not promoted as source authority. Generated PBR textures remain
source-only because geometry acceptance does not imply material acceptance.

All cockpit LOD meshes are closed after repair. The complete cockpit LOD triangle
counts are 447,392, 142,892, and 39,244. Boundary edges, non-manifold edges, wire
edges, and loose vertices are zero at every level. The canonical interior GLB is
21,001,032 bytes with SHA-256
`59be6bb4f39deb693174cc22ef99f585ff28617d9d360a832392121561e9df58`.
The cooked 28-primitive `.tdmodel` is 35,256,520 bytes with SHA-256
`6db140b5c3ec02124d294e4e87ac9d18b014ce1b3f7a7505a623b35376edd4ab`.

The production target and required engine systems are defined in
`HIGH_FIDELITY_SHIP_INTERIORS_AND_COCKPITS_2026-07-22.md`. This fixture is kept
deliberately as a stable test environment until a later hero cockpit passes that
document's visual, systems, interaction, and performance gates.

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

- 51 focused Python asset, cooker, collision, and Meshy-client tests
  (49 passed and 2 environment-dependent tests skipped as designed);
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

The runtime and cockpit testing-baseline gates are complete. Final hero-room art,
state-driven local lighting, diegetic displays, ship resource networks, room
atmosphere, physicalized components, layered wear/damage, audio zones, and
production navigation are later workstreams. They must preserve the certified
2.1 m standing clearance, 0.95 m minimum clear door width, collision packages,
walkable surfaces, portal frames, interaction sockets, moving-part pivots, and
continuous exterior-to-interior reachability.
