// =============================================================================
// scene/scene.cpp — Scene Implementation
// =============================================================================

#include "scene.h"
#include "star_system_seed.h"
#include "../ecs/systems.h"
#include "../sim/star_system.h"
#include "../sim/system_instantiate.h"
#include "../sim/orbit_trace.h"
#include "../sim/soi_transition.h"  // ResolvePrimaryFor
#include "../sim/kepler.h"          // StateVector, StateToElements
#include "../core/log.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace scene
{

namespace
{

constexpr uint64_t kSceneHashOffset = 14695981039346656037ull;
constexpr uint64_t kSceneHashPrime  = 1099511628211ull;

void HashSceneBytes(uint64_t& hash, const void* data, size_t byteCount)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < byteCount; ++i)
    {
        hash ^= bytes[i];
        hash *= kSceneHashPrime;
    }
}

template <typename T>
void HashSceneValue(uint64_t& hash, const T& value)
{
    HashSceneBytes(hash, &value, sizeof(value));
}

} // namespace

// =============================================================================
// Init / Shutdown
// =============================================================================
void Scene::Init()
{
    m_resources.Init();
    m_frames.RebuildFromFrames({});
    m_activeFrame = m_frames.CreateFrame(sim::kInvalidFrame, sim::WorldPos{});
    m_masterFrame = m_activeFrame;
    m_coordinateTime = 0.0;
    m_simTick = 0;
    m_ftlCommands.clear();
    m_atmosphereBindings.clear();
    m_clockGravityBindings.clear();
    core::Log::Info("Scene initialized");
}

void Scene::Shutdown(render::D3D12Device& device, render::Renderer& renderer)
{
    if (m_rtReady)
    {
        m_pathTracer.Shutdown();
        m_rtReady = false;
    }
    m_meshToBLAS.Clear();
    m_resources.Shutdown(device, renderer);
    core::Log::Info("Scene shut down");
}

// =============================================================================
// Entity Creation Helpers
// =============================================================================
ecs::Entity Scene::CreateRenderable(const char* name,
                                     MeshHandle mesh,
                                     const ecs::Material& material,
                                     const ecs::Transform& transform)
{
    ecs::Entity e = m_registry.Create();
    if (e.IsNull()) return e;

    m_registry.Assign<ecs::Transform>(e, transform);
    m_registry.Assign<ecs::MeshInstance>(e, ecs::MeshInstance{ mesh.value, true });
    m_registry.Assign<ecs::Material>(e, material);

    if (name)
    {
        ecs::Name n;
        n.Set(name);
        m_registry.Assign<ecs::Name>(e, n);
    }

    return e;
}

ecs::Entity Scene::CreateSpinner(const char* name,
                                  MeshHandle mesh,
                                  const ecs::Material& material,
                                  const ecs::Transform& transform,
                                  float radiansPerSec,
                                  const core::Vec3f& axis)
{
    ecs::Entity e = CreateRenderable(name, mesh, material, transform);
    if (e.IsNull()) return e;

    ecs::RotationSpeed rs;
    rs.radiansPerSecond = radiansPerSec;
    rs.axis = axis;
    m_registry.Assign<ecs::RotationSpeed>(e, rs);

    return e;
}

uint32_t Scene::SeedStarSystem(MeshHandle bodySphere, float meshRadius)
{
    if (m_starSystemActive)
        return 0; // seed once

    // Shift bodyIds into the reserved namespace so the star (id 1 in the builder)
    // does not collide with the ship (id 1) and abort the gravity step.
    const sim::StarSystem system = OffsetStarSystemBodyIds(
        sim::BuildReferenceSystem(), kStarSystemBodyIdBase);

    const uint32_t created =
        sim::InstantiateStarSystem(m_registry, m_masterFrame, system);

    const uint32_t visuals = AttachStarSystemVisuals(
        m_registry, kStarSystemBodyIdBase, bodySphere.value, meshRadius);

    m_starSystemActive = true;
    m_bodyMeshRadius = meshRadius;
    core::Log::Infof("[SIM] star_system_seeded bodies=%u visuals=%u soi=on",
                     created, visuals);
    return created;
}

void Scene::ApplyStarSystemRenderMode(bool orrery)
{
    if (!m_starSystemActive)
        return;
    auto* pool = m_registry.GetPool<ecs::GravitationalBody>();
    if (!pool)
        return;
    for (uint32_t i = 0; i < pool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = pool->DataAt(i);
        if (g.bodyId < kStarSystemBodyIdBase)
            continue; // gameplay body, not a seeded celestial
        const ecs::Entity e = m_registry.EntityAtIndex(pool->EntityAt(i));
        ecs::Transform* t = m_registry.TryGet<ecs::Transform>(e);
        if (!t)
            continue;

        float s;
        if (orrery)
        {
            // Render each body at a fixed marker size in RENDER units: the K in
            // ToCameraRelativeMatrix multiplies scale, so scale = markerUnits /
            // (meshRadius * K) yields `markerUnits` on screen regardless of K.
            // Sun larger (~14 units at r=6.96e8), every planet floored to the 7-unit
            // minimum so it stays visible (radius/5e7 < 7 for all reference planets).
            const double markerUnits = (std::max)(7.0, g.radius / 5.0e7);
            s = static_cast<float>(markerUnits /
                (static_cast<double>(m_bodyMeshRadius) * m_renderScale));
        }
        else
        {
            s = static_cast<float>(g.radius / m_bodyMeshRadius); // true radius
        }
        t->scale = { s, s, s };
    }
}

