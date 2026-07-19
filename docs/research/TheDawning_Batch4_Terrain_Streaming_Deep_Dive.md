# THE DAWNING — Terrain & World Streaming Deep Dive
# Batch 4, Topic 3: Clipmap LOD, Quadtree Terrain, Virtual Texturing,
#                    Sector Streaming, Double Precision, Seamless Transitions

---

## PART 1: PLANET-SCALE TERRAIN

### Quadtree-Based Terrain LOD

```cpp
// Each planet surface is a cube-mapped quadtree
// 6 faces × recursive quadtree subdivision = spherical terrain

struct TerrainNode {
    int face;           // 0-5 (cube face)
    int depth;          // Quadtree depth (0=entire face, 20=~1cm resolution)
    int x, y;           // Grid position at this depth
    float minHeight;    // Terrain height range (for frustum culling)
    float maxHeight;
    bool hasChildren;   // If subdivided
    TerrainNode* children[4]; // NW, NE, SW, SE
    
    // GPU data
    uint32_t meshBufferId;    // Vertex/index buffer on GPU
    uint32_t textureId;       // Virtual texture tile
    bool gpuResident;         // Is data currently on GPU?
};

// LOD selection: subdivide nodes closer to camera, merge distant ones
// Each node renders as a 33×33 vertex grid (32×32 quads = 2048 triangles)
// At depth 0: 1 node covers entire face (~10,000 km on Earth-sized planet)
// At depth 15: 1 node covers ~0.3m (enough for ground-level detail)

void UpdateTerrainLOD(TerrainNode& node, const Vec3d& cameraPos, 
                         double planetRadius) {
    // Compute distance from camera to node center
    Vec3d nodeCenter = NodeCenterOnSphere(node, planetRadius);
    double dist = (cameraPos - nodeCenter).Length();
    
    // Geometric error: how much visual error this node has at this distance
    double nodeSize = planetRadius * 2.0 / (1 << node.depth);  // Node edge length
    double screenError = nodeSize / dist;  // Approximate screen-space size
    
    double errorThreshold = 0.01;  // ~1% of screen = subdivide
    
    if (screenError > errorThreshold && node.depth < 20) {
        // Subdivide: create 4 children
        if (!node.hasChildren) CreateChildren(node);
        for (int i = 0; i < 4; i++)
            UpdateTerrainLOD(*node.children[i], cameraPos, planetRadius);
    } else {
        // This node is fine — render it, remove children if any
        if (node.hasChildren) CollapseChildren(node);
    }
}
```

### Height Generation (from ProcGen Curriculum)

```
For each terrain vertex:
1. Map cube face UV → sphere position (normalize to unit sphere × radius)
2. Compute tectonic plate influence (large-scale continent shapes)
3. Add fractal noise (6-8 octaves of Simplex noise for mountains/valleys)
4. Apply hydraulic erosion (simulate water flow carving terrain)
5. Apply thermal erosion (steep slopes crumble)
6. Clamp to sea level (flat ocean where height < 0)
7. Add biome-specific detail (desert dunes, arctic ridges, volcanic tubes)

Height range: -10km to +15km from sea level (Earth-scale)
Noise frequencies: 1/10000km (continents) → 1/1m (rocks)
Each octave: frequency ×2, amplitude ×0.5 (standard fractal)
```

---

## PART 2: CLIPMAP TERRAIN (Ground Level)

When on a planet surface, switch from quadtree sphere to clipmap grid.

```
Clipmap: concentric square grids centered on camera, each 2× scale of inner

Level 0: 255×255 grid, 0.5m spacing, 127m extent    (rocks, grass)
Level 1: 255×255 grid, 1.0m spacing, 255m extent    (boulders, paths)
Level 2: 255×255 grid, 2.0m spacing, 510m extent    (hills, rivers)
Level 3: 255×255 grid, 4.0m spacing, 1020m extent   (valleys, forests)
Level 4: 255×255 grid, 8.0m spacing, 2040m extent   (mountains, far terrain)
Level 5: 255×255 grid, 16m spacing, 4080m extent    (horizon)

Total triangles: 6 levels × 64,516 quads × 2 tri = 774,192 triangles
With frustum culling: typically ~400K visible

Transition between levels: 16-vertex blend zone (cross-fade heights)
GPU tessellation: add subdivisions on levels 0-1 for very close terrain

Height data: streamed from disk as camera moves
Update rate: recenter clipmap when camera moves > half a cell at finest level
```

---

## PART 3: VIRTUAL TEXTURING

### Concept
The planet surface can't fit all textures in VRAM (a single planet at 1m resolution
= terabytes of texture data). Virtual texturing streams only visible tiles.

```
Virtual texture: 128K × 128K virtual resolution (one per planet face)
Page/tile size: 128×128 pixels (with 4-pixel border for filtering)
Physical cache: 2048×2048 pixels (256 tiles resident at once)

Runtime:
1. Render scene with special "page ID" shader that outputs which virtual page 
   each pixel needs
2. Read back page requests (GPU → CPU, 1 frame latency)
3. For each requested page not in cache:
   a. Generate texture tile (procedural from biome + noise)
   b. Upload to physical cache (replace least-recently-used tile)
   c. Update indirection table (virtual page → physical location)
4. Final render samples through indirection table

Bandwidth: ~50 new tiles per frame at walking speed, ~200 when flying
Cache thrash prevention: keep tiles for 2 seconds after last use
```

