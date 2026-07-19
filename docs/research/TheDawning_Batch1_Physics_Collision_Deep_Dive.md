# THE DAWNING — Physics & Collision Deep Dive
# Batch 1, Topic 2: Rigid Body Dynamics, Collision Detection, Integration,
#                    Jolt Physics Reference, Constraint Systems
# Sources: Jolt Physics docs, Bullet Physics, Erin Catto (Box2D) GDC talks,
#          Real-Time Collision Detection (Ericson), Game Physics Engine Development

---

## PART 1: PHYSICS PIPELINE ARCHITECTURE

### Fixed Timestep with Interpolation
Physics MUST run at fixed timestep for determinism and stability.

```cpp
// The accumulator pattern (from Glenn Fiedler / Gaffer on Games)
static const double PHYSICS_DT = 1.0 / 60.0;  // 60 Hz fixed step
static double accumulator = 0.0;

void GameLoop(double frameDt) {
    accumulator += frameDt;
    
    // Run as many fixed steps as needed
    while (accumulator >= PHYSICS_DT) {
        PhysicsStep(PHYSICS_DT);
        accumulator -= PHYSICS_DT;
    }
    
    // Interpolation factor for rendering (0-1)
    double alpha = accumulator / PHYSICS_DT;
    // Render at: lerp(previousState, currentState, alpha)
    // This prevents visual stuttering when render FPS ≠ physics FPS
}
```

### Physics Step Order
```
1. Apply external forces (gravity, thrusters, explosions)
2. Broad-phase collision detection (AABB sweep)
3. Narrow-phase collision detection (GJK/SAT)
4. Contact generation (collision manifolds)
5. Solve constraints (joints, contacts, motors)
6. Integrate velocities → positions
7. Update AABBs for next frame's broad-phase
```

---

## PART 2: COLLISION DETECTION

### Broad Phase: Sweep and Prune (SAP)

Sort objects along each axis. Overlapping intervals = potential collisions.
Complexity: O(n log n) sort, O(n + k) overlap detection where k = number of pairs.

```
For 10,000 objects: ~45,000 AABB comparisons (vs 50,000,000 brute force)
Use 3 sorted lists (X, Y, Z axes) with incremental insertion sort
  (nearly sorted each frame → O(n) amortized)
```

### Narrow Phase: GJK + EPA

GJK (Gilbert-Johnson-Keerthi): determines if two convex shapes overlap.
EPA (Expanding Polytope Algorithm): finds penetration depth and contact normal.

```cpp
// Simplified GJK overview (the actual algorithm needs ~100 lines)
// Key concept: Minkowski Difference
// If the Minkowski difference of two shapes contains the origin, they overlap.

struct CollisionResult {
    bool colliding;
    Vec3f contactNormal;     // Direction to push objects apart
    float penetrationDepth;  // How far they overlap
    Vec3f contactPoint;      // Where they touch
};

// For convex hulls: GJK converges in 3-20 iterations typically
// For non-convex: decompose into convex pieces first
//   or use mesh-based collision with triangle-triangle tests
```

### Shape Types and When to Use Them

| Shape | CPU Cost | Use Case | Triangle Count |
|---|---|---|---|
| **Sphere** | Cheapest | Projectiles, small objects, first-pass | 0 |
| **AABB** | Very cheap | Broad phase, grid queries | 0 |
| **OBB** | Cheap | Oriented objects, ships (rough) | 0 |
| **Capsule** | Cheap | Characters, limbs, cylindrical objects | 0 |
| **Convex Hull** | Medium | Detailed ship collision, props | 8-64 verts |
| **Compound** | Medium | Complex objects = union of primitives | varies |
| **Triangle Mesh** | Expensive | Terrain, station interiors | 100-10000 |
| **Height Field** | Medium | Large terrain areas | implicit |

### Collision Layers