namespace
{
struct OrbitColor { float r, g, b, a; };

// Trace color keyed by the body's ORIGINAL (un-offset) id, roughly matching each
// body's own material. Alpha < 1 so the reversed-Z-depth-tested lines read as a
// translucent overlay on the sky.
OrbitColor OrbitColorFor(uint64_t localId)
{
    switch (localId)
    {
        case 10: return { 0.35f, 0.55f, 1.00f, 0.75f }; // Earth — blue
        case 11: return { 0.70f, 0.70f, 0.75f, 0.55f }; // Moon — grey
        case 20: return { 1.00f, 0.50f, 0.35f, 0.75f }; // Mars — orange
        default: return { 0.80f, 0.85f, 0.90f, 0.65f };
    }
}

// Marker color and on-screen DIAMETER (pixels) keyed by the body's ORIGINAL
// (un-offset) id. Roughly tracks each body's own material; the star reads warm and
// largest, the moon smallest, so the map is legible at a glance. Alpha < 1 so a
// marker over a body's own true-scale disc reads as an overlay rather than paint.
struct MarkerStyle { float r, g, b, a; float sizePx; };
MarkerStyle MarkerStyleFor(uint64_t localId)
{
    switch (localId)
    {
        case 1:  return { 1.00f, 0.93f, 0.70f, 0.95f, 18.0f }; // Sun — warm, largest
        case 10: return { 0.40f, 0.60f, 1.00f, 0.90f, 10.0f }; // Earth — blue
        case 11: return { 0.75f, 0.75f, 0.80f, 0.85f,  6.0f }; // Moon — grey, smallest
        case 20: return { 1.00f, 0.50f, 0.35f, 0.90f,  9.0f }; // Mars — orange
        default: return { 0.85f, 0.90f, 0.95f, 0.85f,  8.0f };
    }
}

// The four corners of a marker quad in [-1, 1], as two triangles (six vertices).
// Culling is off, so winding is irrelevant; billboard_ps.hlsl reads |corner| as the
// normalized radius, so the corners must stay within the unit square's circumscribed
// disc reach for the circular falloff to cover the whole quad.
constexpr float kBillboardCorners[6][2] = {
    { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f },
    { -1.0f, -1.0f }, { 1.0f,  1.0f }, { -1.0f, 1.0f },
};

// Per-body atmosphere scattering parameters, keyed by the body's ORIGINAL (un-offset)
// id. `height` is the atmosphere thickness above the surface (m); `betaR`/`betaM` are
// the Rayleigh/Mie scattering coefficients at the surface (1/m); Hr/Hm the scale
// heights (m); g the Mie asymmetry; sunI a radiance scale. valid=false for bodies
// with no atmosphere (the Moon and the star).
struct AtmosphereParams
{
    bool  valid;
    float height;
    float betaR[3];
    float betaM;
    float Hr, Hm, g, sunI;
};
AtmosphereParams AtmosphereParamsFor(uint64_t localId)
{
    switch (localId)
    {
        // Earth: the textbook Nishita coefficients — blue sky, orange sunset limb.
        // The shell is drawn slightly taller than the real ~100 km and a touch
        // brighter so the limb ring reads at orbital distance (a common, tasteful
        // exaggeration); the coefficients and scale heights stay physical.
        case 10: return { true, 180000.0f,
                          { 5.8e-6f, 13.5e-6f, 33.1e-6f }, 21.0e-6f,
                          8500.0f, 1200.0f, 0.76f, 40.0f };
        // Mars: a thin, dusty, reddish atmosphere — weak/ruddy Rayleigh, low density.
        case 20: return { true, 120000.0f,
                          { 19.9e-6f, 13.5e-6f, 8.1e-6f }, 10.0e-6f,
                          11000.0f, 3000.0f, 0.70f, 18.0f };
        default: return { false, 0.0f, { 0.0f, 0.0f, 0.0f }, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    }
}

// -----------------------------------------------------------------------------
// Procedural planet-surface parameters, keyed by the body's ORIGINAL (un-offset)
// localId — the twin of AtmosphereParamsFor/VisualFor. The surface is fully
// procedural (no texture assets); these constants tune the analytic layer stack
// in planet_ps.hlsl. type: 0 Earth-like, 1 Mars-like, 2 Moon-like, 3 generic.
// Every seeded body routed here gets a procedural surface (unrecognised ids fall
// through to the generic default). The central STAR never reaches this — it is
// excluded upstream by the OrbitState predicate in RenderEntities, not here.
// -----------------------------------------------------------------------------
struct PlanetSurfaceParams
{
    int   type;
    float seaLevel;
    float deep[3];     float depthScale;
    float shallow[3];  float coastWidth;
    float landLow[3];  float oceanRough;
    float landHigh[3]; float landRough;
    float ice[3];      float iceLatitude;
    float cloudCoverage, cloudSoftness, cloudRotSpeed, cloudBrightness;
    float night[3];    float nightIntensity;
    float ambient[3];  float glintShininess;
    float seed;
};

PlanetSurfaceParams PlanetParamsFor(uint64_t localId)
{
    switch (localId)
    {
        // Earth: ~70% ocean, green/brown continents, polar ice, drifting clouds,
        // warm night-side city lights, Fresnel ocean glint.
        case 10: return {0, 0.52f,
                          { 0.015f, 0.050f, 0.140f }, 0.25f,
                          { 0.050f, 0.220f, 0.320f }, 0.020f,
                          { 0.090f, 0.230f, 0.100f }, 0.05f,
                          { 0.320f, 0.280f, 0.220f }, 0.90f,
                          { 0.850f, 0.880f, 0.920f }, 0.80f,
                          0.48f, 0.12f, 0.006f, 1.10f,
                          { 1.000f, 0.820f, 0.500f }, 1.00f,
                          { 0.015f, 0.020f, 0.030f }, 220.0f,
                          11.0f };
        // Moon: dark maria vs bright highlands, dense craters, no ocean/atmosphere/
        // clouds/lights, sharp terminator (type 2).
        case 11: return {2, 1.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.045f, 0.045f, 0.050f }, 0.0f,
                          { 0.400f, 0.400f, 0.420f }, 0.98f,
                          { 0.0f, 0.0f, 0.0f }, 2.0f,
                          0.0f, 0.0f, 0.0f, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.010f, 0.010f, 0.012f }, 0.0f,
                          33.0f };
        // Mars: rust/oxide dry land everywhere (no ocean), CO2 polar caps, craters,
        // no night lights.
        case 20: return {1, 1.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.420f, 0.200f, 0.110f }, 0.0f,
                          { 0.620f, 0.360f, 0.230f }, 0.95f,
                          { 0.900f, 0.900f, 0.920f }, 0.82f,
                          0.0f, 0.0f, 0.0f, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.020f, 0.012f, 0.008f }, 0.0f,
                          22.0f };
        // Generic rocky body — neutral, no ocean/clouds/lights. Safe fallback so a
        // larger seeded system still renders every body.
        default: return {3, 1.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.300f, 0.300f, 0.320f }, 0.0f,
                          { 0.500f, 0.500f, 0.520f }, 0.90f,
                          { 0.800f, 0.820f, 0.850f }, 0.85f,
                          0.0f, 0.0f, 0.0f, 0.0f,
                          { 0.0f, 0.0f, 0.0f }, 0.0f,
                          { 0.014f, 0.014f, 0.016f }, 0.0f,
                          7.0f };
    }
}

