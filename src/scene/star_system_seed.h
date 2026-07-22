#pragma once
// =============================================================================
// scene/star_system_seed.h — bring a true-scale StarSystem into a live scene.
// =============================================================================
// Two lean, GPU-free helpers that sit between the sim's star-system builder
// (sim::BuildReferenceSystem / sim::InstantiateStarSystem) and the renderer:
//
//   * OffsetStarSystemBodyIds — shift every bodyId (and the primary references
//     that go with it) into a high, reserved namespace so a seeded star system
//     cannot collide with gameplay bodyIds (the player ship is bodyId 1). The
//     N-body/gravity pass rejects the WHOLE step on any duplicate bodyId
//     (physics_system.cpp), so this shift is load-bearing, not cosmetic.
//
//   * AttachStarSystemVisuals — give each seeded body a shared sphere mesh + a
//     material sized to its TRUE radius, so the bodies render. The star is the
//     only seeded body with no OrbitState, which is how it is told apart (it is
//     emissive; everything that orbits is lit).
//
// Kept out of sim/ because attaching a mesh/material is render intent; kept out
// of scene.cpp so it is unit-testable without a D3D12 device.
// =============================================================================

#include "../ecs/registry.h"
#include "../sim/star_system.h"

#include <cstdint>

namespace scene
{

// Reserved base for seeded celestial bodyIds. Far above any gameplay id (ships,
// missiles, debris use small ids), so seeded + gameplay bodies never collide.
inline constexpr uint64_t kStarSystemBodyIdBase = 1'000'000ull;

// Return a copy of `sys` with every bodyId shifted up by `base`, keeping the
// primary graph internally consistent: primaryBodyId and orbit.primaryBodyId are
// shifted too, EXCEPT a primaryBodyId of 0 (the central star has no primary),
// which stays 0. A body's own bodyId is always shifted.
sim::StarSystem OffsetStarSystemBodyIds(const sim::StarSystem& sys, uint64_t base);

// Attach a MeshInstance (the shared sphere `sphereMeshHandle`) + a Material +
// a true-radius Transform.scale to every GravitationalBody whose bodyId is at or
// above `bodyIdBase` (i.e. the seeded celestial bodies, not the ship). `meshRadius`
// is the radius the sphere mesh was generated at (its vertices span that radius),
// so Transform.scale = bodyRadius / meshRadius renders the body at true size.
// A seeded body with no OrbitState is the star: it is made emissive. Returns the
// number of bodies it touched.
uint32_t AttachStarSystemVisuals(ecs::Registry& registry, uint64_t bodyIdBase,
                                 uint32_t sphereMeshHandle, float meshRadius);

} // namespace scene
