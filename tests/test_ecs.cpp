// =============================================================================
// tests/test_ecs.cpp — ecs/entity.h, ecs/component_pool.h, ecs/registry.h
// =============================================================================
// The sparse set and the generational slot map are the two data structures the
// whole scene layer stands on, and neither has ever been exercised at runtime
// on its removal path (Scene::DestroyEntity has zero callers — see
// docs/ANALYSIS.md section 6). Everything here is pure CPU; no D3D12.
// =============================================================================

#include "test_framework.h"
#include "ecs/entity.h"
#include "ecs/component_pool.h"
#include "ecs/components.h"
#include "ecs/registry.h"
#include "ecs/systems.h"

#include <limits>
#include <vector>

namespace
{

// Small POD used as a component so failures print something meaningful.
struct Payload
{
    int   id    = -1;
    float value = 0.0f;
};

} // namespace

// =============================================================================
// Entity — bit packing
// =============================================================================

TEST_CASE(Entity_PacksIndexAndGeneration)
{
    const ecs::Entity e(12345u, 7u);

    CHECK_EQ(e.Index(), 12345u);
    CHECK_EQ(e.Generation(), 7u);
    CHECK(e.IsValid());
    CHECK_FALSE(e.IsNull());

    // Boundary values must survive the round trip.
    const ecs::Entity maxE(ecs::Entity::kMaxIndex, ecs::Entity::kMaxGen);
    CHECK_EQ(maxE.Index(), ecs::Entity::kMaxIndex);
    CHECK_EQ(maxE.Generation(), ecs::Entity::kMaxGen);

    // Default-constructed entity is the null handle.
    const ecs::Entity nullE;
    CHECK(nullE.IsNull());
    CHECK_FALSE(nullE.IsValid());
    CHECK(nullE == ecs::NullEntity);

    // Same index, different generation must be different handles.
    CHECK(ecs::Entity(5u, 0u) != ecs::Entity(5u, 1u));
    CHECK(ecs::Entity(5u, 0u) == ecs::Entity(5u, 0u));
}

// =============================================================================
// EntityManager — create / destroy / recycle
// =============================================================================

TEST_CASE(EntityManager_CreateAssignsSequentialIndices)
{
    ecs::EntityManager em;
    CHECK_EQ(em.AliveCount(), 0u);
    CHECK_EQ(em.SlotCount(), 0u);

    const ecs::Entity a = em.Create();
    const ecs::Entity b = em.Create();
    const ecs::Entity c = em.Create();

    CHECK_EQ(a.Index(), 0u);
    CHECK_EQ(b.Index(), 1u);
    CHECK_EQ(c.Index(), 2u);
    CHECK_EQ(a.Generation(), 0u);
    CHECK_EQ(em.AliveCount(), 3u);
    CHECK_EQ(em.SlotCount(), 3u);

    CHECK(em.IsAlive(a));
    CHECK(em.IsAlive(b));
    CHECK(em.IsAlive(c));
}

TEST_CASE(EntityManager_DestroyRecyclesSlotAndBumpsGeneration)
{
    ecs::EntityManager em;
    const ecs::Entity a = em.Create();
    const ecs::Entity b = em.Create();

    em.Destroy(a);
    CHECK_FALSE(em.IsAlive(a));
    CHECK(em.IsAlive(b));
    CHECK_EQ(em.AliveCount(), 1u);
    CHECK_EQ(em.SlotCount(), 2u);   // slot is freed, not removed

    // The next Create must reuse slot 0 with a bumped generation.
    const ecs::Entity recycled = em.Create();
    CHECK_EQ(recycled.Index(), 0u);
    CHECK_EQ(recycled.Generation(), 1u);
    CHECK_EQ(em.AliveCount(), 2u);
    CHECK_EQ(em.SlotCount(), 2u);   // no new slot allocated

    // This is the entire point of the generational handle: the old handle
    // must not resurrect just because its slot was reused.
    CHECK(a != recycled);
    CHECK_FALSE(em.IsAlive(a));
    CHECK(em.IsAlive(recycled));
}

