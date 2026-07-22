// =============================================================================
// gameplay/targeting.cpp — see targeting.h
// =============================================================================
#include "targeting.h"

#include <algorithm>
#include <cmath>

namespace gameplay
{

TargetInfo ComputeTargetInfo(const core::Vec3d& shipPos, const core::Vec3d& shipVel,
                             const TargetCandidate& c)
{
    TargetInfo info;
    info.valid    = true;
    info.id       = c.id;
    info.name     = c.name;
    info.relation = c.relation;

    const core::Vec3d los{ c.worldPos.x - shipPos.x,
                           c.worldPos.y - shipPos.y,
                           c.worldPos.z - shipPos.z };
    const double range = std::sqrt(los.x * los.x + los.y * los.y + los.z * los.z);
    info.rangeMeters = range;

    const core::Vec3d relVel{ c.worldVel.x - shipVel.x,
                              c.worldVel.y - shipVel.y,
                              c.worldVel.z - shipVel.z };
    info.relativeSpeed = std::sqrt(relVel.x * relVel.x + relVel.y * relVel.y + relVel.z * relVel.z);

    if (range > 1e-6)
    {
        const double invR = 1.0 / range;
        // closingSpeed = -(V_rel . losHat): positive means the range is shrinking.
        info.closingSpeed = -((relVel.x * los.x + relVel.y * los.y + relVel.z * los.z) * invR);
    }
    return info;
}

FiringSolution ComputeFiringSolution(const core::Vec3d& shipPos, const core::Vec3d& shipVel,
                                     const core::Vec3d& targetPos, const core::Vec3d& targetVel,
                                     double projectileSpeed)
{
    FiringSolution sol;
    if (projectileSpeed <= 0.0) return sol;

    // Work in the shooter's frame (projectile inherits ship velocity): the target
    // starts at d = targetPos - shipPos and moves at V = targetVel - shipVel; the
    // round travels at `w` from the origin. Intercept: |d + V t| = w t, squared to
    // a t^2 + b t + c = 0.
    const core::Vec3d d{ targetPos.x - shipPos.x, targetPos.y - shipPos.y, targetPos.z - shipPos.z };
    const core::Vec3d V{ targetVel.x - shipVel.x, targetVel.y - shipVel.y, targetVel.z - shipVel.z };
    const double w   = projectileSpeed;
    const double vv  = V.x * V.x + V.y * V.y + V.z * V.z;
    const double dv  = d.x * V.x + d.y * V.y + d.z * V.z;
    const double dd  = d.x * d.x + d.y * d.y + d.z * d.z;
    const double a   = vv - w * w;
    const double b   = 2.0 * dv;
    const double c   = dd;

    double t = -1.0;
    const double eps = 1e-9 * std::max(1.0, w * w);
    if (std::fabs(a) < eps)
    {
        // Degenerate: target's relative speed ~ muzzle speed -> linear b t + c = 0.
        if (std::fabs(b) > 1e-12) { const double tl = -c / b; if (tl > 0.0) t = tl; }
    }
    else
    {
        const double disc = b * b - 4.0 * a * c;
        if (disc >= 0.0)
        {
            const double sq = std::sqrt(disc);
            // Numerically stable roots (avoid catastrophic cancellation).
            const double q  = -0.5 * (b + (b >= 0.0 ? sq : -sq));
            const double t1 = q / a;
            const double t2 = (std::fabs(q) > 1e-300) ? c / q : -1.0;
            // Smallest strictly-positive root.
            if (t1 > 0.0) t = t1;
            if (t2 > 0.0 && (t < 0.0 || t2 < t)) t = t2;
        }
    }

    if (t <= 0.0) return sol; // target outruns the round on this geometry
    sol.valid        = true;
    sol.timeToImpact = t;
    sol.worldPos     = { targetPos.x + V.x * t, targetPos.y + V.y * t, targetPos.z + V.z * t };
    return sol;
}

int FindCandidate(const std::vector<TargetCandidate>& candidates, uint64_t id)
{
    for (size_t i = 0; i < candidates.size(); ++i)
        if (candidates[i].id == id) return static_cast<int>(i);
    return -1;
}

uint64_t SelectNearest(const std::vector<TargetCandidate>& candidates,
                       const core::Vec3d& shipPos, uint64_t excludeId)
{
    uint64_t best = 0;
    double bestD2 = 0.0;
    for (const TargetCandidate& c : candidates)
    {
        if (c.id == excludeId) continue;
        const double dx = c.worldPos.x - shipPos.x;
        const double dy = c.worldPos.y - shipPos.y;
        const double dz = c.worldPos.z - shipPos.z;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (best == 0 || d2 < bestD2) { best = c.id; bestD2 = d2; }
    }
    return best;
}

uint64_t CycleTarget(const std::vector<TargetCandidate>& candidates,
                     uint64_t currentId, int dir)
{
    if (candidates.empty()) return 0;

    // Deterministic ascending-id order, independent of scene iteration order.
    std::vector<uint64_t> ids;
    ids.reserve(candidates.size());
    for (const TargetCandidate& c : candidates) ids.push_back(c.id);
    std::sort(ids.begin(), ids.end());

    if (currentId == 0) return dir >= 0 ? ids.front() : ids.back();

    auto it = std::find(ids.begin(), ids.end(), currentId);
    if (it == ids.end()) return dir >= 0 ? ids.front() : ids.back();

    const int n = static_cast<int>(ids.size());
    int idx = static_cast<int>(it - ids.begin());
    idx = ((idx + (dir >= 0 ? 1 : -1)) % n + n) % n;
    return ids[static_cast<size_t>(idx)];
}

} // namespace gameplay
