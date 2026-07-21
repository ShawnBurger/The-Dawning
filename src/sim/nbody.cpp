// =============================================================================
// sim/nbody.cpp — N-body gravity, Forest-Ruth integrator, softening, LOD
// =============================================================================
// See nbody.h for the module contract, the shared-softening policy, the
// determinism mechanism, and the integrator-boundary (operator-split) rules.
// =============================================================================

#include "nbody.h"

#include <algorithm>
#include <cmath>

namespace sim
{

// -----------------------------------------------------------------------------
// Shared softening policy
// -----------------------------------------------------------------------------
double SofteningLength(double mu, double radius)
{
    // r_s = 2*mu/c^2 (Schwarzschild radius; mu = G*M). eps = max(radius, r_s + eps0).
    const double rs = 2.0 * mu / (kSpeedOfLight * kSpeedOfLight);
    const double floorTerm = rs + kSofteningBase;
    return (radius > floorTerm) ? radius : floorTerm;
}

// -----------------------------------------------------------------------------
// Forest-Ruth coefficients (theta = 1/(2 - 2^(1/3)))
// -----------------------------------------------------------------------------
ForestRuthCoefficients GetForestRuthCoefficients()
{
    const double theta = 1.0 / (2.0 - std::cbrt(2.0));
    ForestRuthCoefficients k{};
    k.c[0] = theta * 0.5;
    k.c[1] = (1.0 - theta) * 0.5;
    k.c[2] = (1.0 - theta) * 0.5;
    k.c[3] = theta * 0.5;
    k.d[0] = theta;
    k.d[1] = 1.0 - 2.0 * theta;
    k.d[2] = theta;
    k.d[3] = 0.0;
    return k;
}

// -----------------------------------------------------------------------------
// Deterministic summation order
// -----------------------------------------------------------------------------
std::vector<uint32_t> DeterministicOrder(const std::vector<NBodyParticle>& bodies)
{
    std::vector<uint32_t> order(bodies.size());
    for (uint32_t i = 0; i < order.size(); ++i)
        order[i] = i;
    // Sort by stable bodyId. A tie-break on the index keeps it a total order even
    // if two ids collide (a caller bug), so the sort is still deterministic.
    std::sort(order.begin(), order.end(),
              [&bodies](uint32_t a, uint32_t b)
              {
                  if (bodies[a].bodyId != bodies[b].bodyId)
                      return bodies[a].bodyId < bodies[b].bodyId;
                  return a < b;
              });
    return order;
}

// -----------------------------------------------------------------------------
// Force / acceleration sum (deterministic, softened, pairwise equal/opposite)
// -----------------------------------------------------------------------------
void ComputeAccelerations(const std::vector<NBodyParticle>& bodies,
                          const std::vector<uint32_t>& order,
                          std::vector<Vec3d>& accelOut)
{
    accelOut.assign(bodies.size(), Vec3d{ 0.0, 0.0, 0.0 });

    for (size_t ii = 0; ii < order.size(); ++ii)
    {
        const uint32_t i = order[ii];
        const NBodyParticle& bi = bodies[i];

        for (size_t jj = ii + 1; jj < order.size(); ++jj)
        {
            const uint32_t j = order[jj];
            const NBodyParticle& bj = bodies[j];

            // Two test particles do not interact.
            if (!bi.isSource && !bj.isSource)
                continue;

            // Separation d points from j to i. Positions are already in the common
            // frame (small), so this direct subtraction is exact — no cross-frame
            // catastrophic cancellation (the whole point of the Stage-0 layer).
            const Vec3d d = bi.position - bj.position;

            // ONE shared Plummer softening for the pair (symmetric so the pairwise
            // force is exactly equal/opposite): eps = max of the two bodies' eps.
            const double eps = (bi.softening > bj.softening) ? bi.softening : bj.softening;
            const double r2 = d.LengthSq() + eps * eps;
            const double invR = 1.0 / std::sqrt(r2);
            const double invR3 = invR / r2; // (r^2+eps^2)^-1.5, finite as r->0

            // Shared geometric factor, evaluated ONCE.
            const Vec3d g = d * invR3;

            // Equal/opposite application. g points j->i, so i is pulled -mu_j*g
            // (toward j) and j is pulled +mu_i*g (toward i).
            if (bj.isSource) accelOut[i] -= g * bj.mu;
            if (bi.isSource) accelOut[j] += g * bi.mu;
        }
    }
}

namespace
{
void Drift(std::vector<NBodyParticle>& bodies, double h)
{
    for (NBodyParticle& b : bodies)
        b.position += b.velocity * h;
}

void Kick(std::vector<NBodyParticle>& bodies, const std::vector<Vec3d>& accel, double h)
{
    for (size_t i = 0; i < bodies.size(); ++i)
        bodies[i].velocity += accel[i] * h;
}
} // namespace

// -----------------------------------------------------------------------------
// Forest-Ruth step
// -----------------------------------------------------------------------------
void StepNBody(std::vector<NBodyParticle>& bodies, double dt)
{
    // House guard (matches rigid_body.cpp:118): dt<=0 or non-finite is a no-op.
    if (!(dt > 0.0) || !std::isfinite(dt) || bodies.empty())
        return;

    const std::vector<uint32_t> order = DeterministicOrder(bodies);
    const ForestRuthCoefficients k = GetForestRuthCoefficients();
    std::vector<Vec3d> accel;

    // drift c0, kick d0, drift c1, kick d1, drift c2, kick d2, drift c3 (d3==0).
    for (int stage = 0; stage < 3; ++stage)
    {
        Drift(bodies, k.c[stage] * dt);
        ComputeAccelerations(bodies, order, accel);
        Kick(bodies, accel, k.d[stage] * dt);
    }
    Drift(bodies, k.c[3] * dt);
}

// -----------------------------------------------------------------------------
// Explicit (forward) Euler — the NON-symplectic negative control
// -----------------------------------------------------------------------------
void StepNBodyExplicitEuler(std::vector<NBodyParticle>& bodies, double dt)
{
    if (!(dt > 0.0) || !std::isfinite(dt) || bodies.empty())
        return;

    const std::vector<uint32_t> order = DeterministicOrder(bodies);
    std::vector<Vec3d> accel;
    ComputeAccelerations(bodies, order, accel); // a(x_n) — BEFORE any update

    // x_{n+1} = x_n + v_n dt ; v_{n+1} = v_n + a(x_n) dt. Both from OLD state, so
    // this is forward Euler (not the symplectic semi-implicit form) and WILL drift.
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        const Vec3d vOld = bodies[i].velocity;
        bodies[i].position += vOld * dt;
        bodies[i].velocity += accel[i] * dt;
    }
}

