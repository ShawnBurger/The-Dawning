// =============================================================================
// terrain/chunk_mesh.h — CPU generation of one displaced cube-sphere patch
// =============================================================================
// A chunk is a gridN x gridN grid over a face sub-region [u0,u1] x [v0,v1],
// projected to the sphere (cube_sphere.h) and displaced radially by the SHARED
// procedural height (core::PlanetHeight — the same field the planet_ps shader
// tints, so near terrain matches the far sphere).
//
// PRECISION (RULE 1 at patch granularity): the surface point is formed in DOUBLE
// at full planet magnitude (~6.4e6 m), a per-chunk double ORIGIN is subtracted in
// double, and only the small local RESIDUAL is narrowed to float. Never form
// unit*R + camera-relative-centre in float — at Earth radius float32 ULP is ~1 m,
// which is the jitter this arrangement exists to kill.
// =============================================================================

#ifndef DAWNING_TERRAIN_CHUNK_MESH_H
#define DAWNING_TERRAIN_CHUNK_MESH_H

#include "cube_sphere.h"
#include "core/types.h"

#include <cstdint>
#include <vector>

namespace terrain
{

// Chunk indices are uint16_t. A 256x256 grid has exactly 65,536 vertices and is
// therefore the largest grid whose final vertex index (65,535) is representable.
inline constexpr int kMinChunkGridN = 2;
inline constexpr int kMaxChunkGridN = 256;

// What patch to build and what body to build it on.
struct ChunkParams
{
    CubeFace face      = CubeFace::PosZ;
    double   u0        = -1.0;   // face sub-region in [-1,1]
    double   u1        =  1.0;
    double   v0        = -1.0;
    double   v1        =  1.0;
    int      gridN     = 33;     // vertices per edge, clamped to [2, 256]
    double   planetRadius    = 6.371e6;  // metres
    double   amplitudeMeters = 8000.0;   // height field [0,1] * this = displacement m
    int      type      = 0;      // 0 Earth, 1 Mars, 2 Moon, 3 generic
    float    seed      = 11.0f;
    float    seaLevel  = 0.52f;
    float    coastWidth = 0.02f;
};

struct ChunkVertex
{
    core::Vec3f position;  // chunk-LOCAL (relative to ChunkMesh::origin), metres
    core::Vec3f normal;    // unit outward surface normal
    core::Vec2f uv;        // [0,1] within the chunk
};

struct ChunkMesh
{
    core::Vec3d origin;                    // chunk centre on the displaced sphere (world, double)
    std::vector<ChunkVertex> vertices;     // gridN*gridN, positions relative to origin
    std::vector<uint16_t>    indices;      // (gridN-1)^2 * 6, CW for LH front face
};

// Elevation in metres above the mean sphere at a unit direction (float height eval
// to match the shader, scaled to metres).
double SampleHeightMeters(const core::Vec3d& dir, const ChunkParams& p);

// Full world-space surface position (double) at a unit direction.
core::Vec3d SurfacePoint(const core::Vec3d& dir, const ChunkParams& p);

// Generate the chunk. Positions are chunk-local (origin subtracted in double, then
// narrowed); normals are the surface gradient oriented outward.
ChunkMesh GenerateChunk(const ChunkParams& p);

} // namespace terrain

#endif // DAWNING_TERRAIN_CHUNK_MESH_H
