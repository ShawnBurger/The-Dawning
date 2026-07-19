# THE DAWNING — D3D12 Rendering Pipeline Deep Dive
# Batch 1, Topic 1: GPU-Driven Rendering, Render Graph, Advanced Techniques
# Sources: NVIDIA GPU Gems, DirectX Dev Blog, AMD GPUOpen, MJP "Ten Years of D3D12",
#          PlayerUnknown Productions GPU-driven instancing, Work Graphs spec

---

## PART 1: MODERN D3D12 PIPELINE ARCHITECTURE

### The Render Graph Pattern
Every AAA engine uses a render graph (frame graph) to organize GPU work.
Instead of manually managing resource barriers and pass ordering, you declare
passes and their inputs/outputs, and the graph resolves dependencies automatically.

```cpp
// Render graph pseudocode — how our engine should structure each frame
struct RenderPass {
    std::string name;
    std::vector<ResourceHandle> reads;    // Textures/buffers this pass reads
    std::vector<ResourceHandle> writes;   // Textures/buffers this pass writes
    std::function<void(ID3D12GraphicsCommandList*)> execute;
};

// Frame structure (order determined by dependency resolution):
// 1. Shadow Map Pass (writes: shadowMap)
// 2. GBuffer Pass (writes: albedo, normal, depth, motion vectors)
// 3. SSAO Pass (reads: normal, depth; writes: aoTexture)
// 4. Lighting Pass (reads: gbuffer, shadowMap, aoTexture; writes: hdrColor)
// 5. SSR Pass (reads: hdrColor, normal, depth; writes: reflections)
// 6. Volumetric Clouds (reads: depth; writes: cloudTexture)
// 7. Atmosphere Pass (reads: depth; writes: skyTexture)
// 8. Composite Pass (reads: hdrColor, clouds, sky, reflections; writes: composited)
// 9. Bloom Pass (reads: composited; writes: bloom)
// 10. Tonemap + TAA (reads: composited, bloom, motionVectors, prevFrame; writes: finalLDR)
// 11. UI Overlay (reads: finalLDR; writes: swapchain backbuffer)
```

### Resource Barrier Best Practices
D3D12 requires explicit resource state transitions. Bad barrier management = GPU stalls.

```
RULES:
- Batch barriers: combine multiple ResourceBarrier calls into one
- Split barriers: use D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY / END_ONLY
  for overlapping transitions (GPU can do other work during transition)
- Aliasing barriers: reuse memory for non-overlapping resources
  (e.g., shadow map memory reused for SSAO after shadow pass completes)
- NEVER transition a resource more than once per pass
- Track resource states in the render graph, not manually
```

### Descriptor Management
```
Strategy: Bindless rendering via shader-visible descriptor heap
- Create ONE large CBV/SRV/UAV heap (1,000,000 descriptors)
- All textures, buffers get permanent slots at load time
- Shaders access via ResourceDescriptorHeap[index] (SM 6.6)
- No per-draw descriptor table switches = massive CPU savings
- Material ID → index into descriptor heap
- Eliminates 90% of descriptor-related API calls
```

---

## PART 2: GPU-DRIVEN RENDERING

The goal: CPU sends camera info, GPU does everything else.

### The Pipeline (from PlayerUnknown Productions)

```
CPU per frame:
  1. Upload camera matrices
  2. Upload any changed object transforms
  3. Call ExecuteIndirect (ONE call for entire scene)

GPU per frame (compute shaders):
  1. CLEAR: Reset counters and instance buffers
  2. CULL: Frustum + occlusion cull all objects
     - Read object bounding boxes from structured buffer
     - Test against frustum planes (6 dot products per object)
     - Test against hierarchical Z-buffer (HiZ) from previous frame
     - Output: visible object list
  3. COUNT: Count visible instances per render item
  4. COMPACT: Compute offsets, build indirect draw arguments
  5. SORT: Sort by PSO to minimize state switches
  6. DRAW: ExecuteIndirect processes the draw buffer
```

### Frustum Culling (Compute Shader)

