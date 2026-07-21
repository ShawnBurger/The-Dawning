#include "test_framework.h"

#include "scene/assembly_interior_runtime.h"
#include "scene/assembly_presentation.h"

#include <limits>
#include <string>
#include <vector>

namespace
{

scene::PreparedAssemblyModule Module(
    uint32_t stableIndex,
    const ecs::Transform& local)
{
    scene::PreparedAssemblyModule module;
    module.stableIndex = stableIndex;
    module.localTransform = local;
    return module;
}

scene::PreparedAssemblyMovingPart MovingPart(
    uint32_t stableIndex,
    uint32_t moduleIndex,
    const ecs::Transform& local)
{
    scene::PreparedAssemblyMovingPart part;
    part.stableIndex = stableIndex;
    part.moduleIndex = moduleIndex;
    part.interactionIndex = stableIndex;
    part.localTransform = local;
    return part;
}

void CheckPosition(
    const core::Vec3d& actual,
    const core::Vec3d& expected,
    double epsilon = 1.0e-5)
{
    CHECK_APPROX_EPS(actual.x, expected.x, epsilon);
    CHECK_APPROX_EPS(actual.y, expected.y, epsilon);
    CHECK_APPROX_EPS(actual.z, expected.z, epsilon);
}

void CheckRotationDirection(
    const core::Quatf& actual,
    const core::Quatf& expected)
{
    const core::Vec3f probe{ 0.3f, -0.4f, 0.5f };
    const core::Vec3f actualDirection = actual.Rotate(probe);
    const core::Vec3f expectedDirection = expected.Rotate(probe);
    CHECK_APPROX_EPS(actualDirection.x, expectedDirection.x, 1.0e-5f);
    CHECK_APPROX_EPS(actualDirection.y, expectedDirection.y, 1.0e-5f);
    CHECK_APPROX_EPS(actualDirection.z, expectedDirection.z, 1.0e-5f);
}

} // namespace

TEST_CASE(AssemblyPresentation_ComposesCompleteBatchThroughLiveRoot)
{
    ecs::Transform moduleA;
    moduleA.position = { 2.0, -1.0, 3.0 };
    moduleA.rotation = core::Quatf::FromEuler(0.1f, 0.2f, -0.3f);
    moduleA.scale = { 1.0f, 2.0f, 0.5f };
    ecs::Transform moduleB;
    moduleB.position = { -4.0, 0.5, 1.0 };
    moduleB.rotation = core::Quatf::FromEuler(-0.2f, 0.4f, 0.1f);

    std::vector<scene::PreparedAssemblyModule> modules;
    modules.push_back(Module(0, moduleA));
    modules.push_back(Module(1, moduleB));

    ecs::Transform movingLocal = moduleB;
    movingLocal.position = { -3.25, 0.5, 1.0 };
    movingLocal.rotation = (
        moduleB.rotation *
        core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, 0.75f)).Normalized();
    std::vector<scene::PreparedAssemblyMovingPart> movingParts;
    movingParts.push_back(MovingPart(0, 1, moduleB));
    std::vector<ecs::Transform> movingLocalTransforms{ movingLocal };

    ecs::Transform root;
    root.position = { 9.0e12, -2.0e12, 7.0e12 };
    root.rotation = core::Quatf::FromEuler(0.35f, -0.6f, 0.8f);
    root.scale = { 2.0f, 2.0f, 2.0f };

    std::vector<ecs::Transform> moduleWorld(modules.size());
    std::vector<ecs::Transform> movingWorld(movingParts.size());
    const scene::AssemblyPresentationResult result =
        scene::StageAssemblyPresentation(
            root,
            modules,
            movingParts,
            movingLocalTransforms,
            moduleWorld,
            movingWorld);

    CHECK(result.Succeeded());
    const core::Vec3d expectedModulePosition = root.position +
        core::Vec3d::FromFloat(root.rotation.Rotate({ 4.0f, -2.0f, 6.0f }));
    CheckPosition(moduleWorld[0].position, expectedModulePosition, 0.01);
    CheckRotationDirection(
        moduleWorld[0].rotation,
        (root.rotation * moduleA.rotation).Normalized());
    CHECK_APPROX_EPS(moduleWorld[0].scale.x, 2.0f, 1.0e-6f);
    CHECK_APPROX_EPS(moduleWorld[0].scale.y, 4.0f, 1.0e-6f);
    CHECK_APPROX_EPS(moduleWorld[0].scale.z, 1.0f, 1.0e-6f);

    const core::Vec3d expectedMovingPosition = root.position +
        core::Vec3d::FromFloat(root.rotation.Rotate({ -6.5f, 1.0f, 2.0f }));
    CheckPosition(movingWorld[0].position, expectedMovingPosition, 0.01);
    CheckRotationDirection(
        movingWorld[0].rotation,
        (root.rotation * movingLocal.rotation).Normalized());
}