// Build the per-body root-CBV payload. sunDir is the direction from the body TO
// the star, computed in double before narrowing (RULE 1): the star holds the
// frame origin, so it is normalize(-bodyPos). Sunlight is a fixed warm white.
// sunDir.w carries the cloud rotation ANGLE (radians), wrapped to [0, 2pi) in
// double so it stays float-exact regardless of how far the sim clock has run.
render::Renderer::PlanetConstants BuildPlanetConstants(const PlanetSurfaceParams& ps,
                                                       const ecs::Transform& transform,
                                                       double renderScale,
                                                       double coordinateTime)
{
    const core::Vec3d starPos{ 0.0, 0.0, 0.0 };
    const core::Vec3f sunDir = (starPos - transform.position).Normalized().ToFloat();

    constexpr double kTwoPi = 6.283185307179586;
    const float cloudAngle = static_cast<float>(
        std::fmod(coordinateTime * ps.cloudRotSpeed, kTwoPi));

    render::Renderer::PlanetConstants c = {};
    c.sunDir[0] = sunDir.x; c.sunDir[1] = sunDir.y; c.sunDir[2] = sunDir.z;
    c.sunDir[3] = cloudAngle;
    c.sunColor[0] = 1.0f; c.sunColor[1] = 0.97f; c.sunColor[2] = 0.92f;
    c.sunColor[3] = 1.30f; // sun intensity
    c.params0[0] = static_cast<float>(ps.type);
    c.params0[1] = ps.seaLevel;
    c.params0[2] = ps.seed;
    c.params0[3] = static_cast<float>(renderScale);
    c.deepColor[0] = ps.deep[0]; c.deepColor[1] = ps.deep[1]; c.deepColor[2] = ps.deep[2];
    c.deepColor[3] = ps.depthScale;
    c.shallowColor[0] = ps.shallow[0]; c.shallowColor[1] = ps.shallow[1]; c.shallowColor[2] = ps.shallow[2];
    c.shallowColor[3] = ps.coastWidth;
    c.landLow[0] = ps.landLow[0]; c.landLow[1] = ps.landLow[1]; c.landLow[2] = ps.landLow[2];
    c.landLow[3] = ps.oceanRough;
    c.landHigh[0] = ps.landHigh[0]; c.landHigh[1] = ps.landHigh[1]; c.landHigh[2] = ps.landHigh[2];
    c.landHigh[3] = ps.landRough;
    c.iceColor[0] = ps.ice[0]; c.iceColor[1] = ps.ice[1]; c.iceColor[2] = ps.ice[2];
    c.iceColor[3] = ps.iceLatitude;
    c.cloud[0] = ps.cloudCoverage; c.cloud[1] = ps.cloudSoftness;
    c.cloud[2] = ps.cloudRotSpeed; c.cloud[3] = ps.cloudBrightness;
    c.night[0] = ps.night[0]; c.night[1] = ps.night[1]; c.night[2] = ps.night[2];
    c.night[3] = ps.nightIntensity;
    c.ambient[0] = ps.ambient[0]; c.ambient[1] = ps.ambient[1]; c.ambient[2] = ps.ambient[2];
    c.ambient[3] = ps.glintShininess;
    return c;
}
} // namespace

std::vector<render::Renderer::LineVertex>
Scene::BuildOrbitTraceVertices(const core::Vec3d& cameraPosition,
                               uint64_t focusBodyId) const
{
    std::vector<render::Renderer::LineVertex> verts;
    if (!m_starSystemActive)
        return verts;

    auto* gravPool = m_registry.GetPool<ecs::GravitationalBody>();
    if (!gravPool)
        return verts;

    // World position of every gravity body by bodyId. The star sits at the frame
    // origin, so a body's frame-local Transform.position is its world position.
    std::unordered_map<uint64_t, core::Vec3d> posById;
    posById.reserve(gravPool->Count() * 2u);
    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
        if (const auto* t = m_registry.TryGet<ecs::Transform>(e))
            posById[gravPool->DataAt(i).bodyId] = t->position;
    }

    const double K = m_renderScale;
    constexpr uint32_t kSegments = 128u;

    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = gravPool->DataAt(i);
        if (g.bodyId < kStarSystemBodyIdBase)
            continue; // seeded celestial bodies only
        const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
        const auto* os = m_registry.TryGet<ecs::OrbitState>(e);
        if (!os)
            continue; // the star has no orbit
        const auto primary = posById.find(os->primaryBodyId);
        if (primary == posById.end())
            continue;
        const core::Vec3d primaryPos = primary->second;

        const std::vector<core::Vec3d> local =
            sim::SampleOrbitPath(os->elements, os->primaryMu, kSegments);
        if (local.size() < 2u)
            continue;

        OrbitColor c = OrbitColorFor(g.bodyId - kStarSystemBodyIdBase);
        if (g.bodyId == focusBodyId)
        {
            // The focused body's orbit reads brighter and fully opaque, so the map
            // shows at a glance which body the view is tracking.
            c.r = 0.6f + 0.4f * c.r;
            c.g = 0.6f + 0.4f * c.g;
            c.b = 0.6f + 0.4f * c.b;
            c.a = 1.0f;
        }
        // (worldPt - camera) subtracted in DOUBLE first, then scaled by K, then
        // narrowed (RULE 1) — the same discipline the entity matrices use.
        auto toRender = [&](const core::Vec3d& localPt) -> core::Vec3f
        {
            return ((primaryPos + localPt - cameraPosition) * K).ToFloat();
        };
        verts.reserve(verts.size() + (local.size() - 1u) * 2u);
        for (size_t k = 0; k + 1u < local.size(); ++k)
        {
            const core::Vec3f a = toRender(local[k]);
            const core::Vec3f b = toRender(local[k + 1u]);
            verts.push_back({ { a.x, a.y, a.z }, { c.r, c.g, c.b, c.a } });
            verts.push_back({ { b.x, b.y, b.z }, { c.r, c.g, c.b, c.a } });
        }
    }
    return verts;
}

std::vector<render::Renderer::BillboardVertex>
Scene::BuildBodyMarkerVertices(const core::Vec3d& cameraPosition) const
{
    std::vector<render::Renderer::BillboardVertex> verts;
    if (!m_starSystemActive)
        return verts;

    auto* gravPool = m_registry.GetPool<ecs::GravitationalBody>();
    if (!gravPool)
        return verts;

    const double K = m_renderScale;

    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = gravPool->DataAt(i);
        if (g.bodyId < kStarSystemBodyIdBase)
            continue; // seeded celestial bodies only (the star included: it has no
                      // orbit but is the marker that matters most)
        const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
        const auto* t = m_registry.TryGet<ecs::Transform>(e);
        if (!t)
            continue;

        // (worldPos - camera) in DOUBLE, then scaled by K, then narrowed (RULE 1),
        // exactly as the orbit lines and entity matrices do. The star sits at the
        // frame origin, so a body's frame-local position is its world position.
        const core::Vec3f center =
            ((t->position - cameraPosition) * K).ToFloat();

        const MarkerStyle m = MarkerStyleFor(g.bodyId - kStarSystemBodyIdBase);
        for (const auto& corner : kBillboardCorners)
        {
            verts.push_back({ { center.x, center.y, center.z },
                              { m.r, m.g, m.b, m.a },
                              m.sizePx,
                              { corner[0], corner[1] } });
        }
    }
    return verts;
}

OsculatingOrbit Scene::DeriveOsculatingOrbit(uint64_t bodyId) const
{
    OsculatingOrbit out;
    if (!m_starSystemActive)
        return out;

    auto* gravPool = m_registry.GetPool<ecs::GravitationalBody>();
    if (!gravPool)
        return out;

    // Live Cartesian state of the queried body: position in Transform, velocity in
    // RigidBody (the ship keeps its velocity there — it has no OrbitState of its own).
    core::Vec3d bodyPos, bodyVel;
    bool found = false;
    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        if (gravPool->DataAt(i).bodyId != bodyId)
            continue;
        const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
        const auto* t  = m_registry.TryGet<ecs::Transform>(e);
        const auto* rb = m_registry.TryGet<ecs::RigidBody>(e);
        if (!t || !rb)
            return out;
        bodyPos = t->position;
        bodyVel = rb->linearVelocity;
        found = true;
        break;
    }
    if (!found)
        return out;

    const sim::ResolvedPrimary primary =
        sim::ResolvePrimaryFor(m_registry, bodyPos, bodyId);
    if (!primary.found || primary.mu <= 0.0)
        return out;

    // Primary-relative state -> osculating elements (the inverse of ElementsToState).
    const sim::StateVector rel{ bodyPos - primary.position,
                                bodyVel - primary.velocity };
    const double r = rel.position.Length();
    if (r <= 0.0)
        return out;
    const ecs::OrbitalElements el = sim::StateToElements(rel, primary.mu);
    if (!std::isfinite(el.semiMajorAxis) || !std::isfinite(el.eccentricity) ||
        el.eccentricity < 0.0)
        return out; // parabolic / radial / otherwise degenerate fit

    out.valid         = true;
    out.primaryBodyId = primary.bodyId;
    out.primaryPos    = primary.position;
    out.primaryMu     = primary.mu;
    out.elements      = el;
    out.altitude      = r;
    out.speed         = rel.velocity.Length();

    const double a = el.semiMajorAxis;
    const double e = el.eccentricity;
    out.periapsis = a * (1.0 - e); // positive for both ellipse (a>0) and hyperbola (a<0,e>1)
    if (a > 0.0 && e < 1.0)
    {
        out.apoapsis = a * (1.0 + e);
        constexpr double kTwoPi = 6.283185307179586;
        out.period = kTwoPi * std::sqrt(a * a * a / primary.mu);
    }
    return out;
}

