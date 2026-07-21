// =============================================================================
// tests/test_collision.cpp - N-body close-encounter / collision policy
// =============================================================================
// SIM STAGE 5. Drives the SHIPPED sim/collision.{h,cpp} against the design in
// docs/research/RELATIVISTIC_SIM_ARCHITECTURE.md section 6. Pure CPU; no D3D12.
//
// The policy is what nbody.h's DetectCloseEncounter (a system-wide bool) routes to:
// global power-of-two symplectic subdivision with per-micro-step swept contact
// detection, a mass-fraction central-impulse bounce, and a union-find CoM merge,
// with deliberately NO de-penetration operator. What is asserted is exactly what
// holds: momentum conserved; a bounce never injects energy; between contacts the
// operator is bit-identical to StepNBody. Energy/angular-momentum changes across a
// merge are physical and never asserted.
// =============================================================================

#include "test_framework.h"
#include "sim/collision.h"
#include "sim/nbody.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
using core::Vec3d;
using namespace sim;

NBodyParticle MakeP(uint64_t id, const Vec3d& pos, const Vec3d& vel,
                   double mu, double radius, bool isSource = true)
{
    NBodyParticle p;
    p.bodyId    = id;
    p.position  = pos;
    p.velocity  = vel;
    p.mu        = mu;
    p.radius    = radius;
    p.softening = SofteningLength(mu, radius);
    p.isSource  = isSource;
    return p;
}

Vec3d MomentumSum(const std::vector<NBodyParticle>& b)
{
    Vec3d p{ 0, 0, 0 };
    for (const auto& x : b) p += x.velocity * x.mu;
    return p;
}
double MuSum(const std::vector<NBodyParticle>& b)
{
    double m = 0.0;
    for (const auto& x : b) m += x.mu;
    return m;
}
double KineticGScaled(const std::vector<NBodyParticle>& b)
{
    double k = 0.0;
    for (const auto& x : b) k += 0.5 * x.mu * x.velocity.LengthSq();
    return k;
}
Vec3d AngularMomentumSum(const std::vector<NBodyParticle>& b)
{
    Vec3d L{ 0, 0, 0 };
    for (const auto& x : b) L += x.position.Cross(x.velocity) * x.mu;
    return L;
}
const CollisionEvent* FindMerge(const CloseEncounterReport& r, uint64_t survivor)
{
    for (const auto& e : r.events)
        if (e.merged && e.survivorId == survivor) return &e;
    return nullptr;
}
int CountMerges(const CloseEncounterReport& r)
{
    int n = 0; for (const auto& e : r.events) if (e.merged) ++n; return n;
}
int CountBounces(const CloseEncounterReport& r)
{
    int n = 0; for (const auto& e : r.events) if (!e.merged) ++n; return n;
}
// Resolve a single boundary with prevPos == current positions (so the swept test
// reduces to the instantaneous separation) - the direct classifier/reducer harness.
void ResolveInstant(std::vector<NBodyParticle>& bodies, const CloseEncounterConfig& cfg,
                    CloseEncounterReport& report)
{
    std::vector<Vec3d> prev(bodies.size());
    for (size_t k = 0; k < bodies.size(); ++k) prev[k] = bodies[k].position;
    ResolveContactsAtBoundary(bodies, prev, cfg, report);
}
} // namespace

// =============================================================================
// T1 - INERT BETWEEN COLLISIONS: with no pair in reach, StepNBodyCollisional is
//      BIT-IDENTICAL to a single StepNBody(dt), and reports no subdivision.
// =============================================================================
TEST_CASE(Collision_InertPassthrough_IsBitIdenticalToStepNBody)
{
    const CloseEncounterConfig cfg;
    std::vector<NBodyParticle> a = {
        MakeP(10, Vec3d{ 0, 0, 0 },    Vec3d{ 0, 0, 0 },    3.986e14, 6.371e6),
        MakeP(20, Vec3d{ 1e9, 0, 0 },  Vec3d{ 0, 100, 0 },  1.0,      1.0),
    };
    std::vector<NBodyParticle> b = a; // reference
    const double dt = 1.0;

    CloseEncounterReport report;
    StepNBodyCollisional(a, dt, cfg, report);
    StepNBody(b, dt);

    CHECK_EQ(report.subdivisionLevel, 0);
    CHECK_EQ(static_cast<int>(report.microSteps), 1);
    CHECK(report.events.empty());
    CHECK_EQ(a.size(), b.size());
    for (size_t k = 0; k < a.size(); ++k)
    {
        CHECK_EQ(a[k].position.x, b[k].position.x);
        CHECK_EQ(a[k].position.y, b[k].position.y);
        CHECK_EQ(a[k].position.z, b[k].position.z);
        CHECK_EQ(a[k].velocity.x, b[k].velocity.x);
        CHECK_EQ(a[k].velocity.y, b[k].velocity.y);
        CHECK_EQ(a[k].velocity.z, b[k].velocity.z);
    }
}