TEST_CASE(EntityManager_DestroyIsIdempotentAndBoundsChecked)
{
    ecs::EntityManager em;
    const ecs::Entity a = em.Create();

    em.Destroy(a);
    CHECK_EQ(em.AliveCount(), 0u);

    // Double destroy must not underflow the alive count.
    em.Destroy(a);
    CHECK_EQ(em.AliveCount(), 0u);

    // Out-of-range and null handles must be ignored, not indexed.
    em.Destroy(ecs::Entity(99999u, 0u));
    em.Destroy(ecs::NullEntity);
    CHECK_EQ(em.AliveCount(), 0u);

    CHECK_FALSE(em.IsAlive(ecs::Entity(99999u, 0u)));
    CHECK_FALSE(em.IsAlive(ecs::NullEntity));
}

TEST_CASE(EntityManager_FreeListIsLifo)
{
    ecs::EntityManager em;
    const ecs::Entity a = em.Create();   // index 0
    const ecs::Entity b = em.Create();   // index 1
    const ecs::Entity c = em.Create();   // index 2

    em.Destroy(a);
    em.Destroy(b);
    em.Destroy(c);
    CHECK_EQ(em.AliveCount(), 0u);

    // Free list is threaded through Slot::nextFree, so it pops in reverse.
    CHECK_EQ(em.Create().Index(), 2u);
    CHECK_EQ(em.Create().Index(), 1u);
    CHECK_EQ(em.Create().Index(), 0u);

    // Exhausted free list falls back to a fresh slot.
    CHECK_EQ(em.Create().Index(), 3u);
    CHECK_EQ(em.AliveCount(), 4u);
}

// -----------------------------------------------------------------------------
// REGRESSION GUARD — defect L4, docs/ANALYSIS.md section 4 (LOW)
// -----------------------------------------------------------------------------
// EntityManager::Grow() used to be called from Create() AFTER `m_slotCount++`,
// so when it ran m_slotCount == index + 1 while the OLD array still held only
// m_capacity == index elements. Its copy loop therefore read m_slots[m_capacity]
// — one full 12-byte Slot past the allocation. Undefined behaviour, and an
// instant Application Verifier / PageHeap / ASan hit.
//
// This was fixed in src/ecs/entity.h: Grow() is now hoisted above the
// increment. This test is the regression guard.
//
// The first growth threshold is 256 and main.cpp creates ~12 entities, so the
// overread never fired in the shipping app and nothing in the repo had ever
// crossed the boundary. Creating 300+ entities crosses it on every run.
//
// Note the overread was silent in an ordinary MSVC Debug build — the garbage
// Slot copied into newSlots[256] was immediately overwritten by the caller — so
// what this test asserts is the observable contract: no invariant is damaged
// across the reallocation. Run the suite under PageHeap to catch the raw UB.
// -----------------------------------------------------------------------------
TEST_CASE(EntityManager_ThreeHundredEntitiesCrossesGrowthBoundary_L4)
{
    ecs::EntityManager em;

    constexpr uint32_t kCount = 300;   // first Grow() threshold is 256
    std::vector<ecs::Entity> entities;
    entities.reserve(kCount);

    for (uint32_t i = 0; i < kCount; i++)
        entities.push_back(em.Create());

    CHECK_EQ(em.AliveCount(), kCount);
    CHECK_EQ(em.SlotCount(), kCount);

    // Every handle must be alive, uniquely indexed, and generation 0.
    bool allIndicesSequential = true;
    bool allGenerationsZero   = true;
    bool allAlive             = true;
    for (uint32_t i = 0; i < kCount; i++)
    {
        if (entities[i].Index() != i)        allIndicesSequential = false;
        if (entities[i].Generation() != 0u)  allGenerationsZero = false;
        if (!em.IsAlive(entities[i]))        allAlive = false;
    }
    CHECK(allIndicesSequential);
    CHECK(allGenerationsZero);
    CHECK(allAlive);

    // The slots straddling the 256-element reallocation are the ones the
    // overread touches. Assert they survived intact.
    CHECK(em.IsAlive(entities[254]));
    CHECK(em.IsAlive(entities[255]));
    CHECK(em.IsAlive(entities[256]));
    CHECK(em.IsAlive(entities[257]));
    CHECK_EQ(entities[256].Index(), 256u);
    CHECK_EQ(entities[256].Generation(), 0u);
}

