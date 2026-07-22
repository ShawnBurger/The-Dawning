# Frontier Courier Mk1 Production Source

Status: first production-intent boardable ship kit; cockpit art is an accepted
testing baseline, not the final hero-room target.

The closed assembly is 28.0 m long, 16.0 m wide, and 7.0 m high. Its 1.75
length-to-beam ratio and 0.4375 height-to-beam ratio are enforced by the
acceptance tool. The open hull is slightly shorter because the aft aperture
removes the generated shell's single rearmost point; the separate exterior
hatch restores the full closed envelope.

## Artifacts

- `frontier_courier_hull_retexture_source.glb`: authenticated Meshy retexture
  output promoted to LFS-backed production source; this is the reproducible
  Blender acceptance input, not runtime content.
- `frontier_courier_hull_lods.glb`: three shared-texture exterior primitives.
- `frontier_courier_hull_lods.report.json`: source hashes, manifold cavity
  evidence, PBR inventory, bounds, and deterministic primitive order.
- `frontier_courier_interior_lods.glb`: seven module-local interiors at three
  LODs plus seven independent closure panels. The cockpit module includes the
  accepted generated pressure-vessel architecture and flight furniture.
- `frontier_courier_interior_lods.report.json`: socket-derived primitive map,
  topology evidence, generated-fixture provenance, source hashes, placement
  envelopes, and exact runtime locator bindings.
- `frontier_courier_pilot_seat_source.glb`: authenticated Meshy pilot-seat
  geometry promoted under Git LFS.
- `frontier_courier_flight_station_source.glb`: authenticated Meshy curved
  flight-station geometry promoted under Git LFS.
- `frontier_courier_cockpit_wall_panel_source.glb`: authenticated Meshy wall
  bay normalized into the repeated sidewall, chamfer, and bulkhead skin.
- `frontier_courier_cockpit_deck_panel_source.glb`: authenticated Meshy deck bay
  used in upright deck and inverted overhead placements.

The hull began as reviewed Meshy multi-view source art. Meshy's remeshed result
was rejected because it fractured into thousands of components. The retained
pre-remesh manifold was decimated locally, retextured by Meshy, restored to the
design-authority dimensions, welded at exact UV seams, and cut with Blender
4.5's manifold-only Boolean solver. Architecture, clearances, sockets, pivots,
pressure topology, collision, navigation ownership, and runtime identity are
The Dawning-authored.

The cockpit is no longer the original blockout. Its visible room envelope,
flight station, and pilot seat are generated source geometry placed by authored
dimensions. Deliberate display glass and indicator surfaces are added after
geometry sanitization so generated pseudo-labels never enter the runtime. The
result is the stable testing cockpit. The later production hero room follows
`docs/research/HIGH_FIDELITY_SHIP_INTERIORS_AND_COCKPITS_2026-07-22.md`.
Other ship modules remain the gameplay-safe architectural base and are ready for
reviewed storage, tools, pipes, displays, and habitation props; those assets may
add detail but may not alter clearances, portal locations, pressure boundaries,
or module-local origins.

See `tools/blender/README.md` for reproducible Blender, collision, cook, and
runtime-content commands.