TEST_CASE(AssemblyPresentation_RootMotionNeverAccumulatesChildDrift)
{
    ecs::Transform moduleLocal;
    moduleLocal.position = { 3.0, 0.0, -2.0 };
    std::vector<scene::PreparedAssemblyModule> modules{
        Module(0, moduleLocal)
    };

    asset::AssemblyMovingPart authoredPart;
    authoredPart.motionType = asset::AssemblyMotionType::Rotational;
    authoredPart.axis = { 0.0, 1.0, 0.0 };
    authoredPart.pivotMeters = { 0.5, 0.0, 0.0 };
    authoredPart.travel = 100.0;
    ecs::Transform movingLocal;
    CHECK(scene::BuildAssemblyMovingPartTransform(
        authoredPart, moduleLocal, moduleLocal, 0.65, movingLocal));

    std::vector<scene::PreparedAssemblyMovingPart> parts{
        MovingPart(0, 0, moduleLocal)
    };
    std::vector<ecs::Transform> movingLocals{ movingLocal };
    std::vector<ecs::Transform> moduleWorld(1);
    std::vector<ecs::Transform> movingWorld(1);

    ecs::Transform rootA;
    rootA.position = { 100.0, 20.0, -30.0 };
    rootA.rotation = core::Quatf::FromEuler(0.2f, 0.4f, -0.6f);
    CHECK(scene::StageAssemblyPresentation(
        rootA, modules, parts, movingLocals, moduleWorld, movingWorld).Succeeded());
    const ecs::Transform firstModule = moduleWorld[0];
    const ecs::Transform firstMoving = movingWorld[0];

    ecs::Transform rootB = rootA;
    rootB.position = { -500.0, 75.0, 900.0 };
    rootB.rotation = core::Quatf::FromEuler(-0.7f, 0.1f, 1.0f);
    CHECK(scene::StageAssemblyPresentation(
        rootB, modules, parts, movingLocals, moduleWorld, movingWorld).Succeeded());
    CHECK(scene::StageAssemblyPresentation(
        rootA, modules, parts, movingLocals, moduleWorld, movingWorld).Succeeded());

    CheckPosition(moduleWorld[0].position, firstModule.position);
    CheckPosition(movingWorld[0].position, firstMoving.position);
    CheckRotationDirection(moduleWorld[0].rotation, firstModule.rotation);
    CheckRotationDirection(movingWorld[0].rotation, firstMoving.rotation);
}

TEST_CASE(AssemblyPresentation_NormalizesAcceptedRootBeforeComposition)
{
    ecs::Transform local;
    local.position = { 2.0, 1.0, -3.0 };
    std::vector<scene::PreparedAssemblyModule> modules{ Module(0, local) };
    std::vector<scene::PreparedAssemblyMovingPart> noParts;
    std::vector<ecs::Transform> noMoving;
    std::vector<ecs::Transform> moduleWorld(1);
    std::vector<ecs::Transform> noMovingWorld;

    ecs::Transform root;
    const core::Quatf unit =
        core::Quatf::FromAxisAngle({ 0.0f, 1.0f, 0.0f }, 0.65f);
    constexpr float acceptedLengthBias = 1.0002f;
    root.rotation = {
        unit.x * acceptedLengthBias,
        unit.y * acceptedLengthBias,
        unit.z * acceptedLengthBias,
        unit.w * acceptedLengthBias
    };

    CHECK(scene::StageAssemblyPresentation(
        root,
        modules,
        noParts,
        noMoving,
        moduleWorld,
        noMovingWorld).Succeeded());
    const core::Vec3f expected = unit.Rotate(local.position.ToFloat());
    CheckPosition(moduleWorld[0].position, core::Vec3d::FromFloat(expected));
    CheckRotationDirection(moduleWorld[0].rotation, unit);
}