```hlsl
// frustum_cull.hlsl — GPU frustum culling
// Tests AABB against 6 frustum planes

bool FrustumTest(float3 center, float3 extents, float4 planes[6])
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        float3 absNormal = abs(planes[i].xyz);
        float dist = dot(center, planes[i].xyz) + planes[i].w;
        float radius = dot(extents, absNormal);
        if (dist + radius < 0) return false; // Fully outside this plane
    }
    return true;
}
```

### Occlusion Culling (HiZ)

```
Hierarchical Z-Buffer:
1. After depth prepass, generate HiZ mip chain (half resolution each level)
2. For each object's screen-space bounding box:
   a. Compute which HiZ mip level covers the entire bounding box
   b. Sample that mip level
   c. If object's nearest Z > HiZ value, it's occluded → skip
3. This catches objects behind walls, terrain, ships, etc.
4. Uses PREVIOUS frame's depth (1 frame latency, acceptable)
```

### LOD Selection on GPU

```hlsl
// Select LOD based on screen-space size
uint SelectLOD(float3 center, float radius, float4x4 viewProj, float screenHeight)
{
    float4 clip = mul(viewProj, float4(center, 1.0));
    float screenSize = radius * screenHeight / clip.w;

    if (screenSize > 200.0) return 0;  // LOD0: > 200 pixels
    if (screenSize > 50.0)  return 1;  // LOD1: 50-200 pixels
    if (screenSize > 10.0)  return 2;  // LOD2: 10-50 pixels
    return 3;                           // LOD3: < 10 pixels (billboard)
}
```

---

## PART 3: ADVANCED RENDERING TECHNIQUES

### Clustered Forward+ Lighting
Subdivide the view frustum into 3D clusters (16×16 pixels × depth slices).
Assign lights to clusters. Each pixel only evaluates lights in its cluster.

```
Cluster dimensions: 16×16 pixels × 24 depth slices (logarithmic)
At 1920×1080: (120 × 68 × 24) = 195,840 clusters
Each cluster: stores up to 256 light indices
Light assignment: compute shader iterates all lights, tests against each cluster
Per-pixel cost: only evaluate 0-20 lights instead of all scene lights
Supports: 1000+ dynamic lights per scene at 60fps
```

### Temporal Anti-Aliasing (TAA)
```
Per frame:
1. Jitter projection matrix by sub-pixel offset (Halton sequence)
   Halton(2,3) offsets: (0.5,0.33), (0.25,0.67), (0.75,0.11), (0.125,0.44)...
2. Render scene with jittered projection
3. Reproject previous frame using motion vectors
4. Blend: current 10% + reprojected previous 90%
5. Neighborhood clamping: clamp reprojected color to min/max of 3×3 current pixels
   (prevents ghosting on disoccluded pixels)
6. Detect disocclusion: if motion vector points to very different depth, increase
   current frame weight to 50% (faster convergence on new surfaces)
```

### Mesh Shaders (SM 6.5+)
Replace vertex + geometry shaders with amplification + mesh shaders.
```
Amplification Shader (per meshlet group):
  - Frustum cull meshlet
  - Backface cull meshlet (cone test)
  - LOD select
  - If visible: DispatchMesh() → launch mesh shader

Mesh Shader (per meshlet, 64-128 triangles):
  - Read meshlet vertex/index data from buffer
  - Transform vertices
  - Output primitives directly (no intermediate vertex buffer)

Benefits: 10-40% triangle throughput improvement
Meshlet size: 64 vertices / 124 triangles (NVIDIA optimal)
                64 vertices / 126 triangles (AMD optimal)
```

---

## PART 4: PERFORMANCE TARGETS