TEST_CASE(EntityManager_RecycleAcrossGrowthBoundary)
{
    ecs::EntityManager em;

    std::vector<ecs::Entity> entities;
    for (uint32_t i = 0; i < 300u; i++)
        entities.push_back(em.Create());

    // Destroy three entities straddling the reallocation boundary.
    em.Destroy(entities[255]);
    em.Destroy(entities[256]);
    em.Destroy(entities[257]);
    CHECK_EQ(em.AliveCount(), 297u);
    CHECK_FALSE(em.IsAlive(entities[255]));
    CHECK_FALSE(em.IsAlive(entities[256]));
    CHECK_FALSE(em.IsAlive(entities[257]));

    // Recycling must return those exact slots with generation 1.
    for (int i = 0; i < 3; i++)
    {
        const ecs::Entity recycled = em.Create();
        CHECK(recycled.Index() >= 255u && recycled.Index() <= 257u);
        CHECK_EQ(recycled.Generation(), 1u);
        CHECK(em.IsAlive(recycled));
    }

    CHECK_EQ(em.AliveCount(), 300u);
    CHECK_EQ(em.SlotCount(), 300u);   // recycled, so no new slots

    // Untouched neighbours must be unaffected.
    CHECK(em.IsAlive(entities[254]));
    CHECK(em.IsAlive(entities[258]));
    CHECK(em.IsAlive(entities[299]));
}

// =============================================================================
// ComponentPool — sparse set
// =============================================================================

TEST_CASE(ComponentPool_AddGetHasCount)
{
    ecs::ComponentPool<Payload> pool;
    CHECK_EQ(pool.Count(), 0u);
    CHECK_FALSE(pool.Has(0u));

    pool.Add(10u, Payload{ 10, 1.5f });
    pool.Add(20u, Payload{ 20, 2.5f });

    CHECK_EQ(pool.Count(), 2u);
    CHECK(pool.Has(10u));
    CHECK(pool.Has(20u));
    CHECK_FALSE(pool.Has(15u));

    CHECK_EQ(pool.Get(10u).id, 10);
    CHECK_EQ(pool.Get(20u).id, 20);
    CHECK_APPROX(pool.Get(20u).value, 2.5f);

    // Adding an entity that already has the component overwrites in place.
    pool.Add(10u, Payload{ 10, 99.0f });
    CHECK_EQ(pool.Count(), 2u);
    CHECK_APPROX(pool.Get(10u).value, 99.0f);
}

// Removal is swap-and-pop: the last dense element is relocated into the hole
// and its sparse entry is fixed up. Asserting EntityAt(hole) is what actually
// proves the relocation happened rather than just checking Has().
TEST_CASE(ComponentPool_RemoveSwapsBackLastElement)
{
    ecs::ComponentPool<Payload> pool;
    pool.Add(10u, Payload{ 10, 1.0f });
    pool.Add(20u, Payload{ 20, 2.0f });
    pool.Add(30u, Payload{ 30, 3.0f });
    CHECK_EQ(pool.Count(), 3u);
    CHECK_EQ(pool.EntityAt(1u), 20u);

    pool.Remove(20u);   // middle element — forces a swap

    CHECK_EQ(pool.Count(), 2u);
    CHECK_FALSE(pool.Has(20u));
    CHECK(pool.Has(10u));
    CHECK(pool.Has(30u));

    // Entity 30 was the last dense element; it must now occupy slot 1.
    CHECK_EQ(pool.EntityAt(1u), 30u);
    CHECK_EQ(pool.DataAt(1u).id, 30);

    // And the sparse entry must have been fixed up so Get still works.
    CHECK_EQ(pool.Get(30u).id, 30);
    CHECK_APPROX(pool.Get(30u).value, 3.0f);
    CHECK_EQ(pool.Get(10u).id, 10);

    // Dense array must have no gaps.
    CHECK_EQ(pool.EntityAt(0u), 10u);
}