// -----------------------------------------------------------------------------
// Point-probe gravity (rigid-body operator-split lane)
// -----------------------------------------------------------------------------
Vec3d GravityAccelerationAt(const std::vector<NBodyParticle>& bodies,
                            const Vec3d& point, double pointSoftening,
                            uint64_t selfBodyId)
{
    const std::vector<uint32_t> order = DeterministicOrder(bodies);
    Vec3d a{ 0.0, 0.0, 0.0 };
    for (uint32_t idx : order)
    {
        const NBodyParticle& b = bodies[idx];
        if (!b.isSource || b.bodyId == selfBodyId)
            continue;
        const Vec3d d = point - b.position; // from source to point
        const double eps = (pointSoftening > b.softening) ? pointSoftening : b.softening;
        const double r2 = d.LengthSq() + eps * eps;
        const double invR3 = 1.0 / (r2 * std::sqrt(r2));
        a -= d * (b.mu * invR3);
    }
    return a;
}

// -----------------------------------------------------------------------------
// Conserved-quantity diagnostics (G-scaled: masses expressed as mu/G)
// -----------------------------------------------------------------------------
double TotalEnergyGScaled(const std::vector<NBodyParticle>& bodies)
{
    double ke = 0.0;
    for (const NBodyParticle& b : bodies)
        ke += 0.5 * b.mu * b.velocity.LengthSq();

    double pe = 0.0;
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        for (size_t j = i + 1; j < bodies.size(); ++j)
        {
            if (!bodies[i].isSource || !bodies[j].isSource)
                continue;
            const Vec3d d = bodies[i].position - bodies[j].position;
            const double eps = (bodies[i].softening > bodies[j].softening)
                                   ? bodies[i].softening : bodies[j].softening;
            const double r = std::sqrt(d.LengthSq() + eps * eps);
            // Plummer-softened potential, gradient-consistent with the force.
            pe -= bodies[i].mu * bodies[j].mu / r;
        }
    }
    return ke + pe;
}