// =============================================================================
// T2 - ANTI-TUNNELING (flagship): a light fast body aimed head-on through a
//      target, with closest approach MID-block, must be caught (merged at default
//      e=0) - a single StepNBody would leap it clean past. Nearly gravity-free
//      (tiny mu) so this isolates the contact geometry, not the orbit.
// =============================================================================
TEST_CASE(Collision_FastFlyby_DoesNotTunnel)
{
    CloseEncounterConfig cfg; // default restitution 0 => merge on contact
    std::vector<NBodyParticle> bodies = {
        MakeP(1, Vec3d{ 0, 0, 0 },     Vec3d{ 0, 0, 0 },    1.0e6, 1.0),
        MakeP(2, Vec3d{ -100, 0, 0 },  Vec3d{ 1000, 0, 0 }, 1.0,   1.0),
    };
    const double dt = 1.0; // body 2 moves ~1000 m: from -100 through 0 to ~+900

    // A single unsubdivided step DOES tunnel (sanity for the control): keep a copy.
    std::vector<NBodyParticle> tunneled = bodies;
    StepNBody(tunneled, dt);
    CHECK_EQ(tunneled.size(), 2u); // plain integrator: no contact handling, 2 bodies remain

    CloseEncounterReport report;
    StepNBodyCollisional(bodies, dt, cfg, report);

    CHECK(report.subdivisionLevel > 0);          // refinement engaged
    CHECK_EQ(CountMerges(report), 1);            // the encounter was resolved as a merge
    CHECK_EQ(bodies.size(), 1u);                 // the pair became one body
    const CollisionEvent* ev = FindMerge(report, 1); // survivor = min bodyId
    CHECK(ev != nullptr);
    if (ev) { CHECK_EQ(ev->absorbedIds.size(), 1u); CHECK_EQ(ev->absorbedIds[0], 2ull); }
}