OsculatingOrbit Scene::PreviewProgradeBurn(const OsculatingOrbit& base,
                                           double deltaV) const
{
    OsculatingOrbit out;
    if (!base.valid || base.primaryMu <= 0.0)
        return out;

    // Elements -> primary-relative state, add deltaV along the velocity, -> elements.
    sim::StateVector s = sim::ElementsToState(base.elements, base.primaryMu);
    const double vmag = s.velocity.Length();
    if (vmag <= 0.0)
        return out;
    s.velocity = s.velocity + s.velocity * (deltaV / vmag);

    const ecs::OrbitalElements el = sim::StateToElements(s, base.primaryMu);
    if (!std::isfinite(el.semiMajorAxis) || !std::isfinite(el.eccentricity) ||
        el.eccentricity < 0.0)
        return out;

    out.valid         = true;
    out.primaryBodyId = base.primaryBodyId;
    out.primaryPos    = base.primaryPos;
    out.primaryMu     = base.primaryMu;
    out.elements      = el;
    out.altitude      = s.position.Length();
    out.speed         = s.velocity.Length();
    const double a = el.semiMajorAxis;
    const double e = el.eccentricity;
    out.periapsis = a * (1.0 - e);
    if (a > 0.0 && e < 1.0)
    {
        out.apoapsis = a * (1.0 + e);
        constexpr double kTwoPi = 6.283185307179586;
        out.period = kTwoPi * std::sqrt(a * a * a / base.primaryMu);
    }
    return out;
}

std::vector<render::Renderer::LineVertex>
Scene::BuildDerivedOrbitTraceVertices(const core::Vec3d& cameraPosition,
                                      const OsculatingOrbit& orbit,
                                      float r, float g, float b, float a) const
{
    std::vector<render::Renderer::LineVertex> verts;
    if (!orbit.valid)
        return verts;

    const std::vector<core::Vec3d> local =
        sim::SampleOrbitPath(orbit.elements, orbit.primaryMu, 192u);
    if (local.size() < 2u)
        return verts;

    const double K = m_renderScale;
    // Same RULE-1 discipline as BuildOrbitTraceVertices: subtract the camera in
    // double, then scale by K, then narrow.
    auto toRender = [&](const core::Vec3d& localPt) -> core::Vec3f
    {
        return ((orbit.primaryPos + localPt - cameraPosition) * K).ToFloat();
    };
    verts.reserve((local.size() - 1u) * 2u);
    for (size_t k = 0; k + 1u < local.size(); ++k)
    {
        const core::Vec3f p0 = toRender(local[k]);
        const core::Vec3f p1 = toRender(local[k + 1u]);
        verts.push_back({ { p0.x, p0.y, p0.z }, { r, g, b, a } });
        verts.push_back({ { p1.x, p1.y, p1.z }, { r, g, b, a } });
    }
    return verts;
}

std::vector<render::Renderer::BillboardVertex>
Scene::BuildApsisMarkerVertices(const core::Vec3d& cameraPosition,
                                const OsculatingOrbit& shipOrbit,
                                uint64_t focusBodyId) const
{
    std::vector<render::Renderer::BillboardVertex> verts;
    if (!m_starSystemActive)
        return verts;

    const double K = m_renderScale;
    constexpr float kPeColor[4] = { 0.25f, 0.95f, 1.00f, 0.95f }; // periapsis — cyan
    constexpr float kApColor[4] = { 1.00f, 0.45f, 0.85f, 0.95f }; // apoapsis — magenta
    constexpr float kApsisSizePx = 9.0f;

    auto emitMarker = [&](const core::Vec3d& worldPos, const float col[4])
    {
        const core::Vec3f center = ((worldPos - cameraPosition) * K).ToFloat();
        for (const auto& corner : kBillboardCorners)
            verts.push_back({ { center.x, center.y, center.z },
                              { col[0], col[1], col[2], col[3] },
                              kApsisSizePx, { corner[0], corner[1] } });
    };

    // Periapsis (true anomaly 0) and apoapsis (pi) of one orbit about primaryPos,
    // found by evaluating the shipped ElementsToState at those anomalies — the same
    // propagator the trace samples, so the markers land exactly on the drawn ellipse.
    auto emitApsides = [&](const ecs::OrbitalElements& el, double mu,
                           const core::Vec3d& primaryPos)
    {
        if (!std::isfinite(el.semiMajorAxis) || !std::isfinite(el.eccentricity) ||
            mu <= 0.0)
            return;
        ecs::OrbitalElements peEl = el;
        peEl.trueAnomaly = 0.0;
        emitMarker(primaryPos + sim::ElementsToState(peEl, mu).position, kPeColor);
        if (el.eccentricity < 1.0) // apoapsis exists only for a closed ellipse
        {
            ecs::OrbitalElements apEl = el;
            apEl.trueAnomaly = 3.14159265358979323846;
            emitMarker(primaryPos + sim::ElementsToState(apEl, mu).position, kApColor);
        }
    };

    if (shipOrbit.valid)
        emitApsides(shipOrbit.elements, shipOrbit.primaryMu, shipOrbit.primaryPos);

    // The focused body's apsides on its own orbit — look up its primary's world pos.
    if (auto* gravPool = m_registry.GetPool<ecs::GravitationalBody>())
    {
        std::unordered_map<uint64_t, core::Vec3d> posById;
        posById.reserve(gravPool->Count() * 2u);
        for (uint32_t i = 0; i < gravPool->Count(); ++i)
        {
            const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
            if (const auto* t = m_registry.TryGet<ecs::Transform>(e))
                posById[gravPool->DataAt(i).bodyId] = t->position;
        }
        for (uint32_t i = 0; i < gravPool->Count(); ++i)
        {
            if (gravPool->DataAt(i).bodyId != focusBodyId)
                continue;
            const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
            if (const auto* os = m_registry.TryGet<ecs::OrbitState>(e))
            {
                const auto pit = posById.find(os->primaryBodyId);
                if (pit != posById.end())
                    emitApsides(os->elements, os->primaryMu, pit->second);
            }
            break;
        }
    }
    return verts;
}

