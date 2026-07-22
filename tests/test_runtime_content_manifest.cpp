#include "test_framework.h"
#include "asset/runtime_content_manifest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view kValidManifest =
    "tdcontent 2\n"
    "scene \"ship.reference.runtime\"\n"
    "assembly \"reference_ship.tdassembly\"\n"
    "root 10 20 30 0 90 0 1 2 3\n"
    "visual \"source://reference/hull.glb\" \"corridor_section.tdmodel\" 0\n"
    "visual \"visual://hull_lod0\" \"corridor_section.tdmodel\" 0\n"
    "collision \"collision://hull\" \"reference_hull.tdcollision\"\n"
    "navigation \"nav://cockpit\"\n"
    "walkable \"walk://cockpit_floor\"\n"
    "end\n";

bool Rejected(std::string_view text)
{
    return !asset::ParseRuntimeContentManifest(text).Succeeded();
}

} // namespace

TEST_CASE(RuntimeContentManifest_ParsesTypedBindingsAndRootTransform)
{
    const asset::RuntimeContentManifestResult result =
        asset::ParseRuntimeContentManifest(kValidManifest);

    CHECK(result.Succeeded());
    CHECK_EQ(result.manifest.schemaVersion, 2u);
    CHECK_EQ(result.manifest.sceneId, std::string("ship.reference.runtime"));
    CHECK_EQ(result.manifest.cookedAssemblyPath.generic_string(),
             std::string("reference_ship.tdassembly"));
    CHECK_EQ(result.manifest.bindings.size(), 5u);
    CHECK_EQ(result.manifest.bindings[0].kind,
             asset::AssemblyResourceKind::Visual);
    CHECK_EQ(result.manifest.bindings[0].primitiveIndex, 0u);
    CHECK_EQ(result.manifest.bindings[2].kind,
             asset::AssemblyResourceKind::Collision);
    CHECK_EQ(result.manifest.bindings[2].cookedCollisionPath.generic_string(),
             std::string("reference_hull.tdcollision"));
    CHECK_EQ(result.manifest.bindings[3].kind,
             asset::AssemblyResourceKind::NavigationMesh);
    CHECK_EQ(result.manifest.bindings[4].kind,
             asset::AssemblyResourceKind::WalkableSurface);
    CHECK_APPROX(result.manifest.rootTransform.positionMeters[0], 10.0);
    CHECK_APPROX(result.manifest.rootTransform.rotationEulerDegrees[1], 90.0);
    CHECK_APPROX(result.manifest.rootTransform.scale[2], 3.0);
}

TEST_CASE(RuntimeContentManifest_RejectsMalformedUnsafeAndAmbiguousRecords)
{
    CHECK(Rejected(""));
    CHECK(Rejected("tdcontent 1\n"));
    CHECK(Rejected("scene \"missing.header\"\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\n"
        "visual \"visual://a\" \"b.tdmodel\" 0\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"../a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"C:/a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\n"
        "collision \"visual://wrong\" \"a.tdcollision\"\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 0 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene x\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\nvisual \"visual://a\" \"a.tdmodel\" 0\nend\n"));
    CHECK(Rejected(std::string(kValidManifest) + "scene \"after.end\"\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\ncollision \"collision://a\"\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\n"
        "collision \"collision://a\" \"../a.tdcollision\"\nend\n"));
    CHECK(Rejected(
        "tdcontent 2\nscene \"x\"\nassembly \"a.tdassembly\"\n"
        "root 0 0 0 0 0 0 1 1 1\n"
        "navigation \"nav://a\" \"unexpected.bin\"\nend\n"));

    std::string withNul(kValidManifest);
    withNul.insert(withNul.begin() + 4, '\0');
    CHECK(Rejected(withNul));
}

TEST_CASE(RuntimeContentManifest_EnforcesByteLineStringAndBindingLimits)
{
    asset::RuntimeContentManifestLimits limits;
    limits.maxFileBytes = 10;
    CHECK_EQ(asset::ParseRuntimeContentManifest(kValidManifest, limits).status,
             asset::RuntimeContentManifestStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxLineBytes = 12;
    CHECK_EQ(asset::ParseRuntimeContentManifest(kValidManifest, limits).status,
             asset::RuntimeContentManifestStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxStringBytes = 8;
    CHECK(!asset::ParseRuntimeContentManifest(kValidManifest, limits).Succeeded());

    limits = {};
    limits.maxBindings = 4;
    CHECK_EQ(asset::ParseRuntimeContentManifest(kValidManifest, limits).status,
             asset::RuntimeContentManifestStatus::ResourceLimitExceeded);

    limits = {};
    limits.maxBindings = 0;
    CHECK_EQ(asset::ParseRuntimeContentManifest(kValidManifest, limits).status,
             asset::RuntimeContentManifestStatus::InvalidData);
}

TEST_CASE(RuntimeContentManifest_FileLoadSetsConfinedContentRoot)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "the_dawning_runtime_content_manifest_test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    CHECK(!error);

    const std::filesystem::path path = directory / "reference.tdcontent";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(kValidManifest.data(),
                     static_cast<std::streamsize>(kValidManifest.size()));
        CHECK(static_cast<bool>(output));
    }

    const asset::RuntimeContentManifestResult loaded =
        asset::LoadRuntimeContentManifestFile(path);
    CHECK(loaded.Succeeded());
    CHECK_EQ(loaded.manifest.sourcePath,
             std::filesystem::absolute(path).lexically_normal());
    CHECK_EQ(loaded.manifest.contentRoot,
             std::filesystem::absolute(directory).lexically_normal());

    std::filesystem::remove_all(directory, error);
    CHECK(!error);
    CHECK_EQ(asset::LoadRuntimeContentManifestFile(path).status,
             asset::RuntimeContentManifestStatus::FileNotFound);
}

