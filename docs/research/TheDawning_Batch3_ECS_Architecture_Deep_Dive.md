# THE DAWNING — ECS Architecture Deep Dive
# Batch 3, Topic 1: Entity Component System, Data-Oriented Design,
#                    Archetype Storage, System Scheduling, Parallelism
# Sources: Flecs/EnTT, Sander Mertens ECS FAQ, Unity DOTS, Mike Acton DOD

---

## PART 1: ECS STORAGE STRATEGIES

### Our Choice: Sparse Set ECS (EnTT-style)

For The Dawning, sparse set storage is optimal because:
- Ships frequently gain/lose components (damaged, upgraded, docked)
- Fast add/remove O(1) is more important than maximal iteration speed
- Simpler implementation than archetypes for a custom engine

```cpp
// Sparse set: maps entity ID → dense array index
template<typename T>
class ComponentPool {
    std::vector<T> dense;           // Contiguous component data (cache-friendly iteration)
    std::vector<uint32_t> denseToEntity;  // dense[i] belongs to entity denseToEntity[i]
    std::vector<uint32_t> sparse;   // sparse[entityId] = index into dense (-1 if absent)
    
public:
    T& Get(uint32_t entityId) { return dense[sparse[entityId]]; }
    bool Has(uint32_t entityId) const { return sparse[entityId] != UINT32_MAX; }
    
    void Add(uint32_t entityId, const T& component) {
        sparse[entityId] = (uint32_t)dense.size();
        dense.push_back(component);
        denseToEntity.push_back(entityId);
    }
    
    void Remove(uint32_t entityId) {
        // Swap-and-pop: move last element into the removed slot
        uint32_t idx = sparse[entityId];
        uint32_t lastEntity = denseToEntity.back();
        
        dense[idx] = dense.back();
        denseToEntity[idx] = lastEntity;
        sparse[lastEntity] = idx;
        
        dense.pop_back();
        denseToEntity.pop_back();
        sparse[entityId] = UINT32_MAX;
    }
    
    // Iteration: just walk dense[] — perfectly contiguous!
    size_t Size() const { return dense.size(); }
    T* Data() { return dense.data(); }
    uint32_t EntityAt(size_t i) const { return denseToEntity[i]; }
};
```

### Entity ID with Generation

```cpp
// Entity ID = index + generation counter
// When entity is destroyed, increment generation
// If stale ID is used, generation mismatch = detected as invalid
struct Entity {
    uint32_t index;       // Index into entity array
    uint32_t generation;  // Incremented on destroy, prevents stale references
    
    bool operator==(const Entity& o) const { 
        return index == o.index && generation == o.generation; 
    }
};

class EntityManager {
    std::vector<uint32_t> generations;  // generation[index]
    std::queue<uint32_t> freeList;      // Recycled indices
    uint32_t nextIndex = 0;
    
public:
    Entity Create() {
        uint32_t idx;
        if (!freeList.empty()) {
            idx = freeList.front(); freeList.pop();
        } else {
            idx = nextIndex++;
            generations.push_back(0);
        }
        return {idx, generations[idx]};
    }
    
    void Destroy(Entity e) {
        generations[e.index]++;  // Invalidate all existing references
        freeList.push(e.index); // Recycle the index
    }
    
    bool IsAlive(Entity e) const {
        return e.index < generations.size() && generations[e.index] == e.generation;
    }
};
```

---

## PART 2: COMPONENT DESIGN RULES

### Keep Components Small and Focused

```
GOOD: Each component = one concern, minimal data
  struct Position { Vec3d pos; };                    // 24 bytes
  struct Velocity { Vec3f vel; };                    // 12 bytes
  struct Health { float current; float max; };       // 8 bytes
  struct ShipClass { uint8_t classId; };             // 1 byte
  struct Faction { uint8_t factionId; };             // 1 byte

BAD: Kitchen-sink components
  struct Ship {  // 500+ bytes — terrible cache utilization!
      Vec3d position; Vec3f velocity; Quatd orientation;
      float health, maxHealth, shield, maxShield;
      std::string name; std::vector<Weapon> weapons;  // heap allocations!
      float fuel, maxFuel; int factionId; ...
  };
```

