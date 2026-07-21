// =============================================================================
// sim/atmosphere_system.cpp - frame-aware ECS atmospheric-flight adapter
// =============================================================================

#include "atmosphere_system.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sim
{

namespace
{

bool IsFinite(const Vec3d& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsFinite(const core::Vec3f& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsSafeLocal(const Vec3d& value)
{
    return IsFinite(value) &&
           std::fabs(value.x) <= kSectorSize &&
           std::fabs(value.y) <= kSectorSize &&
           std::fabs(value.z) <= kSectorSize;
}

bool IsUsableFrame(const Frame& frame)
{
    return ValidSector(frame.origin) && IsCanonical(frame.origin) &&
           IsFinite(frame.velocity);
}

bool IsValidModel(const AtmosphereModel& model)
{
    if (model.kind == AtmosphereKind::None)
        return true;
    if (model.kind != AtmosphereKind::Exponential &&
        model.kind != AtmosphereKind::USSA76)
        return false;

    if (!std::isfinite(model.ceiling) ||
        !std::isfinite(model.ceilingFadeWidth) ||
        model.ceiling <= 0.0 || model.ceilingFadeWidth <= 0.0)
        return false;

    if (model.kind == AtmosphereKind::USSA76)
        return true;

    return std::isfinite(model.seaLevelDensity) &&
           std::isfinite(model.scaleHeight) &&
           std::isfinite(model.gasConstant) &&
           std::isfinite(model.gamma) &&
           model.seaLevelDensity > 0.0 && model.scaleHeight > 0.0 &&
           model.gasConstant > 0.0 && model.gamma > 0.0;
}

bool IsValidAero(const ecs::AerodynamicBody& aero)
{
    return std::isfinite(aero.referenceArea) && aero.referenceArea > 0.0 &&
           std::isfinite(aero.baseDragCoefficient) &&
           aero.baseDragCoefficient > 0.0 &&
           std::isfinite(aero.angleOfAttackDrag) &&
           aero.angleOfAttackDrag >= 0.0 &&
           std::isfinite(aero.liftSlope) && aero.liftSlope >= 0.0 &&
           std::isfinite(aero.stallAngleRadians) &&
           aero.stallAngleRadians > 0.0 &&
           std::isfinite(aero.noseRadius) && aero.noseRadius > 0.0 &&
           IsFinite(aero.liftAxis) && IsFinite(aero.centerOfPressure) &&
           (aero.liftSlope == 0.0 || aero.liftAxis.LengthSq() > 1.0e-12f);
}

bool IsKnownOwner(ecs::OrbitOwner owner)
{
    return owner == ecs::OrbitOwner::NBodyActive ||
           owner == ecs::OrbitOwner::OnRails ||
           owner == ecs::OrbitOwner::ForceIntegrated;
}

bool TryNormalize(const core::Quatf& value, core::Quatf& out)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        !std::isfinite(value.z) || !std::isfinite(value.w))
        return false;

    const double scale = (std::max)({ std::fabs(static_cast<double>(value.x)),
                                      std::fabs(static_cast<double>(value.y)),
                                      std::fabs(static_cast<double>(value.z)),
                                      std::fabs(static_cast<double>(value.w)) });
    if (!(scale > 0.0) || !std::isfinite(scale))
        return false;

    const double x = static_cast<double>(value.x) / scale;
    const double y = static_cast<double>(value.y) / scale;
    const double z = static_cast<double>(value.z) / scale;
    const double w = static_cast<double>(value.w) / scale;
    const double length = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(length > 0.0) || !std::isfinite(length))
        return false;

    out = core::Quatf{ static_cast<float>(x / length),
                       static_cast<float>(y / length),
                       static_cast<float>(z / length),
                       static_cast<float>(w / length) };
    return true;
}

bool TryRotate(const core::Quatf& rotation, const Vec3d& value, Vec3d& out)
{
    if (!IsFinite(value))
        return false;

    const double scale = (std::max)({ std::fabs(value.x), std::fabs(value.y),
                                      std::fabs(value.z) });
    if (scale == 0.0)
    {
        out = Vec3d{};
        return true;
    }

    const Vec3d operand = value / scale;
    const Vec3d qv{ static_cast<double>(rotation.x),
                    static_cast<double>(rotation.y),
                    static_cast<double>(rotation.z) };
    const Vec3d uv = qv.Cross(operand);
    const Vec3d uuv = qv.Cross(uv);
    out = (operand + (uv * static_cast<double>(rotation.w) + uuv) * 2.0) * scale;
    return IsFinite(out);
}

bool TryCross(const Vec3d& a, const Vec3d& b, Vec3d& out)
{
    if (!IsFinite(a) || !IsFinite(b))
        return false;
    out = a.Cross(b);
    return IsFinite(out);
}

bool TryUnit(const Vec3d& value, Vec3d& unit, double& length)
{
    if (!IsFinite(value))
        return false;
    const double scale = (std::max)({ std::fabs(value.x), std::fabs(value.y),
                                      std::fabs(value.z) });
    if (scale == 0.0)
    {
        unit = Vec3d{};
        length = 0.0;
        return true;
    }
    const Vec3d scaled = value / scale;
    const double scaledLength = std::sqrt(scaled.LengthSq());
    length = scale * scaledLength;
    unit = scaled / scaledLength;
    return std::isfinite(length) && IsFinite(unit);
}

bool TryNarrow(const Vec3d& value, core::Vec3f& out)
{
    constexpr double kFloatMax =
        static_cast<double>((std::numeric_limits<float>::max)());
    if (!IsFinite(value) || std::fabs(value.x) > kFloatMax ||
        std::fabs(value.y) > kFloatMax || std::fabs(value.z) > kFloatMax)
        return false;
    out = core::Vec3f{ static_cast<float>(value.x),
                       static_cast<float>(value.y),
                       static_cast<float>(value.z) };
    return IsFinite(out);
}

bool IsFiniteState(const AtmosphereState& state)
{
    return std::isfinite(state.temperature) && std::isfinite(state.pressure) &&
           std::isfinite(state.density) && std::isfinite(state.speedOfSound) &&
           state.density >= 0.0 && state.pressure >= 0.0 &&
           state.temperature >= 0.0 && state.speedOfSound >= 0.0;
}

} // namespace

