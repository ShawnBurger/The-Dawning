#include "interior_lighting.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

namespace ship
{
namespace
{

constexpr double kPi = 3.14159265358979323846;
constexpr float kQuaternionUnitTolerance = 1.0e-3f;

bool Finite(double value)
{
    return std::isfinite(value);
}

bool Finite(float value)
{
    return std::isfinite(value);
}

bool Finite(const core::Vec3d& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Vec3f& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const core::Quatf& value)
{
    return Finite(value.x) && Finite(value.y) && Finite(value.z) &&
           Finite(value.w);
}

bool ValidConfig(const InteriorLightFrameConfig& config)
{
    const double maximumFloat = (std::numeric_limits<float>::max)();
    return config.maxLights > 0 &&
           Finite(config.maximumCameraRelativeMagnitude) &&
           config.maximumCameraRelativeMagnitude > 0.0 &&
           config.maximumCameraRelativeMagnitude <= maximumFloat &&
           Finite(config.uniformScaleTolerance) &&
           config.uniformScaleTolerance > 0.0 &&
           config.uniformScaleTolerance <= 0.01 &&
           Finite(config.minimumScale) && config.minimumScale > 0.0 &&
           Finite(config.maximumScale) &&
           config.maximumScale >= config.minimumScale &&
           Finite(config.maximumResolvedIntensity) &&
           config.maximumResolvedIntensity > 0.0 &&
           config.maximumResolvedIntensity <= maximumFloat;
}

bool ValidId(std::string_view value)
{
    if (value.empty())
        return false;
    bool separator = true;
    for (const unsigned char character : value)
    {
        const bool alphanumeric =
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9');
        const bool currentSeparator =
            character == '.' || character == '_' || character == '-';
        if ((!alphanumeric && !currentSeparator) ||
            (separator && currentSeparator))
        {
            return false;
        }
        separator = currentSeparator;
    }
    return !separator;
}

bool KnownType(asset::AssemblyLightType type)
{
    return type == asset::AssemblyLightType::Point ||
           type == asset::AssemblyLightType::Spot;
}

bool KnownShadowPolicy(asset::AssemblyLightShadowPolicy policy)
{
    return policy == asset::AssemblyLightShadowPolicy::None ||
           policy == asset::AssemblyLightShadowPolicy::Static ||
           policy == asset::AssemblyLightShadowPolicy::Dynamic;
}

bool ValidTransform(
    const ecs::Transform& transform,
    const InteriorLightFrameConfig& config,
    double& uniformScale)
{
    if (!Finite(transform.position) || !Finite(transform.rotation) ||
        !Finite(transform.scale))
    {
        return false;
    }
    const float rotationLengthSquared = transform.rotation.LengthSq();
    if (!Finite(rotationLengthSquared) ||
        std::abs(rotationLengthSquared - 1.0f) > kQuaternionUnitTolerance)
    {
        return false;
    }
    const double x = transform.scale.x;
    const double y = transform.scale.y;
    const double z = transform.scale.z;
    uniformScale = (std::max)({ x, y, z });
    return x >= config.minimumScale && y >= config.minimumScale &&
           z >= config.minimumScale && x <= config.maximumScale &&
           y <= config.maximumScale && z <= config.maximumScale &&
           std::abs(x - y) <= config.uniformScaleTolerance * uniformScale &&
           std::abs(x - z) <= config.uniformScaleTolerance * uniformScale;
}

double Clamp(double value, double minimum, double maximum)
{
    return (std::max)(minimum, (std::min)(maximum, value));
}

double SrgbToLinear(double value)
{
    return value <= 0.04045
        ? value / 12.92
        : std::pow((value + 0.055) / 1.055, 2.4);
}

core::Vec3f ColorTemperatureToLinearRgb(double kelvin)
{
    const double temperature = Clamp(kelvin, 1000.0, 20000.0) / 100.0;
    double red = 255.0;
    double green = 255.0;
    double blue = 255.0;
    if (temperature <= 66.0)
    {
        green = 99.4708025861 * std::log(temperature) - 161.1195681661;
        blue = temperature <= 19.0
            ? 0.0
            : 138.5177312231 * std::log(temperature - 10.0) -
                305.0447927307;
    }
    else
    {
        red = 329.698727446 * std::pow(temperature - 60.0, -0.1332047592);
        green = 288.1221695283 *
            std::pow(temperature - 60.0, -0.0755148492);
    }
    const double linearRed = SrgbToLinear(Clamp(red, 0.0, 255.0) / 255.0);
    const double linearGreen = SrgbToLinear(Clamp(green, 0.0, 255.0) / 255.0);
    const double linearBlue = SrgbToLinear(Clamp(blue, 0.0, 255.0) / 255.0);
    const double luminance = 0.2126 * linearRed + 0.7152 * linearGreen +
                             0.0722 * linearBlue;
    if (!Finite(luminance) || luminance <= 1.0e-8)
        return { 1.0f, 1.0f, 1.0f };
    return {
        static_cast<float>(linearRed / luminance),
        static_cast<float>(linearGreen / luminance),
        static_cast<float>(linearBlue / luminance)
    };
}

InteriorLightingResult Failure(
    InteriorLightingStatus status,
    std::string error,
    uint32_t stableIndex = asset::kAssemblyNoIndex)
{
    InteriorLightingResult result;
    result.status = status;
    result.failedStableIndex = stableIndex;
    result.error = std::move(error);
    return result;
}

bool ValidResolvedLight(
    const ResolvedInteriorLight& light,
    uint32_t expectedIndex,
    const InteriorLightFrameConfig& config)
{
    const double directionLengthSquared =
        light.localDirection[0] * light.localDirection[0] +
        light.localDirection[1] * light.localDirection[1] +
        light.localDirection[2] * light.localDirection[2];
    const bool pointCones = light.innerConeDegrees == 180.0 &&
                            light.outerConeDegrees == 180.0;
    const bool spotCones = light.innerConeDegrees >= 0.0 &&
                           light.innerConeDegrees < light.outerConeDegrees &&
                           light.outerConeDegrees <= 89.9;
    return light.stableIndex == expectedIndex && ValidId(light.id) &&
           KnownType(light.type) && KnownShadowPolicy(light.shadowPolicy) &&
           Finite(light.localPositionMeters[0]) &&
           Finite(light.localPositionMeters[1]) &&
           Finite(light.localPositionMeters[2]) &&
           Finite(light.localDirection[0]) && Finite(light.localDirection[1]) &&
           Finite(light.localDirection[2]) &&
           Finite(directionLengthSquared) &&
           std::abs(directionLengthSquared - 1.0) <= 2.0e-4 &&
           Finite(light.colorTemperatureKelvin) &&
           light.colorTemperatureKelvin >= 1000.0 &&
           light.colorTemperatureKelvin <= 20000.0 &&
           Finite(light.intensityLumensOrCandela) &&
           light.intensityLumensOrCandela >= 0.0 &&
           light.intensityLumensOrCandela <= config.maximumResolvedIntensity &&
           Finite(light.rangeMeters) && light.rangeMeters > 0.0 &&
           light.rangeMeters <= 10000.0 &&
           Finite(light.innerConeDegrees) && Finite(light.outerConeDegrees) &&
           (light.type == asset::AssemblyLightType::Point
                ? pointCones
                : spotCones) &&
           Finite(light.importance) && light.importance >= 0.0 &&
           light.importance <= 1.0;
}

} // namespace

const char* InteriorLightingStatusName(InteriorLightingStatus status)
{
    switch (status)
    {
    case InteriorLightingStatus::Success: return "success";
    case InteriorLightingStatus::InvalidArgument: return "invalid_argument";
    case InteriorLightingStatus::InvalidSnapshot: return "invalid_snapshot";
    case InteriorLightingStatus::InvalidTransform: return "invalid_transform";
    case InteriorLightingStatus::ResourceLimitExceeded:
        return "resource_limit_exceeded";
    case InteriorLightingStatus::ArithmeticOverflow:
        return "arithmetic_overflow";
    case InteriorLightingStatus::AllocationFailure: return "allocation_failure";
    case InteriorLightingStatus::InternalError: return "internal_error";
    }
    return "unknown";
}

InteriorLightingResult BuildInteriorLightFrame(
    const ShipInteriorSnapshot& snapshot,
    const InteriorModulePoseView& modulePoses,
    const core::Vec3d& cameraWorldPosition,
    InteriorLightFrame& output,
    const InteriorLightFrameConfig& config)
{
    if (!ValidConfig(config) || !Finite(cameraWorldPosition))
    {
        return Failure(
            InteriorLightingStatus::InvalidArgument,
            "lighting configuration or camera position is invalid");
    }
    if (snapshot.topologyRevision == 0 || snapshot.stateRevision == 0 ||
        modulePoses.topologyRevision != snapshot.topologyRevision ||
        modulePoses.topologySha256 != snapshot.topologySha256 ||
        snapshot.resolvedLights.size() != snapshot.fixtures.size())
    {
        return Failure(
            InteriorLightingStatus::InvalidSnapshot,
            "interior snapshot is incomplete");
    }
    if (snapshot.resolvedLights.size() > config.maxLights)
    {
        return Failure(
            InteriorLightingStatus::ResourceLimitExceeded,
            "resolved fixture count exceeds the frame limit");
    }

    try
    {
        InteriorLightFrame staged;
        staged.topologySha256 = snapshot.topologySha256;
        staged.topologyRevision = snapshot.topologyRevision;
        staged.stateRevision = snapshot.stateRevision;
        staged.simulationTick = snapshot.simulationTick;
        staged.lights.reserve(snapshot.resolvedLights.size());

        std::string_view previousId;
        for (size_t index = 0; index < snapshot.resolvedLights.size(); ++index)
        {
            const ResolvedInteriorLight& light = snapshot.resolvedLights[index];
            if (!ValidResolvedLight(light, static_cast<uint32_t>(index), config) ||
                snapshot.fixtures[index].id != light.id ||
                (!previousId.empty() && previousId >= light.id))
            {
                return Failure(
                    InteriorLightingStatus::InvalidSnapshot,
                    "resolved light table is malformed or not in stable order",
                    static_cast<uint32_t>(index));
            }
            previousId = light.id;
            if (light.moduleIndex >= modulePoses.worldTransforms.size())
            {
                return Failure(
                    InteriorLightingStatus::InvalidSnapshot,
                    "resolved light references a missing module transform",
                    static_cast<uint32_t>(index));
            }

            const ecs::Transform& module =
                modulePoses.worldTransforms[light.moduleIndex];
            double uniformScale = 0.0;
            if (!ValidTransform(module, config, uniformScale))
            {
                return Failure(
                    InteriorLightingStatus::InvalidTransform,
                    "module world transform is invalid for physical lighting",
                    static_cast<uint32_t>(index));
            }
            if (light.intensityLumensOrCandela == 0.0)
                continue;

            const double scaledX = light.localPositionMeters[0] * uniformScale;
            const double scaledY = light.localPositionMeters[1] * uniformScale;
            const double scaledZ = light.localPositionMeters[2] * uniformScale;
            const double maximumFloat = (std::numeric_limits<float>::max)();
            if (!Finite(scaledX) || !Finite(scaledY) || !Finite(scaledZ) ||
                std::abs(scaledX) > maximumFloat ||
                std::abs(scaledY) > maximumFloat ||
                std::abs(scaledZ) > maximumFloat)
            {
                return Failure(
                    InteriorLightingStatus::ArithmeticOverflow,
                    "fixture local position cannot be represented",
                    static_cast<uint32_t>(index));
            }
            const core::Quatf rotation = module.rotation.Normalized();
            const core::Vec3f localOffset = rotation.Rotate({
                static_cast<float>(scaledX),
                static_cast<float>(scaledY),
                static_cast<float>(scaledZ)
            });
            const core::Vec3d relativeDouble =
                (module.position - cameraWorldPosition) +
                core::Vec3d::FromFloat(localOffset);
            if (!Finite(relativeDouble) ||
                std::abs(relativeDouble.x) >
                    config.maximumCameraRelativeMagnitude ||
                std::abs(relativeDouble.y) >
                    config.maximumCameraRelativeMagnitude ||
                std::abs(relativeDouble.z) >
                    config.maximumCameraRelativeMagnitude)
            {
                return Failure(
                    InteriorLightingStatus::ArithmeticOverflow,
                    "camera-relative fixture position exceeds the frame range",
                    static_cast<uint32_t>(index));
            }

            const core::Vec3f direction = rotation.Rotate({
                static_cast<float>(light.localDirection[0]),
                static_cast<float>(light.localDirection[1]),
                static_cast<float>(light.localDirection[2])
            }).Normalized();
            if (!Finite(direction) || direction.LengthSq() < 0.999f)
            {
                return Failure(
                    InteriorLightingStatus::ArithmeticOverflow,
                    "fixture direction cannot be represented",
                    static_cast<uint32_t>(index));
            }
            const double scaledRange = light.rangeMeters * uniformScale;
            if (!Finite(scaledRange) || scaledRange <= 0.0 ||
                scaledRange > maximumFloat)
            {
                return Failure(
                    InteriorLightingStatus::ArithmeticOverflow,
                    "fixture range cannot be represented",
                    static_cast<uint32_t>(index));
            }

            InteriorGpuLight gpu;
            gpu.positionCameraRelative = relativeDouble.ToFloat();
            gpu.rangeMeters = static_cast<float>(scaledRange);
            gpu.direction = direction;
            gpu.intensityLumensOrCandela =
                static_cast<float>(light.intensityLumensOrCandela);
            gpu.colorLinear =
                ColorTemperatureToLinearRgb(light.colorTemperatureKelvin);
            gpu.innerConeCosine = static_cast<float>(std::cos(
                light.innerConeDegrees * kPi / 180.0));
            gpu.outerConeCosine = static_cast<float>(std::cos(
                light.outerConeDegrees * kPi / 180.0));
            gpu.importance = static_cast<float>(light.importance);
            gpu.stableIndex = light.stableIndex;
            gpu.moduleIndex = light.moduleIndex;
            gpu.type = light.type;
            gpu.shadowPolicy = light.shadowPolicy;
            staged.lights.push_back(gpu);
        }

        output = std::move(staged);
        return { InteriorLightingStatus::Success };
    }
    catch (const std::bad_alloc&)
    {
        return Failure(
            InteriorLightingStatus::AllocationFailure,
            "allocation failure while building interior light frame");
    }
    catch (...)
    {
        return Failure(
            InteriorLightingStatus::InternalError,
            "unexpected failure while building interior light frame");
    }
}

} // namespace ship