TEST_CASE(ComponentPool_RemoveLastElementNeedsNoSwap)
{
    ecs::ComponentPool<Payload> pool;
    pool.Add(10u, Payload{ 10, 1.0f });
    pool.Add(20u, Payload{ 20, 2.0f });

    pool.Remove(20u);   // already the last element — the self-swap guard path

    CHECK_EQ(pool.Count(), 1u);
    CHECK_FALSE(pool.Has(20u));
    CHECK(pool.Has(10u));
    CHECK_EQ(pool.EntityAt(0u), 10u);
    CHECK_EQ(pool.Get(10u).id, 10);
}

TEST_CASE(ComponentPool_RemoveAbsentIsNoOp)
{
    ecs::ComponentPool<Payload> pool;
    pool.Add(10u, Payload{ 10, 1.0f });

    pool.Remove(999u);   // never added
    pool.Remove(11u);    // within sparse capacity, never added
    CHECK_EQ(pool.Count(), 1u);
    CHECK(pool.Has(10u));

    pool.Remove(10u);
    CHECK_EQ(pool.Count(), 0u);

    pool.Remove(10u);    // double remove must not underflow m_count
    CHECK_EQ(pool.Count(), 0u);
    CHECK_FALSE(pool.Has(10u));
}

TEST_CASE(ComponentPool_ReAddAfterRemove)
{
    ecs::ComponentPool<Payload> pool;
    pool.Add(10u, Payload{ 10, 1.0f });
    pool.Add(20u, Payload{ 20, 2.0f });
    pool.Add(30u, Payload{ 30, 3.0f });

    pool.Remove(10u);
    CHECK_FALSE(pool.Has(10u));

    pool.Add(10u, Payload{ 10, 42.0f });
    CHECK(pool.Has(10u));
    CHECK_EQ(pool.Count(), 3u);
    CHECK_APPROX(pool.Get(10u).value, 42.0f);

    // All three must still be individually retrievable and distinct.
    CHECK_EQ(pool.Get(20u).id, 20);
    CHECK_EQ(pool.Get(30u).id, 30);
}

// The two-part invariant at component_pool.h:95 (denseIdx < m_count AND the
// back-reference m_dense[denseIdx] == entityIndex) is what makes stale sparse
// entries harmless. Removing everything then querying must stay false rather
// than reading a stale dense index.
TEST_CASE(ComponentPool_StaleSparseEntriesAreHarmless)
{
    ecs::ComponentPool<Payload> pool;
    for (uint32_t i = 0; i < 8u; i++)
        pool.Add(i, Payload{ static_cast<int>(i), 0.0f });

    for (uint32_t i = 0; i < 8u; i++)
        pool.Remove(i);

    CHECK_EQ(pool.Count(), 0u);
    for (uint32_t i = 0; i < 8u; i++)
        CHECK_FALSE(pool.Has(i));

    // Re-populating must produce a consistent dense array again.
    pool.Add(3u, Payload{ 3, 0.0f });
    CHECK_EQ(pool.Count(), 1u);
    CHECK(pool.Has(3u));
    CHECK_EQ(pool.EntityAt(0u), 3u);
    for (uint32_t i = 0; i < 8u; i++)
        if (i != 3u) CHECK_FALSE(pool.Has(i));
}

// Both EnsureSparse (initial 256, doubling) and EnsureDense (initial 64,
// doubling) must survive a sparse index far past the initial capacity.
TEST_CASE(ComponentPool_GrowsSparseAndDenseArrays)
{
    ecs::ComponentPool<Payload> pool;

    // Force several sparse doublings in one call.
    pool.Add(5000u, Payload{ 5000, 1.0f });
    CHECK(pool.Has(5000u));
    CHECK_EQ(pool.Get(5000u).id, 5000);
    CHECK_FALSE(pool.Has(4999u));
    CHECK_FALSE(pool.Has(0u));

    // Force several dense doublings (initial dense capacity is 64).
    for (uint32_t i = 0; i < 400u; i++)
        pool.Add(i, Payload{ static_cast<int>(i), static_cast<float>(i) });

    CHECK_EQ(pool.Count(), 401u);

    bool allPresent = true;
    bool allCorrect = true;
    for (uint32_t i = 0; i < 400u; i++)
    {
        if (!pool.Has(i))                            allPresent = false;
        if (pool.Get(i).id != static_cast<int>(i))   allCorrect = false;
    }
    CHECK(allPresent);
    CHECK(allCorrect);
    CHECK(pool.Has(5000u));
    CHECK_EQ(pool.Get(5000u).id, 5000);   // survived the reallocations
}

