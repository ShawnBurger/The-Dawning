#pragma once
// =============================================================================
// sim/nbody.h — N-body Newtonian gravity, Forest-Ruth symplectic integrator,
//               shared softening, deterministic force sum, and the LOD transition
// =============================================================================
// SIM STAGE 1 (orbital core), the FULL-N-BODY half. GPU-free: includes only
// core/types.h, ecs/components.h and this project's sim/kepler.h + reference_frame.h
// (all device-free), so tests/test_nbody.cpp drives the SHIPPED integrator, not a
// paraphrase. sim includes ecs; ecs never includes sim — no cycle.
//
// This implements the RELATIVISTIC_SIM_ARCHITECTURE.md "DECISION REVISION":
// N-body-default in the active system, analytic Kepler as a distant-system LOD.
//
// -----------------------------------------------------------------------------
// The one shared Plummer softening (architecture §3, §5.4, §2.6)
// -----------------------------------------------------------------------------
// g = -mu * r / (r^2 + eps^2)^1.5, with eps = max(radius, r_s + eps0),
// r_s = 2*mu/c^2. The SAME eps is what the future GR clock sqrt(1 - 2GM/(r c^2))
// will floor r at, so both subsystems agree on where the 1/r singularity is
// bounded. That is why kSofteningBase / SofteningLength live here as a SHARED
// policy, not as a local constant inside the force loop. Discontinuous cutoffs
// (the deep-dive's `if(dist<1) continue`) are explicitly REJECTED — the softened
// force stays finite and C-infinity as r -> 0.
//
// -----------------------------------------------------------------------------
// Determinism (architecture revision, "Determinism")
// -----------------------------------------------------------------------------
// Floating-point addition is non-associative, so a bare pairwise loop over an
// ECS pool would give a result that depends on iteration accident. The force sum
// is therefore done in a FIXED order: bodies are sorted by their stable bodyId
// and every acceleration is accumulated in that canonical order. Two runs with
// the same bodies presented in different input orders produce BIT-IDENTICAL state.
//
// -----------------------------------------------------------------------------
// The integrator boundary — a rigid body under gravity (Codex's correction)
// -----------------------------------------------------------------------------
// Forest-Ruth's bounded-energy (symplectic) property holds ONLY for the isolated,
// conservative, fixed-step gravity Hamiltonian. It is NOT the integrator for
// thrust, drag, collision impulses, or the relativistic adapter. So the engine
// splits by operator:
//   * Passive point masses (planets, moons, coasting stations/debris) are stepped
//     by StepNBody (Forest-Ruth) — the conservative subsystem.
//   * A body that is ALSO a rigid body (attitude + thrust) does NOT enter the
//     symplectic step. Its gravity is ONE MORE EXTERNAL FORCE: call
//     GravityAccelerationAt(sources, pos) to get a (m/s^2), pass a*mass as
//     `externalForce` into sim::IntegrateRigidBody. Attitude stays with the rigid
//     body's implicit gyroscopic solve, untouched. That is the whole of the
//     "gravity is summed into its accumulator" boundary §3.3 / the revision names.
// =============================================================================

#include "../core/types.h"
#include "../ecs/components.h"
#include "kepler.h"
#include "reference_frame.h"

#include <cstdint>
#include <vector>

