// =============================================================================
// terrain/planet_quadtree.cpp — see planet_quadtree.h
// =============================================================================

#include "planet_quadtree.h"

#include <cmath>

namespace terrain
{
namespace
{

// A node's angular half-span (radians) at a given level. A cube face spans a
// quarter turn (pi/2) across its full [-1,1] range; the equiangular (tangent) warp
// makes that span near-linear in angle, so a level-L node spans (pi/2)/2^L, half
// of that either side of centre.
double NodeAngularSpan(int level)
{
    return (core::PI * 0.5) / static_cast<double>(1ll << level);
}

// Nearest point of a node (on the mean sphere) to the camera direction, used for
// the distance term. We approximate with the node centre direction — adequate for
// LOD selection and cheap. Returns the surface point (mean sphere, no displacement).
core::Vec3d NodeCentreSurface(const QuadtreeConfig& cfg, CubeFace face,
                              double u0, double u1, double v0, double v1)
{
    const core::Vec3d dir = FaceToDirection(face, 0.5 * (u0 + u1), 0.5 * (v0 + v1));
    return { dir.x * cfg.planetRadius, dir.y * cfg.planetRadius, dir.z * cfg.planetRadius };
}

void Recurse(const QuadtreeConfig& cfg, const core::Vec3d& cam,
             CubeFace face, double u0, double u1, double v0, double v1, int level,
             std::vector<QuadPatch>& out)
{
    if (static_cast<int>(out.size()) >= cfg.maxLeaves)
    {
        out.push_back({ face, u0, u1, v0, v1, level });
        return;
    }

    const bool atMax = (level >= cfg.maxLevel);
    if (!atMax)
    {
        const core::Vec3d c = NodeCentreSurface(cfg, face, u0, u1, v0, v1);
        const core::Vec3d d{ cam.x - c.x, cam.y - c.y, cam.z - c.z };
        const double dist = d.Length();
        // Camera-below-surface / degenerate: force the deepest split.
        const double safeDist = (dist > 1.0) ? dist : 1.0;

        const double delta = NodeGeometricError(cfg, level);
        const double projScale = cfg.viewportHeight / (2.0 * cfg.tanHalfFovY);
        const double screenError = (delta / safeDist) * projScale;

        if (screenError > cfg.pixelError)
        {
            const double um = 0.5 * (u0 + u1);
            const double vm = 0.5 * (v0 + v1);
            Recurse(cfg, cam, face, u0, um, v0, vm, level + 1, out);
            Recurse(cfg, cam, face, um, u1, v0, vm, level + 1, out);
            Recurse(cfg, cam, face, u0, um, vm, v1, level + 1, out);
            Recurse(cfg, cam, face, um, u1, vm, v1, level + 1, out);
            return;
        }
    }
    out.push_back({ face, u0, u1, v0, v1, level });
}

} // namespace

double NodeGeometricError(const QuadtreeConfig& cfg, int level)
{
    // Chord-arc sagitta of the node's angular half-span on the (radius+amplitude)
    // sphere, plus a terrain-detail term that also shrinks with level.
    const double halfSpan = 0.5 * NodeAngularSpan(level);
    const double R = cfg.planetRadius + cfg.amplitude;
    const double sagitta = R * (1.0 - std::cos(halfSpan));
    const double detail  = cfg.amplitude / static_cast<double>(1ll << level);
    return sagitta + detail;
}

void SelectQuadtreeLOD(const QuadtreeConfig& cfg,
                       const core::Vec3d& cameraBodyPos,
                       std::vector<QuadPatch>& out)
{
    out.clear();
    const CubeFace faces[6] = {
        CubeFace::PosX, CubeFace::NegX, CubeFace::PosY,
        CubeFace::NegY, CubeFace::PosZ, CubeFace::NegZ,
    };
    for (CubeFace f : faces)
        Recurse(cfg, cameraBodyPos, f, -1.0, 1.0, -1.0, 1.0, 0, out);
}

} // namespace terrain