// =============================================================================
// Registry
// =============================================================================

TEST_CASE(Registry_AssignHasGetRemove)
{
    ecs::Registry reg;
    const ecs::Entity e = reg.Create();

    CHECK(reg.IsAlive(e));
    CHECK_EQ(reg.EntityCount(), 1u);
    CHECK_FALSE(reg.Has<ecs::Transform>(e));

    ecs::Transform t;
    t.position = { 1.0f, 2.0f, 3.0f };
    reg.Assign<ecs::Transform>(e, t);

    CHECK(reg.Has<ecs::Transform>(e));
    CHECK_APPROX(reg.Get<ecs::Transform>(e).position.x, 1.0f);
    CHECK_APPROX(reg.Get<ecs::Transform>(e).position.z, 3.0f);

    // Mutating through Get must persist.
    reg.Get<ecs::Transform>(e).position.y = 99.0f;
    CHECK_APPROX(reg.Get<ecs::Transform>(e).position.y, 99.0f);

    reg.Remove<ecs::Transform>(e);
    CHECK_FALSE(reg.Has<ecs::Transform>(e));

    // Has<T> on a type no entity ever had must be false, not a null deref.
    CHECK_FALSE(reg.Has<ecs::Velocity>(e));
}

TEST_CASE(Registry_DestroyStripsAllComponents)
{
    ecs::Registry reg;
    const ecs::Entity a = reg.Create();
    const ecs::Entity b = reg.Create();

    reg.Assign<ecs::Transform>(a);
    reg.Assign<ecs::MeshInstance>(a);
    reg.Assign<ecs::Transform>(b);

    CHECK_EQ(reg.GetPool<ecs::Transform>()->Count(), 2u);
    CHECK_EQ(reg.GetPool<ecs::MeshInstance>()->Count(), 1u);
    CHECK_EQ(reg.EntityCount(), 2u);

    reg.Destroy(a);

    CHECK_FALSE(reg.IsAlive(a));
    CHECK_EQ(reg.EntityCount(), 1u);
    CHECK_EQ(reg.GetPool<ecs::Transform>()->Count(), 1u);
    CHECK_EQ(reg.GetPool<ecs::MeshInstance>()->Count(), 0u);

    // b must be untouched.
    CHECK(reg.IsAlive(b));
    CHECK(reg.Has<ecs::Transform>(b));

    // Destroying an already-dead handle is a no-op.
    reg.Destroy(a);
    CHECK_EQ(reg.EntityCount(), 1u);
}

TEST_CASE(Registry_EachVisitsOnlyEntitiesWithBothComponents)
{
    ecs::Registry reg;

    const ecs::Entity both1 = reg.Create();
    const ecs::Entity onlyT = reg.Create();
    const ecs::Entity both2 = reg.Create();
    const ecs::Entity onlyM = reg.Create();

    reg.Assign<ecs::Transform>(both1);
    reg.Assign<ecs::MeshInstance>(both1);
    reg.Assign<ecs::Transform>(onlyT);
    reg.Assign<ecs::Transform>(both2);
    reg.Assign<ecs::MeshInstance>(both2);
    reg.Assign<ecs::MeshInstance>(onlyM);

    int visited = 0;
    bool sawUnexpected = false;
    reg.Each<ecs::Transform, ecs::MeshInstance>(
        [&](uint32_t idx, ecs::Transform&, ecs::MeshInstance&)
        {
            visited++;
            if (idx != both1.Index() && idx != both2.Index())
                sawUnexpected = true;
        });

    CHECK_EQ(visited, 2);
    CHECK_FALSE(sawUnexpected);

    // A component type that no entity has must make Each a no-op, not a crash.
    int neverVisited = 0;
    reg.Each<ecs::Transform, ecs::Velocity>(
        [&](uint32_t, ecs::Transform&, ecs::Velocity&) { neverVisited++; });
    CHECK_EQ(neverVisited, 0);
}

