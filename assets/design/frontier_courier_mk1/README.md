# Frontier Courier Mk1 Design Authority

This directory owns the dimensions and topology of The Dawning's first
production-intent boardable ship. Generated meshes are candidates fitted to this
authority; they never redefine it.

## Envelope

- Overall ship: 28.0 m long (`+Z`), 16.0 m wide (`X`), 7.0 m high (`Y`).
- Proportion contract: length exceeds beam, beam exceeds height, the
  length-to-beam ratio is 1.75, and the height-to-beam ratio is 0.4375.
- Habitable pressure body: 22.0 m long, 9.0 m wide, 3.2 m high.
- Main corridor: 1.4 m clear width and 2.4 m clear height.
- Nominal pressure door: 1.05 m clear width and 2.2 m clear height.
- Player capsule: 0.35 m radius and 1.8 m height.

The ship uses one continuous world. Exterior shell, boarding vestibule,
airlock, corridors, rooms, and cockpit coexist under the authoritative
assembly root with no interior loading scene.

The silhouette must read longitudinally before texture or surface detail is
considered. Width-dominant, saucer, disk, pancake, or implausibly tall outputs
are rejected rather than stretched into compliance after generation.

## Required Interior

The first accepted assembly contains these independently authored modules:

1. exterior hull with a real aft boarding cutout;
2. unpressurized boarding vestibule and independent exterior closure;
3. pressure airlock with a separate inner closure;
4. main corridor pressure spine;
5. cockpit with pilot seat and safe exit spawn;
6. engineering room with service console space;
7. cargo bay with restraint and loading envelopes; and
8. crew berth with storage and sanitation envelopes.

Every room is an explicit pressure and navigation zone. The vestibule is
vacuum-safe but unpressurized; the airlock is the pressure boundary. Every
connection has sockets on both modules, an interaction where a closure exists,
a separate moving rigid mesh, an authored pivot, collision, and a dynamic
navigation link.

## Meshy Use

Text-to-3D is permitted for disposable silhouette studies and generic props.
Production hero geometry uses an approved multi-view concept followed by Meshy
6 Multi-Image-to-3D, PBR maps, lighting removal, GLB-only output, explicit source
hashes, and a hard credit ceiling.

The text-only study `77756cfe9565468e` is retained as a negative control. It was
not refined because its aircraft-like form, axis mismatch, absent pressure
interfaces, and soft surface detail do not meet the production bar.

The first multi-view concept `d9f33a4c8d3e369d` established the industrial
language, but its reconstruction `2b35ce0e61d9192e` incorporated neighboring
view fragments and was rejected. The cleaned concept `9f3dffcc886ead1f` led to
candidate `cec12ae9a0b1cf90`; its connected pre-remesh master was accepted as
the exterior geometry source. Retexture `e0f7f2a8f6e127ec` supplied the reviewed
PBR source subsequently normalized, welded, and cut by the authored Blender
pipeline. Every manifest remains retained so paid generations and rejection
decisions are auditable without treating vendor output as design authority.

The pilot exit spawn is authored 1.6 m behind the seat along its forward axis.
With the shipped capsule and eye offsets it remains within the 2.5 m seat-use
range, faces the seat, and passes the live collision-backed exit/re-entry smoke
round trip.

## Acceptance

No source is promoted merely because it looks attractive in a thumbnail. It
must pass importer inspection, measured bounds/orientation, topology and
material budgets, exterior/interior socket alignment, deterministic cooking,
collision clearance, complete outside-to-interior reachability, reversible
closure state, cockpit possession, raster/DXR parity, and close-range captures.

The runtime profile is selected with `--content=frontier_courier_mk1`. Its
production smoke contract opens and verifies all seven portal closures, retains
one physical moved-panel collision box per open closure, performs pilot
exit/movement/re-entry, flies the meshless assembly root, and captures the same
ship in raster, stable DXR, and full DXR.
