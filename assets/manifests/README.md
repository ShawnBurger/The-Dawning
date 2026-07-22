# Production assembly manifests

`.tdasset.json` is the engine-owned assembly contract for every boardable ship
and structure. Meshy/GLB supplies visual source geometry; this manifest supplies
the gameplay and integration facts that generated geometry cannot guarantee.

Validate one or more manifests:

```powershell
python tools/validate_asset_manifest.py assets/manifests/reference_ship.tdasset.json
```

Schema v1 requires meters, the engine coordinate convention, exterior and
interior modules, source provenance, collision and three LODs per module,
oriented sockets, pressure/nav/walk zones, a connected outside-to-interior portal
graph, stateful interactions, and pivoted moving parts. The reference ship is a
schema and workflow example, not a shipped visual asset. Each moving rigid part
also names its visual node and shares module ownership with its interaction, so
the cooker can bind animation to geometry without name guessing.

Schema v2 appends a nonempty `light_fixtures` table. Every fixture has stable
identity and interior-module ownership plus a point/spot type, local position
and unit direction, physical intensity, range, cone angles, color temperature,
group, circuit, importance, shadow policy, and explicit emergency behavior.
Emergency behavior is one of `unchanged`, `off`, `emergency_only`, or `override`;
the latter two apply the authored emergency color temperature and intensity
scale. Point fixtures use 180-degree inner/outer cones. The validator rejects
nonphysical values, invalid ownership, unsupported policies, and ambiguous cone
contracts before cooking.
Production authoring is capped at 4,096 fixtures, 1,024 groups, 1,024 circuits,
and 256 bytes per fixture/group/circuit ID, matching the coordinator defaults.
The cooked loader retains a higher independent abuse ceiling so tools can fail
closed before allocating unbounded input.

Boardable assemblies also author locomotion spawns independently from seats.
The reference ship's `pilot_exit_spawn` is a floor anchor owned by the cockpit
module; runtime possession derives the full capsule from that anchor and rejects
an exit when the live assembly-local collision snapshot obstructs it.

Do not weaken this contract to admit a generated monolithic mesh. Additive schema
changes increment `schema_version`; runtime/cooker support must land before a
new version can be promoted. Schema-v1 compilation is covered by an exact-byte
compatibility test against `assets/runtime/reference_ship.tdassembly`.