void Scene::DestroyEntity(ecs::Entity entity)
{
    ClearAtmosphereBinding(entity);
    ClearClockGravityBinding(entity);
    std::erase_if(m_ftlCommands,
                  [&](const sim::FtlCommand& command)
                  { return command.entity == entity; });
    m_registry.Destroy(entity);
}

// =============================================================================
// Phase 1: Update Systems
// =============================================================================
sim::SimulationStepResult Scene::UpdateSystems(double dt)
{
    sim::SimulationStepConfig config;
    config.activeFrame = m_activeFrame;
    config.masterFrame = m_masterFrame;
    config.coordinateTime = m_coordinateTime;
    config.flightAssist = m_flightAssist;
    config.closeEncounters = m_closeEncounters;
    config.enableSoiTransitions = m_starSystemActive;
    config.soiHysteresis = m_soiHysteresis;
    config.ftlCommands = m_ftlCommands;
    config.atmosphereBindings = m_atmosphereBindings;
    config.clockGravityBindings = m_clockGravityBindings;

    sim::SimulationStepResult result =
        sim::StepSimulation(m_registry, m_frames, dt, config);
    m_ftlCommands.clear();
    if (!result.accepted)
        return result;

    // Legacy Velocity/RotationSpeed components remain useful for simple render
    // props. Their systems explicitly skip RigidBody-owned transforms.
    SystemVelocity(dt);
    SystemRotation(dt);
    m_coordinateTime += dt;
    ++m_simTick;

    // Collisions can destroy entities below Scene::DestroyEntity. Keep the
    // host-owned persistent bindings synchronized with the resulting registry.
    std::erase_if(m_atmosphereBindings,
                  [&](const sim::AtmosphereBinding& binding)
                  { return !m_registry.IsAlive(binding.entity); });
    std::erase_if(m_clockGravityBindings,
                  [&](const sim::ClockGravityBinding& binding)
                  { return !m_registry.IsAlive(binding.entity); });
    for (sim::ClockGravityBinding& binding : m_clockGravityBindings)
    {
        const auto remap = std::find_if(
            result.bodyIdRemaps.begin(), result.bodyIdRemaps.end(),
            [&](const auto& candidate)
            { return candidate.first == binding.primaryBodyId; });
        if (remap != result.bodyIdRemaps.end())
            binding.primaryBodyId = remap->second;
    }
    return result;
}

void Scene::QueueFtlCommand(const sim::FtlCommand& command)
{
    m_ftlCommands.push_back(command);
}

void Scene::SetAtmosphereBinding(const sim::AtmosphereBinding& binding)
{
    const auto it = std::find_if(
        m_atmosphereBindings.begin(), m_atmosphereBindings.end(),
        [&](const sim::AtmosphereBinding& current)
        { return current.entity == binding.entity; });
    if (it == m_atmosphereBindings.end())
        m_atmosphereBindings.push_back(binding);
    else
        *it = binding;
}

void Scene::ClearAtmosphereBinding(ecs::Entity entity)
{
    std::erase_if(m_atmosphereBindings,
                  [&](const sim::AtmosphereBinding& binding)
                  { return binding.entity == entity; });
}

void Scene::SetClockGravityBinding(const sim::ClockGravityBinding& binding)
{
    const auto it = std::find_if(
        m_clockGravityBindings.begin(), m_clockGravityBindings.end(),
        [&](const sim::ClockGravityBinding& current)
        { return current.entity == binding.entity; });
    if (it == m_clockGravityBindings.end())
        m_clockGravityBindings.push_back(binding);
    else
        *it = binding;
}

void Scene::ClearClockGravityBinding(ecs::Entity entity)
{
    std::erase_if(m_clockGravityBindings,
                  [&](const sim::ClockGravityBinding& binding)
                  { return binding.entity == entity; });
}

sim::SnapshotBuildResult Scene::BuildSimulationSnapshot(double fixedDt) const
{
    return sim::BuildSnapshot(m_registry, m_frames, m_coordinateTime, fixedDt,
                              m_simTick, m_masterFrame);
}

sim::SnapshotApplyResult Scene::ApplySimulationSnapshot(
    const sim::SimSnapshot& snapshot)
{
    sim::SnapshotApplyResult result =
        sim::ApplySnapshot(m_registry, m_frames, snapshot);
    if (!result.accepted)
        return result;

    m_coordinateTime = snapshot.coordinateTimeEpoch;
    m_simTick = snapshot.simTick;
    m_masterFrame = snapshot.masterFrame;
    m_activeFrame = snapshot.masterFrame;
    m_ftlCommands.clear();
    InvalidatePathTraceHistory();
    return result;
}

void Scene::InvalidatePathTraceHistory()
{
    m_pathTracer.InvalidateAccumulation();
}

// =============================================================================
// System: Velocity - integrates entities that have Transform + Velocity
// =============================================================================
void Scene::SystemVelocity(double dt)
{
    ecs::systems::IntegrateVelocities(m_registry, dt);
}

// =============================================================================
// System: Rotation — spins entities that have Transform + RotationSpeed
// =============================================================================
void Scene::SystemRotation(double dt)
{
    ecs::systems::IntegrateRotations(m_registry, dt);
}

// =============================================================================
// Phase 2: Render Entities
// =============================================================================
// Iterates all entities with Transform + MeshInstance + Material and issues
// draw calls through the renderer. Checks mesh handle validity and visibility.
// =============================================================================
uint32_t Scene::MeshInstanceCount() const
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    return meshPool ? meshPool->Count() : 0u;
}

void Scene::RenderShadowCasters(render::D3D12Device& device,
                               render::Renderer& renderer,
                               const core::Vec3d& cameraPosition)
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);

        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        renderer.DrawMeshShadow(device, *gpuMesh,
                                transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale));
    }
}

void Scene::RenderDepthPrepass(render::D3D12Device& device,
                               render::Renderer& renderer,
                               const core::Vec3d& cameraPosition)
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);

        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        renderer.DrawMeshDepth(device, *gpuMesh,
                               transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale));
    }
}