AtmosphereStepResult ApplyAtmosphereToEntity(ecs::Registry& registry,
                                             ecs::Entity entity,
                                             const FrameGraph& frames,
                                             const AtmosphereEnvironment& environment,
                                             double dt)
{
    AtmosphereStepResult result;

    ecs::Transform* transform = registry.TryGet<ecs::Transform>(entity);
    ecs::SpatialFrame* spatialFrame = registry.TryGet<ecs::SpatialFrame>(entity);
    ecs::RigidBody* rigidBody = registry.TryGet<ecs::RigidBody>(entity);
    const ecs::AerodynamicBody* aero =
        registry.TryGet<ecs::AerodynamicBody>(entity);
    ecs::GravitationalBody* gravitational =
        registry.TryGet<ecs::GravitationalBody>(entity);
    if (!transform || !spatialFrame || !rigidBody || !aero ||
        !(dt > 0.0) || !std::isfinite(dt) ||
        (gravitational && !IsKnownOwner(gravitational->owner)))
        return result;

    const FrameId frameId = static_cast<FrameId>(spatialFrame->frameId);
    core::Quatf orientation;
    if (frameId >= frames.FrameCount() || !IsSafeLocal(transform->position) ||
        !IsUsableFrame(frames.GetFrame(frameId)) ||
        !ValidSector(environment.center) || !IsCanonical(environment.center) ||
        !IsFinite(environment.linearVelocity) ||
        !IsFinite(environment.angularVelocity) ||
        !std::isfinite(environment.radius) || environment.radius <= 0.0 ||
        !IsValidModel(environment.model) || !IsValidAero(*aero) ||
        !TryNormalize(transform->rotation, orientation) ||
        !IsFinite(rigidBody->linearVelocity) ||
        !IsFinite(rigidBody->forceAccum) ||
        !IsFinite(rigidBody->angularVelocity) ||
        !IsFinite(rigidBody->torqueAccum) ||
        !std::isfinite(rigidBody->invMass) || !(rigidBody->invMass > 0.0))
        return result;

    const double mass = 1.0 / rigidBody->invMass;
    if (!std::isfinite(mass) || !(mass > 0.0))
        return result;

    const Body frameBody{ frameId, transform->position,
                          rigidBody->linearVelocity };
    const WorldPos worldPosition = frames.ResolveWorldPos(frameBody);
    const Vec3d worldVelocity = frames.ResolveWorldVel(frameBody);
    if (!ValidSector(worldPosition) || !IsCanonical(worldPosition) ||
        !IsFinite(worldVelocity))
        return result;

    const Vec3d radial = Separation(environment.center, worldPosition);
    Vec3d ignoredRadialDirection;
    double radialDistance = 0.0;
    if (!TryUnit(radial, ignoredRadialDirection, radialDistance))
        return result;
    const double geometricAltitude = radialDistance - environment.radius;
    if (!std::isfinite(geometricAltitude))
        return result;
    const double sampleAltitude = environment.model.kind == AtmosphereKind::USSA76
        ? GeometricToGeopotential(geometricAltitude)
        : geometricAltitude;
    const AtmosphereState atmosphere =
        SampleAtmosphere(environment.model, sampleAltitude);
    if (!IsFiniteState(atmosphere))
        return result;

    result.accepted = true;
    result.density = atmosphere.density;
    if (!(atmosphere.density > 0.0))
        return result;

    Vec3d rotationalVelocity;
    if (!TryCross(environment.angularVelocity, radial, rotationalVelocity))
        return AtmosphereStepResult{};
    const Vec3d airVelocity = environment.linearVelocity + rotationalVelocity;
    if (!IsFinite(airVelocity))
        return AtmosphereStepResult{};

    const Vec3d airspeed = AirspeedVector(worldVelocity, airVelocity);
    Vec3d airspeedUnit;
    double airspeedMagnitude = 0.0;
    if (!TryUnit(airspeed, airspeedUnit, airspeedMagnitude))
        return AtmosphereStepResult{};

    result.inAtmosphere = true;
    result.dynamicPressure = DynamicPressure(atmosphere.density, airspeed);
    result.mach = MachNumber(airspeed, atmosphere.speedOfSound);
    result.heatFlux = SuttonGravesHeatFlux(
        atmosphere.density, aero->noseRadius, airspeed);

    Vec3d forward;
    Vec3d liftAxis;
    if (!TryRotate(orientation, Vec3d{ 0.0, 0.0, 1.0 }, forward) ||
        !TryRotate(orientation, Vec3d::FromFloat(aero->liftAxis), liftAxis))
        return AtmosphereStepResult{};

    Vec3d forwardUnit;
    Vec3d liftAxisUnit;
    double ignoredLength = 0.0;
    if (!TryUnit(forward, forwardUnit, ignoredLength) ||
        !TryUnit(liftAxis, liftAxisUnit, ignoredLength))
        return AtmosphereStepResult{};

    result.angleOfAttack = airspeedMagnitude > 0.0
        ? std::atan2(-airspeedUnit.Dot(liftAxisUnit),
                     airspeedUnit.Dot(forwardUnit))
        : 0.0;
    const double dragCoefficient = DragCoefficientAtMach(
        aero->baseDragCoefficient, result.mach) *
        AngleOfAttackDragFactor(result.angleOfAttack,
                                aero->angleOfAttackDrag);
    if (!std::isfinite(dragCoefficient) || !(dragCoefficient > 0.0))
        return AtmosphereStepResult{};

    const Vec3d nextAirspeed = SemiImplicitDragAirspeed(
        airspeed, atmosphere.density, dragCoefficient,
        aero->referenceArea, mass, dt);
    const Vec3d nextWorldVelocity = airVelocity + nextAirspeed;
    const Vec3d nextLocalVelocity =
        nextWorldVelocity - frames.GetFrame(frameId).velocity;
    if (!IsFinite(nextAirspeed) || !IsFinite(nextLocalVelocity))
        return AtmosphereStepResult{};

    Vec3d liftDirection;
    double liftDirectionLength = 0.0;
    if (airspeedMagnitude > 0.0)
    {
        const Vec3d projected =
            liftAxisUnit - airspeedUnit * liftAxisUnit.Dot(airspeedUnit);
        if (!TryUnit(projected, liftDirection, liftDirectionLength))
            return AtmosphereStepResult{};
    }

    const double liftCoefficient = LiftCoefficient(
        aero->liftSlope, result.angleOfAttack, aero->stallAngleRadians);
    const Vec3d liftForce = liftDirectionLength > 0.0
        ? LiftForce(result.dynamicPressure, liftCoefficient,
                    aero->referenceArea, liftDirection)
        : Vec3d{};

    const Vec3d equivalentDragForce =
        (nextAirspeed - airspeed) * (mass / dt);
    const Vec3d totalAeroForce = equivalentDragForce + liftForce;
    if (!IsFinite(liftForce) || !IsFinite(equivalentDragForce) ||
        !IsFinite(totalAeroForce))
        return AtmosphereStepResult{};

    Vec3d bodyAeroForce;
    if (!TryRotate(orientation.Conjugate(), totalAeroForce, bodyAeroForce))
        return AtmosphereStepResult{};
    Vec3d bodyTorque;
    if (!TryCross(Vec3d::FromFloat(aero->centerOfPressure),
                  bodyAeroForce, bodyTorque))
        return AtmosphereStepResult{};

    const bool enteringForceIntegration = gravitational &&
        gravitational->owner != ecs::OrbitOwner::ForceIntegrated;
    const Vec3d baseForceAccum = enteringForceIntegration
        ? Vec3d{}
        : rigidBody->forceAccum;
    const Vec3d baseTorqueAccum = enteringForceIntegration
        ? Vec3d{}
        : Vec3d::FromFloat(rigidBody->torqueAccum);
    const Vec3d nextForceAccum = baseForceAccum + liftForce;
    const Vec3d nextTorqueAccum =
        baseTorqueAccum + bodyTorque;
    core::Vec3f narrowedTorque;
    if (!IsFinite(nextForceAccum) ||
        !TryNarrow(nextTorqueAccum, narrowedTorque))
        return AtmosphereStepResult{};

    ecs::RigidBody nextRigidBody = *rigidBody;
    nextRigidBody.linearVelocity = nextLocalVelocity;
    nextRigidBody.forceAccum = nextForceAccum;
    nextRigidBody.torqueAccum = narrowedTorque;

    ecs::GravitationalBody nextGravitational;
    if (gravitational)
    {
        nextGravitational = *gravitational;
        nextGravitational.owner = ecs::OrbitOwner::ForceIntegrated;
    }

    *rigidBody = nextRigidBody;
    if (gravitational)
        *gravitational = nextGravitational;
    return result;
}

} // namespace sim