TEST_CASE(AssemblyPresentation_RejectsMalformedRootAndTopology)
{
    std::vector<scene::PreparedAssemblyModule> modules{
        Module(0, ecs::Transform{})
    };
    std::vector<scene::PreparedAssemblyMovingPart> parts{
        MovingPart(0, 0, ecs::Transform{})
    };
    std::vector<ecs::Transform> movingLocals{ ecs::Transform{} };
    std::vector<ecs::Transform> moduleWorld(1);
    std::vector<ecs::Transform> movingWorld(1);

    ecs::Transform root;
    root.position.x = (std::numeric_limits<double>::quiet_NaN)();
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidRoot);
    root = {};
    root.scale.x = 0.0f;
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidRoot);
    root = {};
    root.scale = { 1.0f, 1.01f, 1.0f };
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidRoot);
    root = {};
    root.rotation.w = 2.0f;
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidRoot);
    root = {};
    root.scale = { 1.0e-8f, 1.0e-8f, 1.0e-8f };
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidRoot);

    root = {};
    modules[0].stableIndex = 1;
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld).status,
        scene::AssemblyPresentationStatus::InvalidTopology);
    modules[0].stableIndex = 0;
    parts[0].moduleIndex = 1;
    const scene::AssemblyPresentationResult wrongOwner =
        scene::StageAssemblyPresentation(
            root, modules, parts, movingLocals, moduleWorld, movingWorld);
    CHECK_EQ(wrongOwner.status, scene::AssemblyPresentationStatus::InvalidTopology);
    CHECK(wrongOwner.failedMovingPart);
}

TEST_CASE(AssemblyPresentation_RejectsInvalidLocalAndOverflowWithoutRootNarrowing)
{
    ecs::Transform local;
    std::vector<scene::PreparedAssemblyModule> modules{ Module(0, local) };
    std::vector<scene::PreparedAssemblyMovingPart> noParts;
    std::vector<ecs::Transform> noMoving;
    std::vector<ecs::Transform> moduleWorld(1);
    std::vector<ecs::Transform> noMovingWorld;
    ecs::Transform root;

    modules[0].localTransform.position.x = 1.0e8;
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, noParts, noMoving, moduleWorld, noMovingWorld).status,
        scene::AssemblyPresentationStatus::InvalidLocalTransform);

    modules[0].localTransform = {};
    modules[0].localTransform.rotation.w = 0.5f;
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root, modules, noParts, noMoving, moduleWorld, noMovingWorld).status,
        scene::AssemblyPresentationStatus::InvalidLocalTransform);

    modules[0].localTransform = {};
    modules[0].localTransform.position.x = 2.0;
    root.scale = {
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)(),
        (std::numeric_limits<float>::max)()
    };
    scene::AssemblyPresentationConfig config;
    config.maximumRootScale = (std::numeric_limits<float>::max)();
    CHECK_EQ(
        scene::StageAssemblyPresentation(
            root,
            modules,
            noParts,
            noMoving,
            moduleWorld,
            noMovingWorld,
            config).status,
        scene::AssemblyPresentationStatus::ArithmeticOverflow);
}

TEST_CASE(AssemblyPresentation_GenericPublicationMayOptIntoNonuniformRoot)
{
    std::vector<scene::PreparedAssemblyModule> modules{
        Module(0, ecs::Transform{})
    };
    std::vector<scene::PreparedAssemblyMovingPart> noParts;
    std::vector<ecs::Transform> noMoving;
    std::vector<ecs::Transform> moduleWorld(1);
    std::vector<ecs::Transform> noMovingWorld;
    ecs::Transform root;
    root.scale = { 2.0f, 3.0f, 4.0f };
    scene::AssemblyPresentationConfig config;
    config.requireUniformRootScale = false;

    CHECK(scene::StageAssemblyPresentation(
        root,
        modules,
        noParts,
        noMoving,
        moduleWorld,
        noMovingWorld,
        config).Succeeded());
    CHECK_APPROX_EPS(moduleWorld[0].scale.x, 2.0f, 1.0e-6f);
    CHECK_APPROX_EPS(moduleWorld[0].scale.y, 3.0f, 1.0e-6f);
    CHECK_APPROX_EPS(moduleWorld[0].scale.z, 4.0f, 1.0e-6f);
    CHECK_EQ(
        std::string(scene::AssemblyPresentationStatusName(
            static_cast<scene::AssemblyPresentationStatus>(255))),
        "unknown");
}
