#pragma once
// =============================================================================
// sim/orbit_trace.h — sample an orbit's path as a polyline for map/orrery display.
// =============================================================================
// Pure CPU math: given osculating elements about a primary, produce PRIMARY-
// RELATIVE positions along the orbit. Built on the SHIPPED ElementsToState, so
// the trace passes exactly through where the sim propagates the body — the line
// and the marker can never disagree. The renderer offsets these by the primary's
// world position and applies the camera-relative narrowing (RULE 1).
// =============================================================================

#include "../core/types.h"      // core::Vec3d
#include "../ecs/components.h"  // ecs::OrbitalElements

#include <cstdint>
#include <vector>

namespace sim
{

using core::Vec3d;

// Sample the orbit as a polyline of PRIMARY-RELATIVE positions (metres).
//   * Ellipse (0 <= e < 1): a CLOSED loop of `segments`+1 points sampled uniformly
//     in ECCENTRIC anomaly, so points bunch near periapsis where curvature is high;
//     the last point equals the first.
//   * Hyperbola (e >= 1): an OPEN arc of `segments`+1 points, true anomaly swept
//     symmetrically to just inside the asymptotes (+/- acos(-1/e)).
// Returns an empty vector for degenerate input (non-finite mu/a/e, mu<=0, e<0,
// a==0, or segments < 3).
std::vector<Vec3d> SampleOrbitPath(const ecs::OrbitalElements& elements, double mu,
                                   uint32_t segments);

} // namespace sim