namespace sim
{

using core::Vec3d;

// -----------------------------------------------------------------------------
// Shared physical constants — one canonical value each.
// -----------------------------------------------------------------------------
inline constexpr double kGravitationalConstant = 6.674e-11;   // G, m^3 kg^-1 s^-2
inline constexpr double kSpeedOfLight          = 299792458.0; // c, m/s

// The ONE shared Plummer softening base eps0 (metres). Floors the softening so a
// zero-radius test particle passing exactly through a source cannot produce an
// infinite force or a complex GR clock. Real bodies' radii dominate this; it only
// matters for point sources. Shared with the future GR clock — see the banner.
inline constexpr double kSofteningBase = 1.0;

// Softening length for a body: eps = max(radius, r_s + eps0), r_s = 2*mu/c^2.
// This is the shared policy the gravity force AND the GR clock both floor at.
double SofteningLength(double mu, double radius);

// -----------------------------------------------------------------------------
// Forest-Ruth 4th-order symplectic integrator coefficients
// -----------------------------------------------------------------------------
// Composition of three leapfrog stages, theta = 1/(2 - 2^(1/3)). The DRIFT (c)
// coefficients sum to 1 and the KICK (d) coefficients sum to 1; d[3] == 0 so a
// full step costs exactly 3 force evaluations. Computed from theta at one place;
// tests/test_nbody.cpp pins every entry against its published value (a wrong
// coefficient is a silent accuracy loss).
struct ForestRuthCoefficients
{
    double c[4]; // drift (position) sub-step fractions
    double d[4]; // kick  (velocity) sub-step fractions; d[3] == 0
};
ForestRuthCoefficients GetForestRuthCoefficients();

// -----------------------------------------------------------------------------
// A body inside the active-system N-body step.
// -----------------------------------------------------------------------------
// position/velocity are expressed IN THE COMMON ACTIVE-SYSTEM FRAME (small coords)
// — the caller composes them there with the Stage-0 layer (reference_frame.h) so
// every pairwise separation is a subtraction of two small numbers, never a
// difference of two galaxy-scale absolutes (the catastrophic-cancellation lesson).
struct NBodyParticle
{
    Vec3d    position;         // common-frame position (m)
    Vec3d    velocity;         // common-frame velocity (m/s)
    double   mu       = 0.0;   // gravitational parameter G*M (0 for a test particle)
    double   softening = 0.0;  // eps (m) = SofteningLength(mu, radius); precomputed
    double   radius   = 0.0;   // TRUE physical surface radius (m). CONTACT geometry ONLY -
                               // distinct from softening (a force/GR floor >= radius that is
                               // itself floored at kSofteningBase). 0 => point/test particle:
                               // never initiates surface contact (sim/collision.h). Kept
                               // separate so a small body's contact is not welded to the ~m
                               // softening scale the architecture keeps apart from geometry.
    uint64_t bodyId   = 0;     // STABLE id → deterministic summation order
    bool     isSource = true;  // produces gravity (massive) vs test particle
};

// Build the canonical summation order: indices into `bodies` sorted ascending by
// bodyId. This is the single mechanism that makes the force sum order-independent.
std::vector<uint32_t> DeterministicOrder(const std::vector<NBodyParticle>& bodies);

// Compute the gravitational acceleration of every body (common frame), summed in
// the fixed `order`. Each massive-massive pair is evaluated ONCE from a shared
// geometric factor and applied equal/opposite, so momentum symmetry is designed in.
// Test particles feel the sources but exert no reaction. accelOut is resized to
// bodies.size().
void ComputeAccelerations(const std::vector<NBodyParticle>& bodies,
                          const std::vector<uint32_t>& order,
                          std::vector<Vec3d>& accelOut);

// Advance every particle by one fixed step dt with Forest-Ruth. Deterministic
// (sorts by bodyId internally), softened, common-frame. dt<=0 or non-finite is a
// no-op (the house guard pattern). This is the pure gravity SYSTEM — the ship-slice
// lane calls it once per fixed step from UpdateSystems; the call site is left
// unwired on purpose. RULE 6: dt is a parameter, never a wall-clock read.
void StepNBody(std::vector<NBodyParticle>& bodies, double dt);

// Summed softened gravitational acceleration (common frame) at an arbitrary point
// from the massive sources in `bodies`, in deterministic order. This is the
// operator-split hook for the rigid-body lane: externalForce = a * shipMass. A
// point's own body (if present) is excluded via `selfBodyId` (pass UINT64_MAX for
// none). `pointSoftening` is the softening of the probe (its radius/r_s).
Vec3d GravityAccelerationAt(const std::vector<NBodyParticle>& bodies,
                            const Vec3d& point, double pointSoftening,
                            uint64_t selfBodyId);

// -----------------------------------------------------------------------------
// Conserved-quantity diagnostics (for VERIFICATION; not used by the step itself)
// -----------------------------------------------------------------------------
// Total energy of the system in the common frame, softened-consistent with the
// force (Plummer potential -mu_i mu_j / (G sqrt(r^2+eps^2))). Returned in
// G-SCALED units (masses expressed as mu/G) so only mu is needed; the SLOPE of a
// G-scaled energy is zero iff the true energy's slope is zero, which is all the
// symplectic no-secular-drift test asserts.
double TotalEnergyGScaled(const std::vector<NBodyParticle>& bodies);

// Total angular momentum about the common-frame origin, G-scaled (sum mu_i r_i x v_i).
Vec3d TotalAngularMomentumGScaled(const std::vector<NBodyParticle>& bodies);

// Total linear momentum, G-scaled (sum mu_i v_i).
Vec3d TotalLinearMomentumGScaled(const std::vector<NBodyParticle>& bodies);

// -----------------------------------------------------------------------------
// A deliberately NON-symplectic reference integrator: explicit (forward) Euler.
// -----------------------------------------------------------------------------
// This is the DISCRIMINATING negative control the design demands: on the same
// two-body case it must show SECULAR energy drift (energy walks, slope != 0) where
// Forest-Ruth does not. If both looked fine, the no-secular-drift assertion would
// have no teeth. Shipped (not test-local) so the contrast is over real code.
void StepNBodyExplicitEuler(std::vector<NBodyParticle>& bodies, double dt);

// -----------------------------------------------------------------------------
// Close-encounter detection (architecture revision, "Close encounters")
// -----------------------------------------------------------------------------
// Fixed-step symplectic methods lose accuracy when a pair gets too close; Plummer
// softening keeps the force finite but is NOT permission to integrate through a
// collision. Returns true if any source pair is within
// max(radius_i+radius_j, encounter fraction of the step's reach). The caller routes
// flagged pairs to a collision/high-accuracy path. Reach uses the faster body's
// speed * dt so a fast flyby is caught before it tunnels.
bool DetectCloseEncounter(const std::vector<NBodyParticle>& bodies, double dt);

// One-owner guard (RELATIVISTIC_SIM_ARCHITECTURE.md §5.1, revised). Returns true
// iff no bodyId appears in BOTH the N-body active set and the on-rails id list.
// The caller debug-asserts this each step: a body owned by both movers would be
// double-counted, because an on-rails Kepler orbit already IS the analytic
// two-body gravity solution — feeling N-body gravity on top of it applies the
// primary's pull twice. This is the double-count negative control's guard.
bool OwnersDisjoint(const std::vector<NBodyParticle>& active,
                    const std::vector<uint64_t>& onRailsIds);

// =============================================================================
// LOD transition — N-body active <-> on-rails Kepler, continuous in (r,v)
// =============================================================================
// The active system is full N-body; a distant system's bodies ride analytic
// Kepler rails around their primary. Promotion (player enters) and demotion
// (player leaves) must be CONTINUOUS in both position and velocity — they write
// the SAME primary-relative (r,v), so continuity is by construction (§4.4).

// PROMOTE: seed the N-body primary-relative state from the on-rails elements at
// the promotion instant. `dtSinceEpoch` advances the stored elements to "now"
// first (0 if they are already current). Returns the primary-relative (r,v) to
// add onto the primary's own world state. After this the body's owner flips to
// NBodyActive and it is stepped by StepNBody.
StateVector PromoteFromRails(const ecs::OrbitState& rails, double dtSinceEpoch);

// DEMOTE: fit osculating elements from the current N-body primary-relative (r,v).
// `now` becomes the returned OrbitState's epoch. After this the body's owner flips
// to OnRails and it is propagated by PropagateElements / PropagateUniversal.
ecs::OrbitState DemoteToRails(const StateVector& primaryRelative, double primaryMu,
                              uint64_t primaryBodyId, double now);

} // namespace sim