// =============================================================================
// T3 - LINEAR MOMENTUM ACROSS A MERGE (few ULP, real rounding path). Mass adds
//      bit-exact; momentum survives a divide-through; survivor id + absorbed set
//      are the min-bodyId convention. Non-cancelling velocities (no vacuous zero).
// =============================================================================
TEST_CASE(Collision_Merge_ConservesMomentumAndMass)
{
    CloseEncounterConfig cfg; // e=0 => merge
    std::vector<NBodyParticle> bodies = {
        MakeP(30, Vec3d{ 0, 1, 0 }, Vec3d{ 0, 0, 3 }, 5.0, 1.0),
        MakeP(10, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
        MakeP(20, Vec3d{ 1, 0, 0 }, Vec3d{ 0, 2, 0 }, 3.0, 1.0),
    };
    const Vec3d  p0  = MomentumSum(bodies);
    const double mu0 = MuSum(bodies);

    CloseEncounterReport report;
    ResolveInstant(bodies, cfg, report);

    CHECK_EQ(bodies.size(), 1u);
    CHECK_EQ(CountMerges(report), 1);
    CHECK_EQ(MuSum(bodies), mu0);                 // 2+3+5 == 10, bit-exact
    const Vec3d p1 = MomentumSum(bodies);
    CHECK(std::fabs(p1.x - p0.x) < 1e-12 * (1.0 + std::fabs(p0.x)));
    CHECK(std::fabs(p1.y - p0.y) < 1e-12 * (1.0 + std::fabs(p0.y)));
    CHECK(std::fabs(p1.z - p0.z) < 1e-12 * (1.0 + std::fabs(p0.z)));

    const CollisionEvent* ev = FindMerge(report, 10); // survivor = min id
    CHECK(ev != nullptr);
    if (ev) {
        CHECK_EQ(ev->absorbedIds.size(), 2u);
        CHECK_EQ(ev->absorbedIds[0], 20ull);     // ascending
        CHECK_EQ(ev->absorbedIds[1], 30ull);
    }
}

// =============================================================================
// T4 - CLASSIFIER TRUTH TABLE - the scope-defining predicate.
// =============================================================================
TEST_CASE(Collision_Classifier_TruthTable)
{
    // (a) overlapping, e<=stick => MERGE
    {
        CloseEncounterConfig cfg; // e=0
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
            MakeP(2, Vec3d{ 0.5, 0, 0 }, Vec3d{ -1, 0, 0 }, 2.0, 1.0), // sep 0.5 < contact 2
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountMerges(r), 1); CHECK_EQ(b.size(), 1u);
    }
    // (b) overlapping, e>stick, shallow, approaching => BOUNCE
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.5;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
            MakeP(2, Vec3d{ 1.9, 0, 0 }, Vec3d{ -1, 0, 0 }, 2.0, 1.0), // sep1.9<2, shallow pen 0.1
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountBounces(r), 1); CHECK_EQ(CountMerges(r), 0); CHECK_EQ(b.size(), 2u);
    }
    // (c) overlapping, DEEP (pen > deepMergeFrac*minR), high e => MERGE regardless
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.9;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
            MakeP(2, Vec3d{ 0.4, 0, 0 }, Vec3d{ -1, 0, 0 }, 2.0, 1.0), // sep0.4, pen1.6 > 0.5*1
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountMerges(r), 1); CHECK_EQ(b.size(), 1u);
    }
    // (d) coincident sep==0, both massive => MERGE, finite (no NaN)
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.9;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
            MakeP(2, Vec3d{ 0, 0, 0 }, Vec3d{ -1, 0, 0 }, 3.0, 1.0),
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountMerges(r), 1); CHECK_EQ(b.size(), 1u);
        CHECK(std::isfinite(b[0].velocity.x) && std::isfinite(b[0].position.x));
    }
    // (e) not overlapping => no event
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.5;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
            MakeP(2, Vec3d{ 5, 0, 0 }, Vec3d{ -1, 0, 0 }, 2.0, 1.0), // sep 5 > contact 2
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK(r.events.empty()); CHECK_EQ(b.size(), 2u);
    }
    // (f) overlapping but RECEDING (vn>0), e>stick => no impulse
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.5;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 }, Vec3d{ -1, 0, 0 }, 2.0, 1.0),  // moving apart
            MakeP(2, Vec3d{ 1.5, 0, 0 }, Vec3d{ 1, 0, 0 }, 2.0, 1.0),
        };
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK(r.events.empty()); CHECK_EQ(b.size(), 2u);
    }
}

// =============================================================================
// T5 - BOUNCE: elastic closed form, KE monotonicity, exact L, NaN-free test
//      particle. All via the shipped resolver (cfg.restitution = e).
// =============================================================================
TEST_CASE(Collision_Bounce_ConservationAndClosedForm)
{
    // (i) e=1, head-on, unequal masses: 1-D elastic closed form; KE & momentum kept.
    {
        CloseEncounterConfig cfg; cfg.restitution = 1.0;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ -0.95, 0, 0 }, Vec3d{ 1, 0, 0 },  2.0, 1.0), // sep 1.9, shallow => bounce
            MakeP(2, Vec3d{ 0.95, 0, 0 },  Vec3d{ -1, 0, 0 }, 6.0, 1.0),
        };
        const Vec3d  p0 = MomentumSum(b);
        const double k0 = KineticGScaled(b);
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountBounces(r), 1); CHECK_EQ(b.size(), 2u);
        // closed form: v1'=-2, v2'=0
        CHECK_APPROX_EPS(b[0].velocity.x, -2.0, 1e-12);
        CHECK_APPROX_EPS(b[1].velocity.x, 0.0, 1e-12);
        CHECK_APPROX_EPS(KineticGScaled(b), k0, 1e-9); // e=1 => KE unchanged
        const Vec3d p1 = MomentumSum(b);
        CHECK_APPROX_EPS(p1.x, p0.x, 1e-12);
    }
    // (ii) e=0.5, oblique approaching: KE strictly decreases; momentum & L kept exactly.
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.5;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ -0.9, 0.15, 0 }, Vec3d{ 2, 1, 0 },  2.0, 1.0), // sep ~1.9, shallow
            MakeP(2, Vec3d{ 0.9, -0.15, 0 }, Vec3d{ -1, 0, 0 }, 3.0, 1.0),
        };
        const Vec3d  p0 = MomentumSum(b);
        const Vec3d  L0 = AngularMomentumSum(b);
        const double k0 = KineticGScaled(b);
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountBounces(r), 1);
        CHECK(KineticGScaled(b) < k0);                  // inelastic: strictly less
        const Vec3d p1 = MomentumSum(b), L1 = AngularMomentumSum(b);
        CHECK_APPROX_EPS(p1.x, p0.x, 1e-12); CHECK_APPROX_EPS(p1.y, p0.y, 1e-12);
        CHECK_APPROX_EPS(L1.z, L0.z, 1e-12);            // central impulse: L exact
    }
    // (iii) test particle (mu=0) vs massive: massive unchanged, test reflects, all finite.
    {
        CloseEncounterConfig cfg; cfg.restitution = 0.5;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ -0.95, 0, 0 }, Vec3d{ 4, 0, 0 },  0.0, 1.0), // test particle, sep 1.9
            MakeP(2, Vec3d{ 0.95, 0, 0 },  Vec3d{ 0, 0, 0 },  6.0, 1.0),
        };
        const Vec3d vMassive0 = b[1].velocity;
        CloseEncounterReport r; ResolveInstant(b, cfg, r);
        CHECK_EQ(CountBounces(r), 1);
        CHECK_EQ(b[1].velocity.x, vMassive0.x);         // massive body unchanged (fj=mu_i/M=0)
        CHECK(std::isfinite(b[0].velocity.x));          // NO NaN from a 1/mu
        CHECK(b[0].velocity.x < 4.0);                   // test particle reflected/slowed
    }
}