| Metric | Target | Notes |
|---|---|---|
| **Frame time** | <16.6ms (60fps) or <6.9ms (144fps) | Measured on mid-range GPU |
| **Draw calls** | <2000 traditional, or 1 ExecuteIndirect | GPU-driven = 1 call |
| **Triangle count** | 5-20 million visible per frame | After culling |
| **Texture memory** | <4GB VRAM for all resident textures | Streaming for rest |
| **Shadow maps** | 4 cascades × 2048² | CSM for sun, 512² for point lights |
| **GBuffer** | 3 render targets + depth | Albedo+Metal, Normal+Rough, Motion+Emissive |
| **Post-process** | <3ms total | Bloom 0.3ms, TAA 0.5ms, tonemap 0.2ms, DOF 0.5ms |
| **UI rendering** | <1ms | Immediate mode, batched quads |

### GPU Timing Breakdown (Target Budget)

```
Shadow pass:        2.0ms
Depth prepass:      0.5ms
GBuffer:            3.0ms
SSAO:               1.0ms
Lighting:           2.0ms
Volumetric clouds:  1.5ms
Atmosphere/sky:     0.5ms
SSR:                1.0ms
Bloom:              0.3ms
TAA:                0.5ms
Tonemap + UI:       0.5ms
─────────────────────────
Total:             12.8ms → 78fps headroom
```

---

## PART 5: CURRICULUM STEP UPDATES

### Enhanced Steps for Depth Pass A (1001-1150):

**Step 1001**: Create ResourceManager with ring buffer allocator for dynamic CBVs.
Triple-buffer constants (3 frames in flight). Allocator block size: 256KB.

**Step 1005**: Implement bindless descriptor heap. Allocate 100,000 SRV slots.
Materials reference descriptors by uint32 index. Shader: `Texture2D tex = ResourceDescriptorHeap[materialData.albedoIndex];`

**Step 1015**: Render graph framework. RenderPass struct with read/write declarations.
Automatic barrier insertion from dependency analysis. Aliasing support for transient resources.

**Step 1031**: GPU frustum culling compute shader. StructuredBuffer of object AABBs.
Upload frustum planes as CBV. Output: AppendStructuredBuffer of visible indices.

**Step 1035**: HiZ occlusion culling. Generate HiZ mip chain (compute shader, each mip = max of 2×2 parent).
Sample at appropriate mip for each object's screen-space extent.

**Step 1040**: ExecuteIndirect pipeline. CPU fills DrawIndexedArguments buffer once at load.
GPU compute updates InstanceCount per draw. One ExecuteIndirect call renders entire scene.

**Step 1045**: LOD selection on GPU. Compute shader selects LOD per object based on screen-space radius.
Write LOD index to instance data buffer. Meshes have LOD variants in contiguous index buffer ranges.

**Step 1061**: Clustered Forward+ light assignment. 3D cluster grid (16×16×24 depth slices).
Compute shader assigns lights to clusters. Light index lists stored in UAV buffer.
Max 256 lights per cluster, 4096 total scene lights supported.

**Step 1075**: Shadow mapping with 4-cascade CSM. Cascade splits: 5m, 20m, 80m, 320m.
Stabilize cascades (round to texel grid to prevent shimmer). PCF 3×3 sampling.

**Step 1101**: TAA implementation. Halton(2,3) jitter sequence. Motion vector buffer from GBuffer pass.
Neighborhood clamping with variance-based weight. Sharpen pass after TAA to counteract blur.

**Step 1105**: Bloom. Downsample to 1/2, 1/4, 1/8, 1/16, 1/32 using 13-tap Karis average (prevents fireflies).
Upsample with bilinear tent filter. Additive blend with threshold 1.0 (HDR values only bloom).

**Step 1110**: SSAO (GTAO or HBAO+). 16-32 samples per pixel in screen-space hemisphere.
Temporal accumulation with previous frame. Bilateral blur to preserve edges. Strength: 1.0-1.5.

**Step 1131**: Terrain clipmap rendering. 5 clipmap levels centered on camera.
Each level: 255×255 grid, 2× scale of previous. Transition blend in 16-vertex border.
GPU tessellation for close terrain. Virtual texturing for materials.

---

*Batch 1, Topic 1 complete. See also: PBR Material Bible for shader values,
Atmosphere Bible for sky rendering, Shader Recipes for special materials.*