### The Dawning's Component Catalog

| Component | Size | Description | Entity Types |
|---|---|---|---|
| **Position** | 24B | Vec3d world position | All spatial entities |
| **Velocity** | 12B | Vec3f linear velocity | Moving objects |
| **Orientation** | 32B | Quatd rotation | Oriented objects |
| **AngularVelocity** | 12B | Vec3f rotation rate | Spinning objects |
| **RigidBody** | 48B | Mass, inertia, drag | Physics objects |
| **Health** | 8B | Current + max | Damageable entities |
| **Shield** | 16B | Current, max, regen, delay | Shielded ships |
| **ShipClass** | 4B | Class enum + tier | Ships only |
| **Faction** | 4B | Faction ID + reputation | NPCs, stations |
| **AIController** | 8B | Behavior state + target | NPC ships |
| **PlayerControlled** | 0B | Tag component (no data) | Player entity |
| **Renderable** | 16B | Mesh ID, material ID, LOD | Visible objects |
| **Collider** | 20B | Shape type + dimensions | Physics objects |
| **Inventory** | 8B | Ptr to inventory data | Ships, characters |
| **Thruster** | 32B | Max force, position, dir | Ship thrusters |
| **Weapon** | 24B | Type, damage, range, ammo | Weapon mounts |
| **OrbitalBody** | 48B | Orbital elements | Planets, moons |
| **Atmosphere** | 32B | Composition, pressure, temp | Planets |
| **Selected** | 0B | Tag: currently selected | UI target |
| **MarkedForDestroy** | 0B | Tag: destroy next frame | Cleanup |

### Tag Components (Zero Size)
Components with no data act as flags. They participate in queries
but consume no memory. Example: `PlayerControlled`, `Selected`, `MarkedForDestroy`.

---

## PART 3: SYSTEM DESIGN

### System = Function that Iterates Matching Entities

```cpp
// A system queries for entities with specific components and processes them
using SystemFunc = std::function<void(float dt)>;

// Example: Movement system
void MovementSystem(Registry& reg, float dt) {
    // Query all entities that have BOTH Position AND Velocity
    auto view = reg.View<Position, Velocity>();
    for (auto [entity, pos, vel] : view) {
        pos.pos += Vec3d(vel.vel.x, vel.vel.y, vel.vel.z) * dt;
    }
}

// Example: Gravity system
void GravitySystem(Registry& reg, float dt) {
    auto view = reg.View<Velocity, RigidBody>();
    for (auto [entity, vel, rb] : view) {
        if (rb.affectedByGravity) {
            vel.vel.y -= 9.81f * dt;  // Simple downward gravity
        }
    }
}

// Example: Damage system (reactive — runs on events)
void DamageSystem(Registry& reg, const DamageEvent& event) {
    if (!reg.Has<Health>(event.target)) return;
    auto& health = reg.Get<Health>(event.target);
    
    // Check shield first
    if (reg.Has<Shield>(event.target)) {
        auto& shield = reg.Get<Shield>(event.target);
        if (shield.current > 0) {
            float absorbed = std::min(shield.current, event.damage);
            shield.current -= absorbed;
            event.damage -= absorbed;
        }
    }
    
    health.current -= event.damage;
    if (health.current <= 0) {
        reg.Add<MarkedForDestroy>(event.target);
    }
}
```

### System Execution Order