```cpp
enum CollisionLayer : uint16_t {
    LAYER_STATIC       = 1 << 0,   // Terrain, stations, asteroids
    LAYER_DYNAMIC      = 1 << 1,   // Ships, crates, debris
    LAYER_PLAYER       = 1 << 2,   // Player ship/character
    LAYER_NPC          = 1 << 3,   // NPC ships/characters
    LAYER_PROJECTILE   = 1 << 4,   // Bullets, missiles, beams
    LAYER_TRIGGER      = 1 << 5,   // Invisible trigger volumes
    LAYER_DEBRIS       = 1 << 6,   // Explosion debris (reduced checks)
    LAYER_CHARACTER    = 1 << 7,   // On-foot characters
    LAYER_RAGDOLL      = 1 << 8,   // Ragdoll physics bodies
    LAYER_SENSOR       = 1 << 9,   // Detection volumes (no physical response)
};

// Collision matrix: which layers collide with which
// STATIC × DYNAMIC:    yes (objects land on ground)
// STATIC × PROJECTILE: yes (bullets hit walls)
// DEBRIS × DEBRIS:     NO  (performance: debris ignores other debris)
// TRIGGER × TRIGGER:   NO  (triggers don't interact with each other)
// SENSOR × anything:   overlap query only, no physics response
```

---

## PART 3: CONSTRAINT SOLVER

### Sequential Impulse Solver (Erin Catto's method, used by Box2D/Bullet/Jolt)

```
For each contact pair:
  1. Compute relative velocity at contact point
  2. Compute impulse to resolve penetration (Baumgarte stabilization)
  3. Apply friction impulse (Coulomb friction: lateral force ≤ μ × normal force)
  4. Clamp accumulated impulse ≥ 0 (contacts can only push, not pull)
  5. Iterate 4-8 times for convergence (more = more stable stacking)

Key parameters:
  Solver iterations: 4 (minimum), 8 (default), 16 (very stable stacking)
  Baumgarte factor: 0.1-0.3 (how aggressively to resolve penetration)
  Slop: 0.005m (allowable penetration before correction, prevents jitter)
  Restitution: 0.0 (clay) to 1.0 (perfectly elastic bounce)
  Friction: 0.0 (ice) to 1.0 (rubber)
```

### Common Friction Values

| Material Pair | Static μ | Dynamic μ | Restitution |
|---|---|---|---|
| **Metal on metal** | 0.6 | 0.4 | 0.3 |
| **Metal on concrete** | 0.5 | 0.35 | 0.2 |
| **Rubber on concrete** | 0.8 | 0.7 | 0.5 |
| **Boot on deck plate** | 0.7 | 0.5 | 0.1 |
| **Crate on floor** | 0.5 | 0.3 | 0.1 |
| **Ice on anything** | 0.1 | 0.05 | 0.05 |
| **Character on ground** | 0.8 | 0.6 | 0.0 |

### Joint Types

```cpp
enum JointType {
    FIXED,          // Welded together (no relative motion)
    HINGE,          // Rotate around one axis (doors, elbows)
    BALL_SOCKET,    // Rotate around all axes (shoulders, hips)
    SLIDER,         // Translate along one axis (pistons, drawers)
    DISTANCE,       // Maintain fixed distance (ropes, chains)
    CONE_TWIST,     // Ball socket with angular limits (ragdoll)
    SPRING_DAMPER,  // Spring force between two bodies
    MOTOR,          // Actively drives rotation/translation (wheels, turrets)
};

// For ragdoll: 15 cone-twist joints with limits from Creature Anatomy Bible
// For vehicles: 4 hinge joints with motors (wheels) + spring-dampers (suspension)
// For ship turrets: 2 hinge joints (yaw + pitch) with motor and angle limits
```

---

## PART 4: PERFORMANCE BUDGETS