void Scene::RenderEntities(render::D3D12Device& device,
                           render::Renderer& renderer,
                           const core::Vec3d& cameraPosition)
{
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        // Skip invisible entities
        if (!meshInst.visible) continue;

        // Must also have Transform and Material
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);
        const auto& material  = m_registry.GetByIndex<ecs::Material>(entityIdx);

        // Look up the actual GPU mesh via handle
        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        const render::Texture* albedoTexture = nullptr;
        if (material.albedoTextureHandle != UINT32_MAX)
            albedoTexture = m_resources.GetTexture(TextureHandle(material.albedoTextureHandle));
        const render::Texture* normalTexture = nullptr;
        if (material.normalTextureHandle != UINT32_MAX)
            normalTexture = m_resources.GetTexture(TextureHandle(material.normalTextureHandle));
        const render::Texture* ormTexture = nullptr;
        if (material.ormTextureHandle != UINT32_MAX)
            ormTexture = m_resources.GetTexture(TextureHandle(material.ormTextureHandle));

        const render::Texture* emissiveTexture = nullptr;
        if (material.emissiveTextureHandle != UINT32_MAX)
            emissiveTexture =
                m_resources.GetTexture(TextureHandle(material.emissiveTextureHandle));

        const core::Mat4x4 worldMatrix =
            transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);

        // Route seeded celestial bodies (not the central star, which has no
        // OrbitState) to the procedural planet-surface shader, but only at true
        // scale — in the compressed orrery (K ~ 1e-9) a body is a sub-pixel disc
        // where per-pixel surface noise is pointless, so those stay on DrawMesh.
        // Same gate RenderAtmospheres uses, so surface and atmosphere appear
        // together. Everything else keeps the standard PBR path.
        if (m_renderScale >= 0.5 &&
            m_registry.HasByIndex<ecs::GravitationalBody>(entityIdx) &&
            m_registry.HasByIndex<ecs::OrbitState>(entityIdx))
        {
            const auto& g = m_registry.GetByIndex<ecs::GravitationalBody>(entityIdx);
            if (g.bodyId >= kStarSystemBodyIdBase)
            {
                const PlanetSurfaceParams ps = PlanetParamsFor(g.bodyId - kStarSystemBodyIdBase);
                const render::Renderer::PlanetConstants pc =
                    BuildPlanetConstants(ps, transform, m_renderScale, m_coordinateTime);
                // If this is the Surface-mode terrain body, pull its smooth sphere
                // radially inward by a hair (1e-4 of radius) so the displaced terrain
                // patches drawn over it always win the reversed-Z test. The offset is
                // far below the finest terrain grid-cell chord (~R/2048), so the far
                // limb the sphere still supplies moves sub-pixel, and far above the
                // reversed-Z depth quantisation at these ranges. s*r*t keeps the scale
                // purely radial about the body centre. Note the z-fight this removes is
                // TEMPORAL: because ConfigForTerrainBody mirrors PlanetParamsFor, sphere
                // and terrain shade to the same colour, so a still frame is byte-
                // identical with or without this — the artifact is depth-tie flicker
                // under the descent camera's motion, which this makes deterministic.
                core::Mat4x4 planetWorld = worldMatrix;
                if (m_terrainSurfaceBodyId != 0 && g.bodyId == m_terrainSurfaceBodyId)
                {
                    ecs::Transform pulled = transform;
                    constexpr float kTerrainSpherePullback = 1.0f - 1.0e-4f;
                    pulled.scale.x *= kTerrainSpherePullback;
                    pulled.scale.y *= kTerrainSpherePullback;
                    pulled.scale.z *= kTerrainSpherePullback;
                    planetWorld = pulled.ToCameraRelativeMatrix(cameraPosition, m_renderScale);
                }
                renderer.DrawPlanet(device, *gpuMesh, planetWorld,
                                    material.albedo, material.roughness,
                                    material.metallic, pc);
                continue;
            }
        }

        // Issue draw call
        renderer.DrawMesh(device, *gpuMesh, worldMatrix,
                          material.albedo, material.roughness, material.metallic,
                          albedoTexture, normalTexture, ormTexture, emissiveTexture,
                          material.emissive, material.emissiveStrength);
    }
}

void Scene::RenderAtmospheres(render::D3D12Device& device,
                              render::Renderer& renderer,
                              const core::Vec3d& cameraPosition,
                              const core::Mat4x4& viewProj)
{
    if (!m_starSystemActive)
        return;
    // Compressed (orrery) views scale the world by K ~ 1e-9, where an atmosphere is
    // sub-pixel; the shell math also assumes metres (K == 1). Only true-scale views.
    if (m_renderScale < 0.5)
        return;

    auto* gravPool = m_registry.GetPool<ecs::GravitationalBody>();
    if (!gravPool)
        return;

    // The star holds the frame origin, so the direction to it from any planet is
    // simply -planetPos (normalized in double before narrowing).
    const core::Vec3d starPos{ 0.0, 0.0, 0.0 };

    for (uint32_t i = 0; i < gravPool->Count(); ++i)
    {
        const ecs::GravitationalBody& g = gravPool->DataAt(i);
        if (g.bodyId < kStarSystemBodyIdBase)
            continue; // seeded celestial bodies only
        const AtmosphereParams ap = AtmosphereParamsFor(g.bodyId - kStarSystemBodyIdBase);
        if (!ap.valid)
            continue;

        const ecs::Entity e = m_registry.EntityAtIndex(gravPool->EntityAt(i));
        const auto* t  = m_registry.TryGet<ecs::Transform>(e);
        const auto* mi = m_registry.TryGet<ecs::MeshInstance>(e);
        if (!t || !mi)
            continue;
        const render::Mesh* mesh = m_resources.GetMesh(MeshHandle(mi->meshHandle));
        if (!mesh || !mesh->IsValid())
            continue;

        const double K = m_renderScale; // == 1 here
        const core::Vec3f centerCS = ((t->position - cameraPosition) * K).ToFloat();
        const float Rp = static_cast<float>(g.radius);
        const float Ra = Rp + ap.height;
        const core::Vec3f sunDir = (starPos - t->position).Normalized().ToFloat();

        // Shell world: the shared unit sphere (m_bodyMeshRadius) scaled to Ra and
        // translated to the camera-relative planet centre — the same S*T convention
        // ToCameraRelativeMatrix uses (RULE 1: subtract in double, then narrow).
        const float shellScale = Ra / m_bodyMeshRadius;
        const core::Mat4x4 shellWorld =
            core::Mat4x4::Scaling(shellScale) *
            core::Mat4x4::Translation(centerCS);

        render::Renderer::AtmosphereConstants c = {};
        std::memcpy(c.world, shellWorld.Data(), sizeof(c.world));
        std::memcpy(c.viewProj, viewProj.Data(), sizeof(c.viewProj));
        c.planetCenterRadius[0] = centerCS.x;
        c.planetCenterRadius[1] = centerCS.y;
        c.planetCenterRadius[2] = centerCS.z;
        c.planetCenterRadius[3] = Rp;
        c.sunDirAtmoRadius[0] = sunDir.x;
        c.sunDirAtmoRadius[1] = sunDir.y;
        c.sunDirAtmoRadius[2] = sunDir.z;
        c.sunDirAtmoRadius[3] = Ra;
        c.betaRayleighG[0] = ap.betaR[0];
        c.betaRayleighG[1] = ap.betaR[1];
        c.betaRayleighG[2] = ap.betaR[2];
        c.betaRayleighG[3] = ap.g;
        c.betaMieHeights[0] = ap.betaM;
        c.betaMieHeights[1] = ap.Hr;
        c.betaMieHeights[2] = ap.Hm;
        c.betaMieHeights[3] = ap.sunI;

        renderer.DrawAtmosphere(device, *mesh, c);
    }
}

// =============================================================================
// Path Tracer Init
// =============================================================================
bool Scene::InitPathTracer(render::D3D12Device& device)
{
    if (!m_pathTracer.Init(device))
    {
        core::Log::Error("Failed to initialize path tracer");
        return false;
    }
    m_rtReady = true;
    return true;
}

bool Scene::ResizePathTracer(render::D3D12Device& device, uint32_t width, uint32_t height)
{
    if (!m_rtReady) return true;   // Nothing to resize; not a failure.
    return m_pathTracer.Resize(device.Device5(), width, height);
}

