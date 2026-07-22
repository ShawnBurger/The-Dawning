# Blender Production Tools

These scripts turn reviewed source art into deterministic production inputs.
They run inside Blender and treat The Dawning's asset manifest as the authority
for dimensions, axes, topology, sockets, collision, and interaction boundaries.

## Frontier courier hull LOD0

Run from a shell with Blender 4.5 available:

```powershell
blender --background --factory-startup --python `
  tools/blender/prepare_hull_lod0.py -- `
  --source assets/generated/<accepted-candidate>/master_pre_remesh.glb `
  --output tmp/frontier_courier_hull_lod0.glb `
  --report tmp/frontier_courier_hull_lod0.report.json `
  --blend tmp/frontier_courier_hull_lod0.blend `
  --asset-id ship.frontier.courier_mk1 `
  --module-id exterior_hull `
  --width 16 --height 7 --length 28 --target-faces 250000
```

The tool rejects sources and outputs that are disconnected, open,
non-manifold, wire-only, loose, inverted, or outside the authored dimensions.
It records the Blender version, source and output hashes, geometry statistics,
and exact authored dimensions in a JSON report. It does not create collision,
walk surfaces, portals, or moving parts from generated geometry.

After an accepted Meshy retexture, run `accept_retextured_hull.py`. It restores
the authored dimensions, welds coincident vertices introduced at UV seams while
preserving loop UVs, unions the manifest-owned room volumes, and uses Blender
4.5's manifold-only Boolean solver to create the real aft boarding opening and
connected interior cavity. It rejects any open or non-manifold result, derives
LOD1 and LOD2, and exports all three primitives in one GLB so they share one
embedded PBR texture set. The runtime content catalog selects primitive indices
0, 1, and 2. The separately authored exterior hatch restores the full 28 m
closed-assembly envelope.

The courier's exact accepted vendor input is retained at
`assets/source/frontier_courier_mk1/frontier_courier_hull_retexture_source.glb`
under Git LFS. Rebuild the canonical hull with:

```powershell
blender --background --factory-startup --python `
  tools/blender/accept_retextured_hull.py -- `
  --source assets/source/frontier_courier_mk1/frontier_courier_hull_retexture_source.glb `
  --output assets/source/frontier_courier_mk1/frontier_courier_hull_lods.glb `
  --report assets/source/frontier_courier_mk1/frontier_courier_hull_lods.report.json `
  --asset-id ship.frontier.courier_mk1 --module-id exterior_hull `
  --width 16 --height 7 --length 28
```

Reports store repository-relative paths so hashes and evidence remain portable
across worktrees and clean clones.

## Frontier courier interior

`build_frontier_courier_interior.py` reads the design authority and reviewed
`.tdasset.json` graph. It derives every wall opening from module-local portal
sockets, validates coincident world-space endpoints, and exports:

- seven architectural modules at LOD0, LOD1, and LOD2;
- a separate rigid mesh for each of the seven moving closures;
- a generated cockpit pressure-vessel room assembled from authenticated wall,
  deck/overhead, flight-station, and pilot-seat source masters;
- one vertex-color PBR material; and
- a JSON locator-to-primitive map used by the runtime content generator.

The cockpit sources are deliberately modular. Meshy's whole-room attempts were
rejected by the provider, while a generated sealed frame also failed visual
integration. The accepted masters are normalized to design-authority
dimensions, voxel-closed once, decimated into bounded LODs, and instanced with
explicit right-handed bases. Generated geometry owns every visible wall, deck,
ceiling, chamfer, and bulkhead skin. The exact aft portal frame, interaction
sockets, collision, and pressure boundary stay authored gameplay contracts.

```powershell
blender --background --factory-startup --python `
  tools/blender/build_frontier_courier_interior.py -- `
  --dimensions assets/design/frontier_courier_mk1/dimensions.json `
  --manifest assets/manifests/frontier_courier_mk1.design.tdasset.json `
  --pilot-seat-source assets/source/frontier_courier_mk1/frontier_courier_pilot_seat_source.glb `
  --flight-station-source assets/source/frontier_courier_mk1/frontier_courier_flight_station_source.glb `
  --cockpit-wall-source assets/source/frontier_courier_mk1/frontier_courier_cockpit_wall_panel_source.glb `
  --cockpit-deck-source assets/source/frontier_courier_mk1/frontier_courier_cockpit_deck_panel_source.glb `
  --output assets/source/frontier_courier_mk1/frontier_courier_interior_lods.glb `
  --report assets/source/frontier_courier_mk1/frontier_courier_interior_lods.report.json
```

Render the three deterministic cockpit acceptance views with:

```powershell
blender --background --factory-startup --python `
  tools/blender/render_frontier_courier_cockpit_review.py -- `
  --model assets/source/frontier_courier_mk1/frontier_courier_interior_lods.glb `
  --output-dir tmp/validation/frontier_courier_cockpit
```

Render geometry is never collision. Run
`tools/generate_frontier_courier_collision.py`, then cook each generated source
with `tools/compile_collision_asset.py`. Finally run
`tools/generate_frontier_courier_content.py` to bind the cooked hull, interior,
collision, navigation, and walkable locators with exact coverage.
