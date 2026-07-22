// =============================================================================
// terrain/cube_sphere.h — cube-sphere mapping for chunked-LOD planetary terrain
// =============================================================================
// A planet's surface is a quadtree over the 6 faces of a cube, each face point
// projected onto the sphere. We use the TANGENT (equiangular) warp — a' = tan(a *
// pi/4) — rather than a naive normalize(), so arc steps are near-uniform and the
// corner chunks stay comparable in size to the centre chunks (naive projection
// stretches corners and wastes the triangle budget there).
//
// Everything is in DOUBLE: directions are unit Vec3d, so the caller can displace
// by the true planet radius (~6.4e6 m) and subtract a per-chunk origin before
// narrowing to float (RULE 1 at patch granularity — see chunk_mesh.h).
// =============================================================================

#ifndef DAWNING_TERRAIN_CUBE_SPHERE_H
#define DAWNING_TERRAIN_CUBE_SPHERE_H

#include "core/types.h"

namespace terrain
{

// The 6 cube faces, by outward axis.
enum class CubeFace : int
{
    PosX = 0, NegX = 1,
    PosY = 2, NegY = 3,
    PosZ = 4, NegZ = 5,
    Count = 6
};

// Map a face-local coordinate (u,v) in [-1,1]^2 to a UNIT sphere direction via the
// tangent warp. (0,0) is the face centre (its outward axis); the four corners map
// to the cube corners on the sphere. Continuous and bijective within a face; the
// shared edges of adjacent faces map to the same great-circle arc (so a chunk on
// one face and its neighbour on the next meet with no gap — the cross-face
// adjacency that P2 builds relies on this).
core::Vec3d FaceToDirection(CubeFace face, double u, double v);

// The outward axis of a face (its centre direction).
core::Vec3d FaceAxis(CubeFace face);

} // namespace terrain

#endif // DAWNING_TERRAIN_CUBE_SPHERE_H
