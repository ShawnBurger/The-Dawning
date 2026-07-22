# Frontier Courier Mk1 Production Source

Status: first production-intent boardable ship kit.

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
  LODs plus seven independent closure panels.
- `frontier_courier_interior_lods.report.json`: socket-derived primitive map,
  topology evidence, source hashes, and exact runtime locator bindings.

The hull began as reviewed Meshy multi-view source art. Meshy's remeshed result
was rejected because it fractured into thousands of components. The retained
pre-remesh manifold was decimated locally, retextured by Meshy, restored to the
design-authority dimensions, welded at exact UV seams, and cut with Blender
4.5's manifold-only Boolean solver. Architecture, clearances, sockets, pivots,
pressure topology, collision, navigation ownership, and runtime identity are
The Dawning-authored.

The current interior is the gameplay-safe architectural and systems-layout
base. It is ready for production dressing with reviewed consoles, seats,
storage, tools, pipes, displays, and habitation props; those assets may add
detail but may not alter clearances, portal locations, pressure boundaries, or
module-local origins.

See `tools/blender/README.md` for reproducible Blender, collision, cook, and
runtime-content commands.