// =============================================================================
// Ensure BLAS exists for each unique mesh
// =============================================================================
void Scene::EnsureBLAS(render::D3D12Device& device)
{
    auto* dev5 = device.Device5();
    auto* cmd4 = device.CmdList4();
    if (!dev5 || !cmd4) return;

    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        const auto& meshInst = meshPool->DataAt(i);
        MeshHandle handle(meshInst.meshHandle);
        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || !gpuMesh->IsValid()) continue;

        // A recycled ResourceManager slot has a different full handle and must
        // build a new BLAS rather than inheriting the old mesh's geometry.
        if (!m_meshToBLAS.Contains(handle))
        {
            uint32_t blasIdx = m_pathTracer.GetAcceleration().BuildBLAS(dev5, cmd4, *gpuMesh);
            if (blasIdx != UINT32_MAX)
                m_meshToBLAS.Set(handle, blasIdx);
        }
    }
}

// =============================================================================
// Build Acceleration Structures (called once per frame before path tracing)
// =============================================================================
void Scene::BuildAccelerationStructures(render::D3D12Device& device,
                                         const core::Vec3d& cameraPosition)
{
    if (!m_rtReady) return;

    // Runtime mesh additions must receive a BLAS before TLAS extraction. The
    // startup path also calls EnsureBLAS explicitly, so this is normally a
    // cheap cache walk and only records work for newly observed handles.
    EnsureBLAS(device);

    auto* dev5 = device.Device5();
    auto* cmd4 = device.CmdList4();
    if (!dev5 || !cmd4) return;

    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    // Collect TLAS instances from all renderable entities
    std::vector<render::TLASInstance> instances;
    instances.reserve(meshPool->Count());

    uint32_t instanceID = 0;
    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        MeshHandle handle(meshInst.meshHandle);
        uint32_t blasIdx = UINT32_MAX;
        if (!m_meshToBLAS.TryGet(handle, blasIdx))
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        const auto& blas = m_pathTracer.GetAcceleration().GetBLAS(blasIdx);

        const auto& transform = m_registry.GetByIndex<ecs::Transform>(entityIdx);
        const core::Mat4x4 worldMat =
            transform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);

        render::TLASInstance inst = {};
        // Convert the engine's row-vector matrix to DXR's 3x4 instance
        // transform. Engine translation lives in row 3; DXR expects it in
        // the final column of the 3x4 descriptor.
        const float* m = worldMat.Data();
        inst.transform[0][0] = m[0]; inst.transform[0][1] = m[4]; inst.transform[0][2] = m[8];  inst.transform[0][3] = m[12];
        inst.transform[1][0] = m[1]; inst.transform[1][1] = m[5]; inst.transform[1][2] = m[9];  inst.transform[1][3] = m[13];
        inst.transform[2][0] = m[2]; inst.transform[2][1] = m[6]; inst.transform[2][2] = m[10]; inst.transform[2][3] = m[14];

        inst.instanceID    = instanceID;
        inst.instanceMask  = 0xFF;
        inst.hitGroupOffset = instanceID * 2; // 2 ray types per instance
        inst.instanceFlags = 0;
        inst.blasAddress   = blas.gpuAddress;

        instances.push_back(inst);
        instanceID++;
    }

    if (instances.empty()) return;

    // Build TLAS
    m_pathTracer.GetAcceleration().BuildTLAS(device,
        instances.data(), static_cast<uint32_t>(instances.size()));

    // Build shader table if instance count changed
    m_pathTracer.GetPipeline().BuildShaderTable(dev5,
        static_cast<uint32_t>(instances.size()));
}

