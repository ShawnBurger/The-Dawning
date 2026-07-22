// =============================================================================
// terrain/planet_quadtree.cpp — see planet_quadtree.h
// =============================================================================

#include "planet_quadtree.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>

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
    const int safeLevel = std::clamp(level, 0, kMaxQuadtreeLevel);
    return (core::PI * 0.5) * std::ldexp(1.0, -safeLevel);
}

// Nearest point of a node (on the mean sphere) to the camera direction, used for
// the distance term. We approximate with the node centre direction — adequate for
// LOD selection and cheap. Returns the surface point (mean sphere, no displacement).
core::Vec3d NodeCentreSurface(double planetRadius, CubeFace face,
                              double u0, double u1, double v0, double v1)
{
    const core::Vec3d dir = FaceToDirection(face, 0.5 * (u0 + u1), 0.5 * (v0 + v1));
    return { dir.x * planetRadius, dir.y * planetRadius, dir.z * planetRadius };
}

struct Candidate
{
    QuadPatch patch;
    double screenError = 0.0;
    uint64_t order = 0;
};

struct CandidateLess
{
    bool operator()(const Candidate& a, const Candidate& b) const
    {
        if (a.screenError != b.screenError)
            return a.screenError < b.screenError;
        return a.order > b.order;
    }
};

bool IsFinite(const core::Vec3d& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

double ScreenError(const QuadtreeConfig& cfg, const core::Vec3d& cam,
                   const QuadPatch& patch)
{
    const core::Vec3d c = NodeCentreSurface(
        cfg.planetRadius, patch.face, patch.u0, patch.u1, patch.v0, patch.v1);
    const core::Vec3d d{ cam.x - c.x, cam.y - c.y, cam.z - c.z };
    const double distance = d.Length();
    const double safeDist = (distance > 1.0) ? distance : 1.0;
    const double projScale = cfg.viewportHeight / (2.0 * cfg.tanHalfFovY);
    const double screenError =
        (NodeGeometricError(cfg, patch.level) / safeDist) * projScale;
    return std::isfinite(screenError)
        ? (std::max)(0.0, screenError)
        : (std::numeric_limits<double>::max)();
}

bool PatchLess(const QuadPatch& a, const QuadPatch& b)
{
    if (a.face != b.face) return static_cast<int>(a.face) < static_cast<int>(b.face);
    if (a.level != b.level) return a.level < b.level;
    if (a.v0 != b.v0) return a.v0 < b.v0;
    return a.u0 < b.u0;
}

} // namespace

double NodeGeometricError(const QuadtreeConfig& cfg, int level)
{
    // Chord-arc sagitta of the node's angular half-span on the (radius+amplitude)
    // sphere, plus a terrain-detail term that also shrinks with level.
    const int safeLevel = std::clamp(level, 0, kMaxQuadtreeLevel);
    const double radius = (std::isfinite(cfg.planetRadius) && cfg.planetRadius > 0.0)
        ? cfg.planetRadius : 0.0;
    const double amplitude = (std::isfinite(cfg.amplitude) && cfg.amplitude > 0.0)
        ? cfg.amplitude : 0.0;
    const double halfSpan = 0.5 * NodeAngularSpan(safeLevel);
    constexpr double maximum = (std::numeric_limits<double>::max)();
    const double R = radius > maximum - amplitude
        ? maximum
        : radius + amplitude;
    const double sagitta = R * (1.0 - std::cos(halfSpan));
    const double detail  = amplitude * std::ldexp(1.0, -safeLevel);
    return sagitta > maximum - detail ? maximum : sagitta + detail;
}

void SelectQuadtreeLOD(const QuadtreeConfig& cfg,
                       const core::Vec3d& cameraBodyPos,
                       std::vector<QuadPatch>& out)
{
    out.clear();
    const QuadtreeConfig defaults;
    QuadtreeConfig safe = cfg;
    safe.planetRadius = (std::isfinite(cfg.planetRadius) && cfg.planetRadius > 0.0)
        ? cfg.planetRadius : defaults.planetRadius;
    safe.amplitude = (std::isfinite(cfg.amplitude) && cfg.amplitude >= 0.0)
        ? cfg.amplitude : defaults.amplitude;
    safe.maxLevel = std::clamp(cfg.maxLevel, 0, kMaxQuadtreeLevel);
    safe.pixelError = (std::isfinite(cfg.pixelError) && cfg.pixelError > 0.0)
        ? cfg.pixelError : defaults.pixelError;
    safe.viewportHeight = (std::isfinite(cfg.viewportHeight) && cfg.viewportHeight > 0.0)
        ? cfg.viewportHeight : defaults.viewportHeight;
    safe.tanHalfFovY = (std::isfinite(cfg.tanHalfFovY) && cfg.tanHalfFovY > 0.0)
        ? cfg.tanHalfFovY : defaults.tanHalfFovY;
    safe.maxLeaves = std::clamp(cfg.maxLeaves, kMinQuadtreeLeaves, kMaxQuadtreeLeaves);

    const CubeFace faces[6] = {
        CubeFace::PosX, CubeFace::NegX, CubeFace::PosY,
        CubeFace::NegY, CubeFace::PosZ, CubeFace::NegZ,
    };
    const bool cameraValid = IsFinite(cameraBodyPos);
    const core::Vec3d cam = cameraValid ? cameraBodyPos : core::Vec3d{};

    std::priority_queue<Candidate, std::vector<Candidate>, CandidateLess> pending;
    uint64_t order = 0;
    for (CubeFace face : faces)
    {
        QuadPatch root{ face, -1.0, 1.0, -1.0, 1.0, 0 };
        pending.push({ root, cameraValid ? ScreenError(safe, cam, root) : 0.0, order++ });
    }

    size_t leafCount = static_cast<size_t>(kMinQuadtreeLeaves);
    out.reserve(static_cast<size_t>(safe.maxLeaves));
    while (!pending.empty())
    {
        Candidate candidate = pending.top();
        pending.pop();
        const QuadPatch& p = candidate.patch;
        const bool canSplit = p.level < safe.maxLevel
            && candidate.screenError > safe.pixelError
            && leafCount + 3 <= static_cast<size_t>(safe.maxLeaves);
        if (!canSplit)
        {
            out.push_back(p);
            continue;
        }

        const double um = 0.5 * (p.u0 + p.u1);
        const double vm = 0.5 * (p.v0 + p.v1);
        const QuadPatch children[4] = {
            { p.face, p.u0, um, p.v0, vm, p.level + 1 },
            { p.face, um, p.u1, p.v0, vm, p.level + 1 },
            { p.face, p.u0, um, vm, p.v1, p.level + 1 },
            { p.face, um, p.u1, vm, p.v1, p.level + 1 },
        };
        for (const QuadPatch& child : children)
            pending.push({ child, ScreenError(safe, cam, child), order++ });
        leafCount += 3;
    }

    std::sort(out.begin(), out.end(), PatchLess);
}

} // namespace terrain