// =============================================================================
// T6 - DETERMINISM UNDER INPUT PERMUTATION: a coupled pileup (a merge trio and a
//      separate merge pair) produces bit-identical post-state + events regardless
//      of input order. (Global e=0, so the whole scene merges by cluster.)
// =============================================================================
TEST_CASE(Collision_Determinism_UnderInputPermutation)
{
    CloseEncounterConfig cfg; // e=0
    auto scene = [] {
        return std::vector<NBodyParticle>{
            MakeP(40, Vec3d{ 0.5, 0.5, 0 }, Vec3d{ 0, 0, 1 }, 7.0, 1.0), // trio
            MakeP(10, Vec3d{ 0, 0, 0 },     Vec3d{ 1, 0, 0 }, 2.0, 1.0), // trio
            MakeP(25, Vec3d{ 1, 0, 0 },     Vec3d{ 0, 2, 0 }, 3.0, 1.0), // trio
            MakeP(80, Vec3d{ 100, 0, 0 },   Vec3d{ 0, 0, 0 }, 5.0, 1.0), // separate pair
            MakeP(60, Vec3d{ 100.5, 0, 0 }, Vec3d{ 1, 1, 1 }, 4.0, 1.0), // separate pair
        };
    };
    std::vector<NBodyParticle> a = scene();
    std::vector<NBodyParticle> b = scene();
    std::reverse(b.begin(), b.end()); // shuffle input order

    CloseEncounterReport ra, rb;
    ResolveInstant(a, cfg, ra);
    ResolveInstant(b, cfg, rb);

    auto byId = [](std::vector<NBodyParticle>& v) {
        std::sort(v.begin(), v.end(), [](const NBodyParticle& x, const NBodyParticle& y) {
            return x.bodyId < y.bodyId; });
    };
    byId(a); byId(b);
    CHECK_EQ(a.size(), b.size());
    for (size_t k = 0; k < a.size() && k < b.size(); ++k)
    {
        CHECK_EQ(a[k].bodyId, b[k].bodyId);
        CHECK_EQ(a[k].mu, b[k].mu);
        CHECK_EQ(a[k].position.x, b[k].position.x);
        CHECK_EQ(a[k].position.y, b[k].position.y);
        CHECK_EQ(a[k].velocity.x, b[k].velocity.x);
        CHECK_EQ(a[k].velocity.z, b[k].velocity.z);
        CHECK_EQ(a[k].radius, b[k].radius);
    }
    CHECK_EQ(ra.events.size(), rb.events.size());
    for (size_t k = 0; k < ra.events.size() && k < rb.events.size(); ++k)
        CHECK_EQ(ra.events[k].survivorId, rb.events[k].survivorId);
}