// =============================================================================
// Dispatch Path Tracing
// =============================================================================
void Scene::PathTraceEntities(
    render::D3D12Device& device,
    const render::Camera& camera,
    const core::Vec3f& lightDir,
    const core::Vec3f& lightColor,
    const core::Vec3f& ambientColor,
    render::RTQualityMode qualityMode,
    const render::RTEnvironmentInputs& environment)
{
    if (!m_rtReady) return;

    const core::Vec3d& cameraPosition = camera.Position();
    uint64_t sceneSignature = kSceneHashOffset;
    HashSceneValue(sceneSignature, lightDir.x);
    HashSceneValue(sceneSignature, lightDir.y);
    HashSceneValue(sceneSignature, lightDir.z);
    HashSceneValue(sceneSignature, lightColor.x);
    HashSceneValue(sceneSignature, lightColor.y);
    HashSceneValue(sceneSignature, lightColor.z);
    HashSceneValue(sceneSignature, ambientColor.x);
    HashSceneValue(sceneSignature, ambientColor.y);
    HashSceneValue(sceneSignature, ambientColor.z);

    // Collect materials in instance order (matching TLAS instance IDs)
    auto* meshPool = m_registry.GetPool<ecs::MeshInstance>();
    if (!meshPool) return;

    std::vector<render::RTMaterialData> materials;
    std::vector<render::RTInstanceData> instanceData;
    std::vector<render::RTTriangleNormalData> triangleNormals;
    std::vector<render::RTTriangleUVData> triangleUVs;
    std::vector<render::RTTrianglePositionData> trianglePositions;
    std::vector<const render::Texture*> albedoTextures;
    std::vector<const render::Texture*> normalTextures;
    std::vector<const render::Texture*> ormTextures;
    std::vector<const render::Texture*> emissiveTextures;
    materials.reserve(meshPool->Count());
    instanceData.reserve(meshPool->Count());

    auto resolveTextureIndex = [&](uint32_t handleValue,
                                   std::vector<const render::Texture*>& textures,
                                   uint32_t maxTextures,
                                   const char* label) -> uint32_t
    {
        if (handleValue == UINT32_MAX)
            return UINT32_MAX;

        const render::Texture* texture = m_resources.GetTexture(TextureHandle(handleValue));
        if (!texture || !texture->IsValid())
            return UINT32_MAX;

        for (uint32_t i = 0; i < textures.size(); ++i)
        {
            if (textures[i] == texture)
                return i;
        }

        if (textures.size() >= maxTextures)
        {
            core::Log::Warnf("Path tracer %s texture table is full; material texture skipped", label);
            return UINT32_MAX;
        }

        textures.push_back(texture);
        return static_cast<uint32_t>(textures.size() - 1);
    };

    for (uint32_t i = 0; i < meshPool->Count(); i++)
    {
        uint32_t entityIdx = meshPool->EntityAt(i);
        const auto& meshInst = meshPool->DataAt(i);

        if (!meshInst.visible) continue;
        if (!m_registry.HasByIndex<ecs::Transform>(entityIdx)) continue;
        if (!m_registry.HasByIndex<ecs::Material>(entityIdx)) continue;

        MeshHandle handle(meshInst.meshHandle);
        if (!m_meshToBLAS.Contains(handle))
            continue;

        const render::Mesh* gpuMesh = m_resources.GetMesh(handle);
        if (!gpuMesh || gpuMesh->rtTriangleNormals.empty() ||
            gpuMesh->rtTriangleUVs.size() != gpuMesh->rtTriangleNormals.size() ||
            gpuMesh->rtTrianglePositions.size() != gpuMesh->rtTriangleNormals.size())
            continue;

        const auto& mat = m_registry.GetByIndex<ecs::Material>(entityIdx);
        const auto& instTransform = m_registry.GetByIndex<ecs::Transform>(entityIdx);

        HashSceneValue(sceneSignature, entityIdx);
        HashSceneValue(sceneSignature, meshInst.meshHandle);
        HashSceneValue(sceneSignature, instTransform.position.x);
        HashSceneValue(sceneSignature, instTransform.position.y);
        HashSceneValue(sceneSignature, instTransform.position.z);
        HashSceneValue(sceneSignature, instTransform.rotation.x);
        HashSceneValue(sceneSignature, instTransform.rotation.y);
        HashSceneValue(sceneSignature, instTransform.rotation.z);
        HashSceneValue(sceneSignature, instTransform.rotation.w);
        HashSceneValue(sceneSignature, instTransform.scale.x);
        HashSceneValue(sceneSignature, instTransform.scale.y);
        HashSceneValue(sceneSignature, instTransform.scale.z);
        HashSceneValue(sceneSignature, mat.albedo.r);
        HashSceneValue(sceneSignature, mat.albedo.g);
        HashSceneValue(sceneSignature, mat.albedo.b);
        HashSceneValue(sceneSignature, mat.albedo.a);
        HashSceneValue(sceneSignature, mat.roughness);
        HashSceneValue(sceneSignature, mat.metallic);
        HashSceneValue(sceneSignature, mat.albedoTextureHandle);
        HashSceneValue(sceneSignature, mat.normalTextureHandle);
        HashSceneValue(sceneSignature, mat.ormTextureHandle);
        HashSceneValue(sceneSignature, mat.emissive.r);
        HashSceneValue(sceneSignature, mat.emissive.g);
        HashSceneValue(sceneSignature, mat.emissive.b);
        HashSceneValue(sceneSignature, mat.emissive.a);
        HashSceneValue(sceneSignature, mat.emissiveStrength);
        HashSceneValue(sceneSignature, mat.emissiveTextureHandle);
        const uint32_t albedoTextureIndex =
            resolveTextureIndex(mat.albedoTextureHandle, albedoTextures,
                                render::kMaxRTAlbedoTextures, "albedo");
        const uint32_t ormTextureIndex =
            resolveTextureIndex(mat.ormTextureHandle, ormTextures,
                                render::kMaxRTOrmTextures, "orm");
        const uint32_t normalTextureIndex =
            resolveTextureIndex(mat.normalTextureHandle, normalTextures,
                                render::kMaxRTNormalTextures, "normal");
        const uint32_t emissiveTextureIndex =
            resolveTextureIndex(mat.emissiveTextureHandle, emissiveTextures,
                                render::kMaxRTEmissiveTextures, "emissive");

        render::RTMaterialData rtMat = {};
        rtMat.albedo[0]  = mat.albedo.r;
        rtMat.albedo[1]  = mat.albedo.g;
        rtMat.albedo[2]  = mat.albedo.b;
        rtMat.albedo[3]  = mat.albedo.a;
        rtMat.roughness   = mat.roughness;
        rtMat.metallic    = mat.metallic;
        rtMat.albedoTextureIndex = albedoTextureIndex == UINT32_MAX ? 0u : albedoTextureIndex;
        rtMat.useAlbedoTexture = albedoTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.normalTextureIndex = normalTextureIndex == UINT32_MAX ? 0u : normalTextureIndex;
        rtMat.useNormalTexture = normalTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.ormTextureIndex = ormTextureIndex == UINT32_MAX ? 0u : ormTextureIndex;
        rtMat.useOrmTexture = ormTextureIndex == UINT32_MAX ? 0u : 1u;
        rtMat.emissive[0] = mat.emissive.r;
        rtMat.emissive[1] = mat.emissive.g;
        rtMat.emissive[2] = mat.emissive.b;
        rtMat.emissiveStrength = mat.emissiveStrength;
        rtMat.emissiveTextureIndex =
            emissiveTextureIndex == UINT32_MAX ? 0u : emissiveTextureIndex;
        rtMat.useEmissiveTexture = emissiveTextureIndex == UINT32_MAX ? 0u : 1u;
        materials.push_back(rtMat);

        render::RTInstanceData rtInstance = {};
        rtInstance.triangleNormalOffset = static_cast<uint32_t>(triangleNormals.size());
        rtInstance.triangleUVOffset = static_cast<uint32_t>(triangleUVs.size());
        rtInstance.trianglePositionOffset = static_cast<uint32_t>(trianglePositions.size());

        // Normal matrix for this instance, stored transposed so the closest-hit
        // shader can evaluate component i as dot(row[i].xyz, objectNormal). Uses the
        // same InverseTranspose3x3 the raster path uses, so both paths now shade
        // identically under non-uniform scale.
        {
            const core::Mat4x4 instWorld =
                instTransform.ToCameraRelativeMatrix(cameraPosition, m_renderScale);
            const core::Mat4x4 normalMat = core::Mat4x4::InverseTranspose3x3(instWorld);
            for (int row = 0; row < 3; ++row)
            {
                rtInstance.normalMatrix[row * 4 + 0] = normalMat.m[0][row];
                rtInstance.normalMatrix[row * 4 + 1] = normalMat.m[1][row];
                rtInstance.normalMatrix[row * 4 + 2] = normalMat.m[2][row];
                rtInstance.normalMatrix[row * 4 + 3] = 0.0f;
            }
        }

        instanceData.push_back(rtInstance);

        triangleNormals.insert(triangleNormals.end(),
                               gpuMesh->rtTriangleNormals.begin(),
                               gpuMesh->rtTriangleNormals.end());
        triangleUVs.insert(triangleUVs.end(),
                           gpuMesh->rtTriangleUVs.begin(),
                           gpuMesh->rtTriangleUVs.end());
        trianglePositions.insert(trianglePositions.end(),
                                 gpuMesh->rtTrianglePositions.begin(),
                                 gpuMesh->rtTrianglePositions.end());
    }

    m_pathTracer.Dispatch(device, camera, lightDir, lightColor, ambientColor,
                          materials.data(), static_cast<uint32_t>(materials.size()),
                          instanceData.data(), static_cast<uint32_t>(instanceData.size()),
                          triangleNormals.data(), static_cast<uint32_t>(triangleNormals.size()),
                          triangleUVs.data(), static_cast<uint32_t>(triangleUVs.size()),
                          trianglePositions.data(), static_cast<uint32_t>(trianglePositions.size()),
                          albedoTextures.data(), static_cast<uint32_t>(albedoTextures.size()),
                          normalTextures.data(), static_cast<uint32_t>(normalTextures.size()),
                          ormTextures.data(), static_cast<uint32_t>(ormTextures.size()),
                          emissiveTextures.data(),
                          static_cast<uint32_t>(emissiveTextures.size()),
                          static_cast<uint32_t>(materials.size()),
                          sceneSignature,
                          qualityMode,
                          environment);
}

void Scene::CopyPathTraceToBackBuffer(render::D3D12Device& device)
{
    if (m_rtReady)
        m_pathTracer.CopyToBackBuffer(device);
}

} // namespace scene