// =============================================================================
// Velocity integration
// =============================================================================

TEST_CASE(VelocitySystem_PreservesLinearMotionAtPlanetaryCoordinates)
{
    ecs::Transform transform;
    transform.position = { 1.0e10, -2.0e10, 3.0e10 };
    const core::Vec3d start = transform.position;

    ecs::Velocity velocity;
    velocity.linear = { 15.0f, -6.0f, 0.25f };

    ecs::systems::IntegrateVelocity(transform, velocity, 0.5);

    CHECK_APPROX_EPS(transform.position.x - start.x, 7.5, 1e-6);
    CHECK_APPROX_EPS(transform.position.y - start.y, -3.0, 1e-6);
    CHECK_APPROX_EPS(transform.position.z - start.z, 0.125, 1e-6);
}

TEST_CASE(VelocitySystem_IntegratesAxisScaledAngularVelocity)
{
    ecs::Transform transform;
    ecs::Velocity velocity;
    velocity.angular = { 0.0f, core::PI, 0.0f };

    ecs::systems::IntegrateVelocity(transform, velocity, 0.5);

    const core::Vec3f rotated = transform.rotation.Rotate({ 1.0f, 0.0f, 0.0f });
    CHECK_APPROX(rotated.x, 0.0f);
    CHECK_APPROX(rotated.y, 0.0f);
    CHECK_APPROX(rotated.z, -1.0f);
    CHECK_APPROX(transform.rotation.LengthSq(), 1.0f);
}

TEST_CASE(VelocitySystem_AngularVelocityUsesBodyLocalAxes)
{
    ecs::Transform transform;
    transform.rotation = core::Quatf::FromAxisAngle(
        { 0.0f, 1.0f, 0.0f }, core::HALF_PI);

    ecs::Velocity velocity;
    velocity.angular = { core::PI, 0.0f, 0.0f };

    ecs::systems::IntegrateVelocity(transform, velocity, 0.5);

    // Local +X rotation applies before the existing world orientation. Under
    // this engine's convention, local +Y therefore ends at world +X. Reversing
    // the quaternion product would incorrectly leave it on world +Z.
    const core::Vec3f rotated = transform.rotation.Rotate({ 0.0f, 1.0f, 0.0f });
    CHECK_APPROX(rotated.x, 1.0f);
    CHECK_APPROX(rotated.y, 0.0f);
    CHECK_APPROX(rotated.z, 0.0f);
}

TEST_CASE(VelocitySystem_RegistryVisitsOnlyCompleteMotionPairs)
{
    ecs::Registry registry;
    const ecs::Entity moving = registry.Create();
    const ecs::Entity transformOnly = registry.Create();
    const ecs::Entity velocityOnly = registry.Create();

    registry.Assign<ecs::Transform>(moving);
    registry.Assign<ecs::Velocity>(moving, ecs::Velocity{ { 2.0f, 0.0f, 0.0f }, {} });
    registry.Assign<ecs::Transform>(transformOnly);
    registry.Assign<ecs::Velocity>(velocityOnly, ecs::Velocity{ { 20.0f, 0.0f, 0.0f }, {} });

    ecs::systems::IntegrateVelocities(registry, 0.25);

    CHECK_APPROX(registry.Get<ecs::Transform>(moving).position.x, 0.5);
    CHECK_APPROX(registry.Get<ecs::Transform>(transformOnly).position.x, 0.0);
}