Vec3d TotalAngularMomentumGScaled(const std::vector<NBodyParticle>& bodies)
{
    Vec3d L{ 0.0, 0.0, 0.0 };
    for (const NBodyParticle& b : bodies)
        L += b.position.Cross(b.velocity) * b.mu;
    return L;
}

Vec3d TotalLinearMomentumGScaled(const std::vector<NBodyParticle>& bodies)
{
    Vec3d p{ 0.0, 0.0, 0.0 };
    for (const NBodyParticle& b : bodies)
        p += b.velocity * b.mu;
    return p;
}

// -----------------------------------------------------------------------------
// One-owner guard + close-encounter detection
// -----------------------------------------------------------------------------
bool OwnersDisjoint(const std::vector<NBodyParticle>& active,
                    const std::vector<uint64_t>& onRailsIds)
{
    for (const NBodyParticle& b : active)
        for (uint64_t id : onRailsIds)
            if (b.bodyId == id)
                return false; // a body owned by BOTH movers — double-count hazard
    return true;
}

bool DetectCloseEncounter(const std::vector<NBodyParticle>& bodies, double dt)
{
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        for (size_t j = i + 1; j < bodies.size(); ++j)
        {
            // Encounters only matter where at least one body is a real (radius'd)
            // gravitational source; two point test particles never "collide" here.
            if (!bodies[i].isSource && !bodies[j].isSource)
                continue;

            const Vec3d d = bodies[i].position - bodies[j].position;
            const double sep = d.Length();
            const Vec3d relV = bodies[i].velocity - bodies[j].velocity;
            const double closing = relV.Length() * dt; // distance closed in one step

            // Flag if they are within contact + one step's closing distance. The
            // softening length (>= physical radius by construction) is the contact
            // proxy the particle carries, so a fast flyby is caught before it
            // tunnels through the softened core in a single fixed step.
            const double reach = bodies[i].softening + bodies[j].softening + closing;
            if (sep < reach)
                return true;
        }
    }
    return false;
}

// -----------------------------------------------------------------------------
// LOD transition — promote (rails -> N-body) / demote (N-body -> rails)
// -----------------------------------------------------------------------------
StateVector PromoteFromRails(const ecs::OrbitState& rails, double dtSinceEpoch)
{
    ecs::OrbitalElements el = rails.elements;

    if (dtSinceEpoch != 0.0)
    {
        if (el.eccentricity < 1.0)
        {
            // Elliptic: advance the elements analytically (Markley under the hood).
            el = PropagateElements(el, rails.primaryMu, dtSinceEpoch);
        }
        else
        {
            // Hyperbolic / near-parabolic: propagate the state with universal vars.
            const StateVector s0 = ElementsToState(el, rails.primaryMu);
            return PropagateUniversal(s0, rails.primaryMu, dtSinceEpoch);
        }
    }
    return ElementsToState(el, rails.primaryMu);
}

ecs::OrbitState DemoteToRails(const StateVector& primaryRelative, double primaryMu,
                              uint64_t primaryBodyId, double now)
{
    ecs::OrbitState rails;
    rails.elements      = StateToElements(primaryRelative, primaryMu);
    rails.primaryMu     = primaryMu;
    rails.primaryBodyId = primaryBodyId;
    rails.epoch         = now;
    return rails;
}

} // namespace sim
