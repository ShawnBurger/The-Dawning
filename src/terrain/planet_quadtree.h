// =============================================================================
// terrain/planet_quadtree.h — camera-adaptive LOD selection for planet terrain
// =============================================================================
// A planet's surface is a quadtree over the 6 cube faces. This selects the leaf
// set to render for a given camera by the Ulrich screen-space-error rule: a node
// whose coarse mesh would deviate from the true surface by more than a few pixels
// (given its distance to the camera) is subdivided; otherwise it is emitted as a
// leaf. Near the camera this drives deep subdivision (fine mesh); far away, a
// single coarse patch per face.
//
// Stateless (recomputed per query). The stateful streaming/pooling + split-merge
// hysteresis that a smoothly-moving camera needs layers on top of this; the leaf
// SELECTION is the load-bearing math and is unit-tested here. Cracks between
// different-level neighbours are expected and are closed later by skirts.
// =============================================================================

#ifndef DAWNING_TERRAIN_PLANET_QUADTREE_H
#define DAWNING_TERRAIN_PLANET_QUADTREE_H

#include "cube_sphere.h"
#include "core/types.h"

#include <vector>

namespace terrain
{

// Terrain leaf keys reserve 24 bits per face-local cell coordinate. Keeping the
// selector inside that range also prevents unsafe integer shifts and runaway
// subdivision when a configuration is loaded from malformed data.
inline constexpr int kMaxQuadtreeLevel = 24;
inline constexpr int kMinQuadtreeLeaves = 6;       // one root for each cube face
inline constexpr int kMaxQuadtreeLeaves = 65536;  // bounded CPU/memory work per query

struct QuadtreeConfig
{
    double planetRadius   = 6.371e6; // metres
    double amplitude      = 8000.0;  // terrain displacement scale, metres
    int    maxLevel       = 14;      // clamped to [0, kMaxQuadtreeLevel]
    double pixelError      = 2.0;    // screen-space error threshold, pixels
    double viewportHeight  = 1080.0;
    double tanHalfFovY     = 0.5773502692; // tan(30deg) = 60deg vertical FOV
    int    maxLeaves       = 4096;   // hard cap, clamped to the supported range above
};

// A selected leaf patch (a face sub-region at some subdivision level).
struct QuadPatch
{
    CubeFace face;
    double   u0, u1, v0, v1;
    int      level;
};

// World-space geometric error of a node at `level`: the deviation of its coarse
// (chord) mesh from the displaced sphere. Chord-arc sagitta of the node's angular
// span plus the terrain amplitude, halving with level. Invalid levels and
// non-finite/negative radius inputs are sanitized to the supported domain.
double NodeGeometricError(const QuadtreeConfig& cfg, int level);

// Select the leaf set for a camera at `cameraBodyPos` (body space, relative to the
// planet centre). Appends QuadPatches to `out` (cleared first).
void SelectQuadtreeLOD(const QuadtreeConfig& cfg,
                       const core::Vec3d& cameraBodyPos,
                       std::vector<QuadPatch>& out);

} // namespace terrain

#endif // DAWNING_TERRAIN_PLANET_QUADTREE_H