| Metric | Budget | Notes |
|---|---|---|
| **Physics timestep** | 16.67ms (60Hz) | Fixed, never skip |
| **Broad phase** | <1ms | SAP with incremental sort |
| **Narrow phase** | <3ms | GJK/EPA on candidate pairs |
| **Solver** | <4ms | 8 iterations × all contacts |
| **Total physics** | <8ms per step | Leaves 8ms for everything else |
| **Max rigid bodies** | 2000 active | Sleeping bodies cost nearly zero |
| **Max contacts** | 10,000 per step | Typical: 500-2000 |
| **Max joints** | 500 active | Ragdolls: 15 joints each |

### Sleep System
Objects that haven't moved for 2 seconds enter "sleep" state.
Sleeping objects: zero CPU cost. Wake when contacted by active object.

```
Sleep threshold: linear velocity < 0.05 m/s AND angular velocity < 0.05 rad/s
Sleep timer: 2.0 seconds below threshold
Wake conditions: collision with active body, explosion within radius, player interaction
Typical scene: 2000 bodies, 200 active, 1800 sleeping = physics cost of 200 bodies
```

---

## PART 5: SPACE-SPECIFIC PHYSICS

### Zero-G Physics
```
In space/zero-G zones:
- Gravity = 0 (no sleep due to resting on surfaces — objects float forever)
- No air drag (objects maintain velocity indefinitely)
- Angular momentum conserved (spinning objects never stop)
- Collisions are perfectly Newtonian (coefficient of restitution matters a lot)
- Characters use mag-boots (binary: attached or floating)
  Mag-boot force: 500N per boot (enough to hold 50kg person at 1G-equivalent)
  Detach threshold: external force > 1000N (explosion, etc.)
```

### Gravity Zones
```
// Smooth transition between gravity zones
float ComputeGravity(const Vec3d& position, const GravityZone& zone) {
    double dist = (position - zone.center).Length();
    if (dist > zone.outerRadius) return 0; // Outside zone
    if (dist < zone.innerRadius) return zone.gravity; // Full gravity
    // Linear blend in transition region
    float t = (dist - zone.innerRadius) / (zone.outerRadius - zone.innerRadius);
    return zone.gravity * (1.0f - t);
}

// Station gravity: artificial, generated by rotation or gravity plates
// Planet surface: real gravity = GM/r²
// Ship interior: artificial (0.8-1.0G on large ships, 0G on small fighters)
// Space: 0G except near massive bodies
```

---

## PART 6: CURRICULUM STEP UPDATES

**Step 121-130 (Physics foundation)**: Fixed timestep accumulator pattern.
Symplectic Euler integration. Gravity as constant downward force initially.

**Step 131-140 (Collision detection)**: Implement sphere-sphere and AABB-AABB first.
Add collision layers. Broad phase with spatial grid (simple, upgrade to SAP later).

**Step 141-150 (Collision response)**: Impulse-based resolution. Compute contact normal
and penetration depth. Apply impulse proportional to inverse mass. Restitution and friction.

**Step 151-160 (Character controller)**: Capsule shape. Sweep test for movement.
Step detection (can climb 0.3m steps). Slope limit (45° maximum walkable).
Ground detection ray. Separate from rigid body system (kinematic, not dynamic).

**Step 161-170 (Rigid body dynamics)**: Full 3×3 inertia tensor. Torque from off-center forces.
Gyroscopic precession term. Quaternion integration for orientation.

**Step 171-180 (Constraints)**: Fixed joint (welding). Hinge joint (doors).
Distance constraint (tethers). Sequential impulse solver, 8 iterations.

**Step 1301-1340 (Physics depth pass)**: Replace homebrew with Jolt Physics library.
Jolt: MIT license, modern C++17, SIMD optimized, 50K+ bodies, deterministic.
Integration: Jolt handles broad+narrow+solver; our code handles game logic.
Add: compound shapes from convex decomposition, ragdoll system, vehicle constraints.

**Step 1341-1380 (Destruction depth)**: Voronoi fracture for destructible objects.
Pre-fracture meshes into ~8-32 convex pieces. On impact above threshold: break apart.
Each piece becomes a rigid body. Chain reactions for explosive objects.