TEST_CASE(VelocitySystem_RejectsNonPositiveAndNonFiniteTimesteps)
{
    ecs::Transform transform;
    transform.position = { 4.0, 5.0, 6.0 };
    transform.rotation = core::Quatf::FromAxisAngle({ 1.0f, 0.0f, 0.0f }, 0.25f);

    ecs::Velocity velocity;
    velocity.linear = { 10.0f, 20.0f, 30.0f };
    velocity.angular = { 0.0f, 1.0f, 0.0f };

    const ecs::Transform original = transform;
    ecs::systems::IntegrateVelocity(transform, velocity, 0.0);
    ecs::systems::IntegrateVelocity(transform, velocity, -1.0);
    ecs::systems::IntegrateVelocity(
        transform, velocity, std::numeric_limits<double>::infinity());

    CHECK(transform.position == original.position);
    CHECK_APPROX(transform.rotation.x, original.rotation.x);
    CHECK_APPROX(transform.rotation.y, original.rotation.y);
    CHECK_APPROX(transform.rotation.z, original.rotation.z);
    CHECK_APPROX(transform.rotation.w, original.rotation.w);
}

// -----------------------------------------------------------------------------
// REGRESSION GUARD — defect M6, docs/ANALYSIS.md section 4 (MEDIUM)
// -----------------------------------------------------------------------------
// Registry::Has<T>, Get<T> and Remove<T> used to route straight through
// entity.Index() and never call m_entities.IsAlive(entity). The generation was
// validated in exactly one place — EntityManager::IsAlive (entity.h:94-99) —
// which no component path invoked.
//
// So after a slot was recycled, a STALE handle to the destroyed entity reported
// Has<T>() == true and Get<T>() handed back the NEW entity's component: a
// silent wrong-entity read/write with no diagnostic, negating the entire reason
// the handle is 20 bits of index plus 12 bits of generation.
//
// This was fixed in src/ecs/registry.h (every Entity-keyed operation now
// early-outs on !IsAlive, and Get returns a per-type fallback instead of
// dereferencing null). These checks were originally written as
// CHECK_KNOWN_FAILING and have been promoted to hard assertions now that they
// pass — they exist to keep the fix from regressing.
//
// It stayed latent at runtime only because Scene::DestroyEntity has zero
// callers anywhere in the repo, so the removal path never executed.
// -----------------------------------------------------------------------------
TEST_CASE(Registry_StaleHandleIsRejected_M6)
{
    ecs::Registry reg;

    const ecs::Entity original = reg.Create();
    ecs::Transform originalTransform;
    originalTransform.position = { 111.0f, 0.0f, 0.0f };
    reg.Assign<ecs::Transform>(original, originalTransform);

    reg.Destroy(original);

    // Slot 0 is recycled with generation 1 and given its own Transform.
    const ecs::Entity recycled = reg.Create();
    ecs::Transform recycledTransform;
    recycledTransform.position = { 222.0f, 0.0f, 0.0f };
    reg.Assign<ecs::Transform>(recycled, recycledTransform);

    // Sanity: the two handles really are different, and IsAlive gets it right.
    CHECK(original != recycled);
    CHECK_EQ(original.Index(), recycled.Index());
    CHECK_FALSE(reg.IsAlive(original));
    CHECK(reg.IsAlive(recycled));
    CHECK_APPROX(reg.Get<ecs::Transform>(recycled).position.x, 222.0f);

    // A dead handle must not report ownership of the live entity's component.
    CHECK_FALSE(reg.Has<ecs::Transform>(original));

    // ...and must not hand back the live entity's data.
    CHECK(reg.Get<ecs::Transform>(original).position.x != 222.0f);

    // Removing through a stale handle must not strip the live entity's
    // component — the same generation check on the write path.
    reg.Remove<ecs::Transform>(original);
    CHECK(reg.Has<ecs::Transform>(recycled));
    CHECK_APPROX(reg.Get<ecs::Transform>(recycled).position.x, 222.0f);

    // Assigning through a stale handle must not overwrite the live entity.
    ecs::Transform intruder;
    intruder.position = { 333.0f, 0.0f, 0.0f };
    reg.Assign<ecs::Transform>(original, intruder);
    CHECK_APPROX(reg.Get<ecs::Transform>(recycled).position.x, 222.0f);
}