```
The Dawning tick order (43 systems from v1, expanded):

1. INPUT PHASE (read hardware state)
   - InputSystem
   - UIInputSystem

2. NETWORK PHASE (receive remote data)
   - NetworkReceiveSystem
   - ReconciliationSystem

3. AI PHASE (NPC decisions)
   - AIPerceptionSystem
   - AIDecisionSystem
   - AINavigationSystem
   - FactionStrategySystem
   - GossipPropagationSystem

4. GAMEPLAY PHASE (game rules)
   - PlayerControlSystem
   - IFCSSystem (ship flight computer)
   - WeaponSystem (fire weapons)
   - MissileGuidanceSystem
   - DamageSystem
   - ShieldRegenSystem
   - ComponentDegradeSystem
   - CraftingSystem
   - TradeSystem
   - QuestSystem

5. PHYSICS PHASE (simulate world, FIXED TIMESTEP)
   - ThrusterForceSystem
   - GravitySystem
   - DragSystem
   - CollisionBroadPhase
   - CollisionNarrowPhase
   - ConstraintSolver
   - IntegrationSystem (update positions)
   - OrbitalMechanicsSystem

6. WORLD PHASE (update world state)
   - SectorStreamingSystem
   - LODSystem
   - DestructionSystem
   - FireSystem
   - ParticleSystem
   - WeatherSystem

7. ANIMATION PHASE
   - SkeletonAnimSystem
   - IKSystem
   - ProceduralAnimSystem

8. AUDIO PHASE
   - SpatialAudioSystem
   - MusicSystem
   - AmbienceSystem

9. RENDER PHASE
   - FrustumCullSystem
   - RenderSubmitSystem
   - UIRenderSystem

10. CLEANUP PHASE
    - DestroyMarkedSystem
    - NetworkSendSystem
    - FrameEndSystem
```

### Parallel System Scheduling

```
Systems that don't share writable components can run in parallel:

Thread 1: AIPerceptionSystem (reads Position, writes AIController)
Thread 2: ShieldRegenSystem (reads/writes Shield)
Thread 3: ParticleSystem (reads Position, writes ParticleEmitter)
Thread 4: MusicSystem (reads PlayerControlled, writes MusicState)

These CANNOT run in parallel (both write Position):
  MovementSystem + CollisionResolution

Dependency graph determines execution order:
  - Build DAG from component read/write sets
  - Topological sort
  - Systems at same depth level = parallelizable
```

---

## PART 4: PERFORMANCE CHARACTERISTICS

### Cache Line Utilization

```
CPU cache line: 64 bytes (x86-64)
Position component: 24 bytes → 2.67 positions per cache line
Velocity component: 12 bytes → 5.33 velocities per cache line
Health component: 8 bytes → 8 healths per cache line

For 10,000 entities in MovementSystem:
  AoS (all in one struct): 10,000 × 200B = 2MB, but only using 36B → 82% waste
  SoA (sparse set): 10,000 × 24B + 10,000 × 12B = 360KB → 0% waste
  Cache misses: AoS ≈ 31,250 | SoA ≈ 5,625 → 5.5× fewer misses
```

### Entity Capacity Targets

| Category | Expected Count | Active | Sleeping/LOD |
|---|---|---|---|
| Player | 1 | 1 | 0 |
| Player ship | 1 | 1 | 0 |
| NPC ships (nearby) | 50-200 | 50-200 | 0 |
| NPC ships (sector) | 500-2000 | 0 | On-rails |
| Stations | 5-20 per system | 1-3 | Simplified |
| Planets/moons | 5-15 per system | 1-3 | On-rails |
| Projectiles | 0-500 | All | N/A |
| Debris | 0-500 | All | Auto-despawn |
| NPCs on-foot | 0-100 | 0-100 | 0 |
| **Total active** | **~1000-3000** | | |

---

## PART 5: CURRICULUM STEP UPDATES

**Step 81-85 (ECS foundation)**: Entity with index+generation. EntityManager with free list recycling. ComponentPool<T> with sparse set storage.

**Step 86-90 (Registry)**: Registry class holding all ComponentPools. Add/Remove/Get/Has methods. View<T1,T2,...> for multi-component iteration.

**Step 91-95 (Core components)**: Position, Velocity, Orientation, RigidBody, Health, Renderable. Each as separate small struct.

**Step 96-100 (Systems)**: System as function taking Registry + dt. MovementSystem, GravitySystem as first examples. Fixed execution order array.

**Step 101-110 (Queries)**: Multi-component view with smallest-set iteration. Optional component exclusion filters. Tag components for zero-cost flags.

**Step 111-120 (Events)**: Event queue for deferred operations (spawn, destroy, damage). Systems emit events, other systems consume. Prevents iterator invalidation.

**Step 1001-1010 (ECS depth)**: Parallel system scheduling with dependency graph. SIMD-optimized iteration for Position/Velocity (process 4 at a time with SSE). Memory pool allocators for component arrays (no realloc fragmentation).
