# Production assembly manifests

`.tdasset.json` is the engine-owned assembly contract for every boardable ship
and structure. Meshy/GLB supplies visual source geometry; this manifest supplies
the gameplay and integration facts that generated geometry cannot guarantee.

Validate one or more manifests:

```powershell
python tools/validate_asset_manifest.py assets/manifests/reference_ship.tdasset.json
```

The current schema requires meters, the engine coordinate convention, exterior
and interior modules, source provenance, collision and three LODs per module,
oriented sockets, pressure/nav/walk zones, a connected outside-to-interior portal
graph, stateful interactions, and pivoted moving parts. The reference ship is a
schema and workflow example, not a shipped visual asset. Each moving rigid part
also names its visual node and shares module ownership with its interaction, so
the cooker can bind animation to geometry without name guessing.

Boardable assemblies also author locomotion spawns independently from seats.
The reference ship's `pilot_exit_spawn` is a floor anchor owned by the cockpit
module; runtime possession derives the full capsule from that anchor and rejects
an exit when the live assembly-local collision snapshot obstructs it.

Do not weaken this contract to admit a generated monolithic mesh. Additive schema
changes increment `schema_version`; runtime/cooker support must land before a
new version can be promoted.
