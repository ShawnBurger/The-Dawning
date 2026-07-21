#include "test_framework.h"
#include "scene/assembly_runtime_resources.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

asset::Sha256Digest Hash(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return asset::ComputeSha256(bytes);
}

asset::AssemblyCatalogIdentity Identity(
    asset::AssemblyResourceKind kind,
    uint64_t value,
    const asset::Sha256Digest& digest)
{
    asset::AssemblyCatalogIdentity identity;
    identity.kind = kind;
    identity.value = value;
    identity.generation = 1;
    identity.contentSha256 = digest;
    return identity;
}

template <asset::AssemblyResourceKind Kind>
asset::TypedAssemblyResourceIdentity<Kind> Typed(
    const asset::AssemblyCatalogIdentity& identity)
{
    asset::TypedAssemblyResourceIdentity<Kind> typed;
    typed.value = identity.value;
    typed.generation = identity.generation;
    typed.contentSha256 = identity.contentSha256;
    return typed;
}

} // namespace

TEST_CASE(AssemblyRuntimeOwners_SealExactTypedBindingsForAdapterUse)
{
    scene::AssemblyRuntimeResourceOwners owners;
    const asset::Sha256Digest visualDigest = Hash("visual");
    const asset::Sha256Digest collisionDigest = Hash("collision");
    const asset::Sha256Digest navigationDigest = Hash("navigation");
    const asset::Sha256Digest walkableDigest = Hash("walkable");

    scene::AssemblyRuntimeOwnerRegistration visual;
    visual.kind = asset::AssemblyResourceKind::Visual;
    visual.locator = "visual://hull";
    visual.contentSha256 = visualDigest;
    visual.visual.mesh = scene::MeshHandle(7, 2);
    visual.visual.material.roughness = 0.25f;
    const auto visualAdded = owners.Register(visual);
    CHECK(visualAdded.Succeeded());

    const auto addContract = [&](asset::AssemblyResourceKind kind,
                                 const char* locator,
                                 const asset::Sha256Digest& digest) {
        scene::AssemblyRuntimeOwnerRegistration registration;
        registration.kind = kind;
        registration.locator = locator;
        registration.contentSha256 = digest;
        return owners.Register(registration);
    };
    const auto collisionAdded = addContract(
        asset::AssemblyResourceKind::Collision, "collision://hull", collisionDigest);
    const auto navigationAdded = addContract(
        asset::AssemblyResourceKind::NavigationMesh, "nav://cockpit", navigationDigest);
    const auto walkableAdded = addContract(
        asset::AssemblyResourceKind::WalkableSurface, "walk://cockpit", walkableDigest);
    CHECK(collisionAdded.Succeeded());
    CHECK(navigationAdded.Succeeded());
    CHECK(walkableAdded.Succeeded());

    const auto visualIdentity = Identity(
        asset::AssemblyResourceKind::Visual, 101, visualDigest);
    const auto collisionIdentity = Identity(
        asset::AssemblyResourceKind::Collision, 102, collisionDigest);
    const auto navigationIdentity = Identity(
        asset::AssemblyResourceKind::NavigationMesh, 103, navigationDigest);
    const auto walkableIdentity = Identity(
        asset::AssemblyResourceKind::WalkableSurface, 104, walkableDigest);
    CHECK_EQ(owners.BindCatalogIdentity(visualAdded.ownerToken, visualIdentity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.BindCatalogIdentity(collisionAdded.ownerToken, collisionIdentity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.BindCatalogIdentity(navigationAdded.ownerToken, navigationIdentity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.BindCatalogIdentity(walkableAdded.ownerToken, walkableIdentity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.Seal(), scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK(owners.IsSealed());

    scene::PreparedAssemblyVisual prepared;
    CHECK_EQ(owners.PrepareVisual(
                 Typed<asset::AssemblyResourceKind::Visual>(visualIdentity),
                 visualAdded.ownerToken, prepared),
             scene::AssemblyRuntimeAdapterStatus::Success);
    CHECK_EQ(prepared.mesh, visual.visual.mesh);
    CHECK_APPROX(prepared.material.roughness, 0.25f);
    CHECK_EQ(owners.ValidateCollision(
                 Typed<asset::AssemblyResourceKind::Collision>(collisionIdentity),
                 collisionAdded.ownerToken),
             scene::AssemblyRuntimeAdapterStatus::Success);
    CHECK_EQ(owners.ValidateNavigationMesh(
                 Typed<asset::AssemblyResourceKind::NavigationMesh>(navigationIdentity),
                 navigationAdded.ownerToken),
             scene::AssemblyRuntimeAdapterStatus::Success);
    CHECK_EQ(owners.ValidateWalkableSurface(
                 Typed<asset::AssemblyResourceKind::WalkableSurface>(walkableIdentity),
                 walkableAdded.ownerToken),
             scene::AssemblyRuntimeAdapterStatus::Success);
}

TEST_CASE(AssemblyRuntimeOwners_RejectMutationDuplicatesAndUnboundSeal)
{
    scene::AssemblyRuntimeResourceOwners owners;
    scene::AssemblyRuntimeOwnerRegistration invalid;
    invalid.kind = asset::AssemblyResourceKind::Visual;
    invalid.locator = "visual://invalid";
    invalid.contentSha256 = Hash("invalid");
    CHECK_EQ(owners.Register(invalid).status,
             scene::AssemblyRuntimeOwnerStatus::InvalidArgument);

    scene::AssemblyRuntimeOwnerRegistration visual = invalid;
    visual.visual.mesh = scene::MeshHandle(1, 0);
    const auto added = owners.Register(visual);
    CHECK(added.Succeeded());
    CHECK_EQ(owners.Register(visual).status,
             scene::AssemblyRuntimeOwnerStatus::Duplicate);
    CHECK_EQ(owners.Seal(), scene::AssemblyRuntimeOwnerStatus::Conflict);

    const auto identity = Identity(
        asset::AssemblyResourceKind::Visual, 5, visual.contentSha256);
    CHECK_EQ(owners.BindCatalogIdentity(added.ownerToken, identity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.Seal(), scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.Register(visual).status,
             scene::AssemblyRuntimeOwnerStatus::Sealed);
    CHECK_EQ(owners.BindCatalogIdentity(added.ownerToken, identity),
             scene::AssemblyRuntimeOwnerStatus::Sealed);
}

TEST_CASE(AssemblyRuntimeOwners_FailClosedForWrongTokenKindAndGeneration)
{
    scene::AssemblyRuntimeResourceOwners owners;
    scene::AssemblyRuntimeOwnerRegistration visual;
    visual.kind = asset::AssemblyResourceKind::Visual;
    visual.locator = "visual://hull";
    visual.contentSha256 = Hash("visual");
    visual.visual.mesh = scene::MeshHandle(3, 1);
    const auto added = owners.Register(visual);
    const auto identity = Identity(
        asset::AssemblyResourceKind::Visual, 77, visual.contentSha256);
    CHECK_EQ(owners.BindCatalogIdentity(added.ownerToken, identity),
             scene::AssemblyRuntimeOwnerStatus::Success);
    CHECK_EQ(owners.Seal(), scene::AssemblyRuntimeOwnerStatus::Success);

    scene::PreparedAssemblyVisual prepared;
    const asset::AssemblyVisualIdentity exact =
        Typed<asset::AssemblyResourceKind::Visual>(identity);
    CHECK_EQ(owners.PrepareVisual(exact, 0, prepared),
             scene::AssemblyRuntimeAdapterStatus::NotFound);
    CHECK_EQ(owners.PrepareVisual(exact, 999, prepared),
             scene::AssemblyRuntimeAdapterStatus::NotFound);

    asset::AssemblyVisualIdentity stale = exact;
    ++stale.generation;
    CHECK_EQ(owners.PrepareVisual(stale, added.ownerToken, prepared),
             scene::AssemblyRuntimeAdapterStatus::Stale);

    asset::AssemblyCollisionIdentity wrongKind;
    wrongKind.value = exact.value;
    wrongKind.generation = exact.generation;
    wrongKind.contentSha256 = exact.contentSha256;
    CHECK_EQ(owners.ValidateCollision(wrongKind, added.ownerToken),
             scene::AssemblyRuntimeAdapterStatus::Unsupported);

    scene::AssemblyRuntimeResourceOwners unsealed;
    CHECK_EQ(unsealed.PrepareVisual(exact, added.ownerToken, prepared),
             scene::AssemblyRuntimeAdapterStatus::InternalError);
}

TEST_CASE(AssemblyRuntimeOwners_StatusNamesAreStable)
{
    CHECK_EQ(std::string(scene::AssemblyRuntimeOwnerStatusName(
                 scene::AssemblyRuntimeOwnerStatus::Success)),
             std::string("success"));
    CHECK_EQ(std::string(scene::AssemblyRuntimeOwnerStatusName(
                 static_cast<scene::AssemblyRuntimeOwnerStatus>(255))),
             std::string("unknown"));
}
