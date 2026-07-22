# Helix Induction Carbine Mk1 Source Assets

This directory contains accepted, exact-scale source assets for the modular
Helix carbine. `assets/design/helix_carbine_mk1/component_plan.json` owns all
dimensions, assembly positions, interfaces, and component status.

Only one component is produced at a time. Generated render geometry never owns
collision, ballistics, sockets, pivots, moving-part state, or interaction
behavior.

## Upper receiver

- `helix_carbine_upper_receiver_authored.blend` is the editable hard-surface
  authority.
- `helix_carbine_upper_receiver_authored.glb` is the exact input sent to Meshy.
- `helix_carbine_upper_receiver_retexture_source.glb` is the untouched accepted
  Meshy output retained for provenance.
- `helix_carbine_upper_receiver_pbr.blend` and
  `helix_carbine_upper_receiver_pbr.glb` are the exact-scale production source.
- The two JSON reports record authored interfaces, hashes, dimensions, topology,
  provider provenance, PBR maps, and the uniform scale restoration.

Rebuild the authored component and final PBR source with the commands in
`tools/blender/README.md`.