// =============================================================================
// T7 - WATCHED DEPTH CAP, both directions: a violent encounter with a too-small
//      cap trips hitDepthCap; a well-resolved one does not.
// =============================================================================
TEST_CASE(Collision_DepthCap_FiresWhenSaturatedSilentOtherwise)
{
    // A: violent high-speed head-on, deliberately tiny cap.
    {
        CloseEncounterConfig cfg; cfg.maxLevel = 3;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 },      Vec3d{ 0, 0, 0 },     1.0e6, 1.0),
            MakeP(2, Vec3d{ -1000, 0, 0 },  Vec3d{ 1.0e5, 0, 0 }, 1.0,   1.0),
        };
        CloseEncounterReport r;
        StepNBodyCollisional(b, 1.0, cfg, r);
        CHECK(r.hitDepthCap);
    }
    // B: gentle, well-resolved, generous cap.
    {
        CloseEncounterConfig cfg; cfg.maxLevel = 16;
        std::vector<NBodyParticle> b = {
            MakeP(1, Vec3d{ 0, 0, 0 },     Vec3d{ 0, 0, 0 }, 1.0e6, 1.0),
            MakeP(2, Vec3d{ 500, 0, 0 },   Vec3d{ 0, 5, 0 }, 1.0,   1.0),
        };
        CloseEncounterReport r;
        StepNBodyCollisional(b, 1.0, cfg, r);
        CHECK_FALSE(r.hitDepthCap);
    }
}

// =============================================================================
// T8 - CLEAN FLYBY, characterization: an eccentric BOUND orbit whose periapsis
//      enters the reach shell (subdivision engages) but stays OUTSIDE contact.
//      Over ~40 periods the collisional stepper never spuriously collides and the
//      energy stays BOUNDED (no secular blow-up). Tolerances are MEASURED, not
//      guessed: at this dt the peak excursion is ~0.18% of |E0|.
//
//      NOTE (honest scope): the collisional stepper's value is anti-tunneling and
//      correct contact handling (T2), NOT better energy conservation on a clean
//      flyby. Variable subdivision level is not exactly symplectic, so refinement
//      does NOT beat fixed-step StepNBody on a symplectic-stable flyby - a claim
//      that it did would be false, and is deliberately not asserted here.
// =============================================================================
TEST_CASE(Collision_BoundOrbit_CleanFlyby_StaysBounded)
{
    CloseEncounterConfig cfg; // radius 2 => contact = 4; periapsis ~6 stays above it

    // r_p=6, r_a=30 => a=18, e=0.667; at apoapsis v = sqrt(mu*(2/r_a - 1/a)).
    const double muP = 1.0e4;
    std::vector<NBodyParticle> col = {
        MakeP(1, Vec3d{ 0, 0, 0 },   Vec3d{ 0, 0, 0 },      muP, 2.0),
        MakeP(2, Vec3d{ 30, 0, 0 },  Vec3d{ 0, 10.5409, 0 },1.0, 2.0), // v_apoapsis
    };
    const double dt = 0.05;
    const int    steps = 4000; // ~40 periods (T ~ 4.8 s)

    const double e0 = TotalEnergyGScaled(col);
    double peakCol = 0.0, minSep = 1e300;
    int maxLevel = 0;
    bool anyEvent = false;
    for (int s = 0; s < steps; ++s)
    {
        CloseEncounterReport r;
        StepNBodyCollisional(col, dt, cfg, r);
        maxLevel = std::max(maxLevel, r.subdivisionLevel);
        anyEvent = anyEvent || !r.events.empty();
        if (col.size() == 2u)
            minSep = std::min(minSep, (col[0].position - col[1].position).Length());
        peakCol = std::max(peakCol, std::fabs(TotalEnergyGScaled(col) - e0));
    }

    CHECK(col.size() == 2u);          // clean flyby: never merged
    CHECK_FALSE(anyEvent);            // no collision event
    CHECK(minSep > 4.0);              // stayed OUTSIDE contact (=4): a flyby, not a contact
    CHECK(minSep < 8.0);              // ... but did make a genuinely close approach
    CHECK(maxLevel > 0);              // the refinement actually engaged near periapsis
    // Energy stays bounded over ~40 periods (measured ~0.0018): no secular blow-up.
    // A secular drift would exceed this by the end of the run.
    CHECK(peakCol < 5e-3 * std::fabs(e0));
}