### Material Blending

```
Each terrain point blends between 4 materials based on:
  - Slope: steep = rock, flat = soil/grass
  - Altitude: low = vegetation, high = snow
  - Moisture: wet = mud/moss, dry = sand/dust
  - Temperature: hot = desert, cold = ice
  
Blend weights stored in terrain vertex color (RGBA = 4 material weights)
Material IDs from PBR Material Bible (exact albedo, roughness, etc.)

Triplanar projection for cliff faces:
  Instead of UV mapping (which stretches on cliffs), project texture 
  from X, Y, Z directions and blend based on surface normal
  Weight_x = abs(normal.x)^8, Weight_y = abs(normal.y)^8, etc.
  The ^8 power makes the blend sharp (mostly one direction dominates)
```

---

## PART 4: SECTOR STREAMING

### Space Sector System

```
The galaxy is divided into sectors (star systems).
Only the current sector + adjacent sectors are loaded.

Sector data:
  Star(s): position, type, luminosity (from seed)
  Planets: orbital elements, composition, seed (from galaxy seed)
  Stations: position, type, faction (from seed + narrative overrides)
  NPCs: spawned from faction templates (transient, regenerate on revisit)
  Asteroids: belt parameters (generated on demand, not stored)
  
Loading strategy:
  Current sector: fully loaded (all objects active)
  Adjacent sectors (6-26 neighbors): metadata only (star positions for skybox)
  Distant sectors: on-rails only (faction ships move between systems analytically)

Transition:
  Player approaches sector boundary → begin preloading next sector
  At boundary: seamless handoff (no loading screen within a star system)
  Between systems (warp): loading screen while generating destination
  Generation time target: <3 seconds per star system
```

### Double Precision World

```
Problem: float32 has ~7 digits of precision
  At 10,000m from origin: precision = ~1mm (OK)
  At 1,000,000m: precision = ~0.1m (visible jitter!)
  At 1 AU (150 billion m): precision = ~10km (useless)

Solution: double precision (float64) for world positions
  At 1 AU: precision = ~0.01mm (perfect)
  
But GPU only does float32 efficiently!

Camera-relative rendering:
  1. World positions stored as Vec3d (double)
  2. Before rendering: subtract camera position from all objects
     Vec3f renderPos = (worldPos - cameraPos).ToFloat()
  3. GPU receives float32 positions relative to camera
  4. At camera position (0,0,0), precision is perfect
  5. Objects 10km away: float32 precision = ~1mm (fine for rendering)
  
This is how every space game handles the precision problem.
```

---

## PART 5: SPACE-TO-SURFACE TRANSITION

```
The seamless space-to-surface transition:

Altitude > 100km: SPACE MODE
  Quadtree sphere terrain (low detail)
  Atmosphere visible as rim glow
  No ground-level detail

Altitude 100km-10km: ATMOSPHERE ENTRY
  Quadtree terrain subdivides toward camera
  Atmosphere scattering shader activates (see Atmosphere Bible)
  Reentry effects on ship (heat glow, plasma trail)
  Cloud layer becomes visible (volumetric clouds from Particles Bible)

Altitude 10km-1km: APPROACH
  Terrain detail increases rapidly
  Trees/structures begin appearing as LOD0
  Switch from orbital velocity to surface-relative velocity
  Weather effects activate (rain, wind, snow)

Altitude 1km-0: SURFACE
  Switch to clipmap terrain for ground detail
  Quadtree nodes at camera position at maximum subdivision
  Virtual texturing provides surface detail
  Grass, rocks, creatures visible
  
The key: there is NO loading screen. The LOD system smoothly
transitions from space-visible resolution to ground-level detail.
CPU budget: terrain generation runs in background thread,
always 2-3 nodes ahead of camera movement.
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 181-190 (Terrain foundation)**: Flat grid terrain with heightmap. Simple vertex displacement. Basic texturing from height.

**Step 191-200 (Clipmap)**: 5-level clipmap grid. Height streaming. Level transition blending.

**Step 201-210 (Procedural height)**: Fractal noise height generation. Tectonic plate shapes. Biome-driven detail.

**Step 211-220 (Material blending)**: 4-material blend from slope/altitude/moisture. Triplanar projection on cliffs. PBR material values from Material Bible.

**Step 221-240 (Planet rendering)**: Cube-sphere projection. Quadtree LOD on sphere. Double-precision camera-relative rendering.

**Step 241-250 (Streaming)**: Sector loading/unloading. Background thread generation. Preload on approach.

**Step 1131-1150 (Terrain depth)**: Virtual texturing system. GPU tessellation. Hydraulic erosion pass. Procedural placement of vegetation/rocks. LOD for placed objects. Seamless space-to-surface transition.

**Step 2001-2100 (ProcGen terrain depth)**: Full tectonic simulation, hydraulic/thermal erosion, climate model, biome classification — all detailed in the 3000-step ProcGen curriculum.
