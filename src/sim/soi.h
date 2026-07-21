#pragma once
// =============================================================================
// sim/soi.h — sphere-of-influence radii, dominant-SOI membership, and the
// continuity-preserving patch that re-expresses an on-rails orbit about a NEW
// primary. This is Sim Stage 7 (patched conics), the on-rails↔on-rails handoff
// that makes multi-body travel work: as a body crosses a gravitational sphere of
// influence its two-body primary changes (heliocentric↔planetocentric).
//
// PURE CPU MATH: no ECS registry, no frame graph traversal, no orchestration.
// It builds only on the shipped kepler/nbody element machinery. The orbit
// orchestrator is the consumer — it calls ResolveDominantSoi() each step and
// Repatch() on a crossing to rewrite OrbitState. Keeping the primitive pure is
// what makes its invariants (SOI radius, deepest-membership, global-state
// continuity) unit-testable in isolation.
//
// DESIGN AUTHORITY: RELATIVISTIC_SIM_ARCHITECTURE.md §4.4 — "an SOI IS a frame;
// an SOI crossing is a frame rebase, continuous in position AND velocity"
// (v_planetocentric = v_heliocentric − v_planet). Formulas verified in
// PHYSICS_RESEARCH_MATERIAL.md (Laplace SOI, Hill sphere, r₋=r₊ / v₋=v₊ patch).
// Verification honesty (§4.5): assert Kepler closure and continuity to the
// tolerance the analytic round-trip achieves — never exact conservation.
// =============================================================================

#include "reference_frame.h"    // WorldPos, Separation, Vec3d (via types.h)
#include "../ecs/components.h"  // ecs::OrbitState

#include <cstdint>
#include <vector>

namespace sim {

// Laplace sphere-of-influence radius: r_SOI = a · (mu_body / mu_primary)^(2/5).
// The patched-conics boundary (Bate-Mueller-White; the radius KSP uses). `a` is
// the body's orbital semi-major axis about its primary. r_SOI depends only on
// the mu RATIO, so G cancels and mu = G·M is used directly. Returns 0 for any
// non-finite / non-positive input (a degenerate body has no sphere of influence).
double SphereOfInfluenceRadius(double orbitSemiMajorAxis, double bodyMu,
                               double primaryMu);

// Hill-sphere radius, periapsis form (the tightest bound over the orbit):
//   r_H = a·(1 − e) · (mu_body / (3·mu_primary))^(1/3).
// Offered as the alternative stability boundary; membership uses whichever radius
// the caller stored in the well. Returns 0 for non-finite input, e outside
// [0, 1) (an unbound arc has no stable Hill sphere), or non-positive a/mu.
double HillRadius(double orbitSemiMajorAxis, double eccentricity, double bodyMu,
                  double primaryMu);

inline constexpr uint64_t kInvalidSoi = 0xFFFFFFFFFFFFFFFFull;

// One gravitational well for membership testing. `position` is GLOBAL. `soiRadius`
// is this body's sphere radius about ITS OWN primary — use +infinity for the root
// star so interplanetary points resolve to it, or 0 for "no finite sphere" (a
// degenerate body / a root that should NOT catch interstellar space). A well with
// soiRadius <= 0 never contains a point. Nesting is implicit in the radii: a valid
// hierarchy has each child's sphere strictly smaller than and inside its parent's,
// so no explicit parent link is needed for membership.
struct SoiWell
{
    uint64_t bodyId    = kInvalidSoi;
    WorldPos position;
    double   soiRadius = 0.0;
};

// The dominant sphere of influence at `where`: the TIGHTEST (smallest true
// soiRadius) well whose sphere contains the point, tie-broken by smaller bodyId
// for determinism. Because a valid SOI hierarchy is strictly nested, the smallest
// containing sphere is the most-local gravitational authority — the correct
// patched-conics primary. Returns kInvalidSoi if the point is inside no well's
// sphere (interstellar space / the implicit root).
uint64_t ResolveDominantSoi(const WorldPos& where, const std::vector<SoiWell>& wells);

// Hysteresis wrapper that suppresses per-step primary thrash for a body loitering
// on a boundary: the current primary's sphere is treated as if scaled by
// (1 + hysteresis) — sticky, harder to leave — and every OTHER well's sphere by
// (1 − hysteresis) — harder to enter — before the same deepest-membership test.
// This opens a deadband of width ~hysteresis·radius around each boundary.
// `hysteresis` is clamped to [0, 1); 0 reproduces ResolveDominantSoi. Passing
// kInvalidSoi (or an id absent from `wells`) as currentPrimaryId means "currently
// in the root", so every well is contracted. Deterministic.
uint64_t ResolveSoiWithHysteresis(const WorldPos& where,
                                  const std::vector<SoiWell>& wells,
                                  uint64_t currentPrimaryId, double hysteresis);

// Re-express a body's orbit about a NEW primary, continuous in GLOBAL position
// AND velocity (§4.4). The body's absolute state is (bodyPos, bodyVel); the new
// primary's absolute state is (primaryPos, primaryVel) with gravitational
// parameter primaryMu. Elements are re-fit (DemoteToRails) from the primary-
// RELATIVE state — (bodyPos − primaryPos, bodyVel − primaryVel) — at epoch `now`.
// Continuity is by construction: PromoteFromRails(result, 0) yields that same
// primary-relative (r, v), so adding the primary's (pos, vel) back reproduces the
// body's global state. The returned OrbitState carries primaryBodyId ==
// newPrimaryBodyId and epoch == now; the round-trip fidelity of its elements is
// exactly that of the shipped Demote/Promote pair.
ecs::OrbitState Repatch(const WorldPos& bodyPos, const Vec3d& bodyVel,
                        const WorldPos& primaryPos, const Vec3d& primaryVel,
                        double primaryMu, uint64_t newPrimaryBodyId, double now);

} // namespace sim