TEST_CASE(RuntimeContentManifest_ContentSelectionBuildsOnlyConfinedNames)
{
    const asset::RuntimeContentSelectionResult courier =
        asset::BuildRuntimeContentManifestPath("frontier_courier_mk1");
    CHECK(courier.accepted);
    CHECK_EQ(courier.contentId, std::string("frontier_courier_mk1"));
    CHECK_EQ(courier.manifestPath.generic_string(),
             std::string("assets/runtime/frontier_courier_mk1.tdcontent"));
    CHECK(asset::BuildRuntimeContentManifestPath(
        std::string(asset::kRuntimeContentSelectorMaxBytes, 'a')).accepted);

    CHECK(!asset::BuildRuntimeContentManifestPath("").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("../reference_ship").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("reference_ship.tdcontent").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("C:reference_ship").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("ship/reference").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("ship\\reference").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath("reference ship").accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath(
        std::string(asset::kRuntimeContentSelectorMaxBytes + 1u, 'a')).accepted);
    CHECK(!asset::BuildRuntimeContentManifestPath(
        "reference_ship", std::filesystem::path{}).accepted);
}

TEST_CASE(RuntimeContentManifest_CoverageRequiresEveryAndOnlyAuthoredLocator)
{
    const asset::RuntimeContentManifestResult parsed =
        asset::ParseRuntimeContentManifest(kValidManifest);
    CHECK(parsed.Succeeded());

    asset::CookedAssembly assembly;
    asset::AssemblyModule module;
    module.visualSource = "source://reference/hull.glb";
    module.collisionSource = "collision://hull";
    module.lods.push_back(asset::AssemblyLod{
        0, "visual://hull_lod0", 80.0 });
    assembly.modules.push_back(module);
    asset::AssemblyZone zone;
    zone.navmeshSource = "nav://cockpit";
    zone.walkableSurface = "walk://cockpit_floor";
    assembly.zones.push_back(zone);

    CHECK(asset::ValidateRuntimeContentCoverage(
              parsed.manifest, assembly).matched);

    asset::RuntimeContentManifest missing = parsed.manifest;
    missing.bindings.pop_back();
    const asset::RuntimeContentCoverageResult missingResult =
        asset::ValidateRuntimeContentCoverage(missing, assembly);
    CHECK(!missingResult.matched);
    CHECK_EQ(missingResult.failedKind,
             asset::AssemblyResourceKind::WalkableSurface);

    asset::RuntimeContentManifest extra = parsed.manifest;
    asset::RuntimeContentBinding extraBinding;
    extraBinding.kind = asset::AssemblyResourceKind::Collision;
    extraBinding.locator = "collision://not-authored";
    extra.bindings.push_back(extraBinding);
    const asset::RuntimeContentCoverageResult extraResult =
        asset::ValidateRuntimeContentCoverage(extra, assembly);
    CHECK(!extraResult.matched);
    CHECK_EQ(extraResult.failedLocator, std::string("collision://not-authored"));

    asset::RuntimeContentManifest duplicate = parsed.manifest;
    duplicate.bindings.push_back(duplicate.bindings.front());
    const asset::RuntimeContentCoverageResult duplicateResult =
        asset::ValidateRuntimeContentCoverage(duplicate, assembly);
    CHECK(!duplicateResult.matched);
    CHECK_EQ(duplicateResult.failedLocator,
             std::string("source://reference/hull.glb"));
}

TEST_CASE(RuntimeContentManifest_StatusNamesAreStable)
{
    CHECK_EQ(std::string(asset::RuntimeContentManifestStatusName(
                 asset::RuntimeContentManifestStatus::Success)),
             std::string("success"));
    CHECK_EQ(std::string(asset::RuntimeContentManifestStatusName(
                 static_cast<asset::RuntimeContentManifestStatus>(255))),
             std::string("unknown"));
}
