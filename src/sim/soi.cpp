// =============================================================================
// sim/soi.cpp — sphere-of-influence radii, dominant-SOI membership, and the
// continuity-preserving repatch. See soi.h for the design contract.
// =============================================================================

#include "soi.h"

#include "kepler.h"  // StateVector
#include "nbody.h"   // DemoteToRails

#include <cmath>
#include <cstddef>
#include <limits>

namespace sim {

double SphereOfInfluenceRadius(double orbitSemiMajorAxis, double bodyMu,
                               double primaryMu)
{
    if (!std::isfinite(orbitSemiMajorAxis) || orbitSemiMajorAxis <= 0.0 ||
        !std::isfinite(bodyMu) || bodyMu <= 0.0 ||
        !std::isfinite(primaryMu) || primaryMu <= 0.0)
        return 0.0;
    // r_SOI = a · (mu_body / mu_primary)^(2/5). The mu ratio carries the mass
    // ratio (G cancels), so this is exact for gravitational parameters.
    return orbitSemiMajorAxis * std::pow(bodyMu / primaryMu, 0.4);
}

double HillRadius(double orbitSemiMajorAxis, double eccentricity, double bodyMu,
                  double primaryMu)
{
    if (!std::isfinite(orbitSemiMajorAxis) || orbitSemiMajorAxis <= 0.0 ||
        !std::isfinite(eccentricity) || eccentricity < 0.0 ||
        eccentricity >= 1.0 ||  // an unbound arc has no stable Hill sphere
        !std::isfinite(bodyMu) || bodyMu <= 0.0 ||
        !std::isfinite(primaryMu) || primaryMu <= 0.0)
        return 0.0;
    // r_H = a·(1 − e) · (mu_body / (3·mu_primary))^(1/3) — the periapsis form.
    return orbitSemiMajorAxis * (1.0 - eccentricity) *
           std::pow(bodyMu / (3.0 * primaryMu), 1.0 / 3.0);
}

namespace {

// The TIGHTEST well whose EFFECTIVE sphere (effRadius[i]) contains `where`:
// smallest TRUE soiRadius wins, tie-broken by smaller bodyId. A valid SOI
// hierarchy is strictly nested — a child's sphere is always smaller than, and
// inside, its parent's — so "smallest containing sphere" IS "most-local body",
// with no separate depth/parent bookkeeping needed (and the frame tree the caller
// builds wells from is always properly nested). effRadius lets the hysteresis
// wrapper inflate/deflate the CONTAINMENT test while the tie-break stays on the
// physical radius.
uint64_t SelectTightestContaining(const WorldPos& where,
                                  const std::vector<SoiWell>& wells,
                                  const std::vector<double>& effRadius)
{
    uint64_t best = kInvalidSoi;
    double   bestRadius = 0.0;
    for (std::size_t i = 0; i < wells.size(); ++i)
    {
        const double r = effRadius[i];
        if (!(r > 0.0))
            continue;  // no finite sphere (also rejects NaN)
        const double dist = Separation(wells[i].position, where).Length();
        if (!(dist <= r))
            continue;  // outside (also rejects NaN distance)

        const double trueR = wells[i].soiRadius;
        const bool better =
            best == kInvalidSoi || trueR < bestRadius ||
            (trueR == bestRadius && wells[i].bodyId < best);
        if (better)
        {
            best = wells[i].bodyId;
            bestRadius = trueR;
        }
    }
    return best;
}

} // namespace

uint64_t ResolveDominantSoi(const WorldPos& where, const std::vector<SoiWell>& wells)
{
    std::vector<double> effRadius(wells.size());
    for (std::size_t i = 0; i < wells.size(); ++i)
        effRadius[i] = wells[i].soiRadius;
    return SelectTightestContaining(where, wells, effRadius);
}

uint64_t ResolveSoiWithHysteresis(const WorldPos& where,
                                  const std::vector<SoiWell>& wells,
                                  uint64_t currentPrimaryId, double hysteresis)
{
    double h = hysteresis;
    if (!(h >= 0.0))
        h = 0.0;
    if (h >= 1.0)
        h = std::nextafter(1.0, 0.0);  // keep the (1 − h) factor strictly positive

    std::vector<double> effRadius(wells.size());
    for (std::size_t i = 0; i < wells.size(); ++i)
    {
        // The current primary's sphere is inflated (sticky, harder to leave);
        // every other sphere is deflated (harder to enter). An infinite radius
        // stays infinite under either scale.
        const double scale =
            (wells[i].bodyId == currentPrimaryId) ? (1.0 + h) : (1.0 - h);
        effRadius[i] = wells[i].soiRadius * scale;
    }
    return SelectTightestContaining(where, wells, effRadius);
}

ecs::OrbitState Repatch(const WorldPos& bodyPos, const Vec3d& bodyVel,
                        const WorldPos& primaryPos, const Vec3d& primaryVel,
                        double primaryMu, uint64_t newPrimaryBodyId, double now)
{
    // Cancellation-safe primary-relative state. Separation(primaryPos, bodyPos)
    // is bodyPos − primaryPos computed without narrowing the absolute positions.
    const StateVector primaryRelative{
        Separation(primaryPos, bodyPos),
        bodyVel - primaryVel,
    };
    // DemoteToRails re-fits osculating elements about the new primary. Continuity
    // holds because the SAME primary (pos, vel) is subtracted here and added back
    // when PromoteFromRails evaluates the result.
    return DemoteToRails(primaryRelative, primaryMu, newPrimaryBodyId, now);
}

} // namespace sim
